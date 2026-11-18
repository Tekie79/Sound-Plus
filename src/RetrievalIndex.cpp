#include "RetrievalIndex.h"
#include "RegionRanking.h"
#include "ModelPack.h"
#include "Performance.h"
#include <juce_cryptography/juce_cryptography.h>
#include <Accelerate/Accelerate.h>
#include <cmath>
#include <pthread/qos.h>
namespace sp
{
namespace {struct DeltaOverflow {};struct BuildPaused {};}
RetrievalIndex::RetrievalIndex(juce::File path,std::function<void(const IndexStats &)> callback,std::function<bool()> pause)
    :database(std::move(path)),directory(database.getParentDirectory().getChildFile("retrieval")),changed(std::move(callback)),pauseRequested(std::move(pause))
{ builder=std::thread([this]{run();}); }
RetrievalIndex::~RetrievalIndex(){stopping=true;wake.notify_all();if(builder.joinable())builder.join();}
IndexStats RetrievalIndex::stats(){std::lock_guard lock(mutex);return state;}
void RetrievalIndex::publish(std::shared_ptr<Snapshot> next,bool reused,double elapsed)
{
    IndexStats copy;
    {
        std::lock_guard lock(mutex);active=std::move(next);state.ready=true;state.building=false;state.paused=false;state.reused=reused;
        state.regions=active->count;state.bytes=active->file.getSize();
        state.sequence=active->state.sequence;
        if(reused)state.openMs=elapsed;else state.buildMs=elapsed;
        state.status="Sound index ready";copy=state;
    }
    if(changed)changed(copy);
}
std::shared_ptr<RetrievalIndex::Snapshot> RetrievalIndex::load(Catalog &db)
{
    auto meta=directory.getChildFile("current.json");if(!meta.existsAsFile() || meta.getSize()>128*1024*1024)return {};
    auto json=juce::JSON::parse(meta);auto current=db.indexState();
    auto metadataHash=json["metadataSha256"].toString();
    if(auto *object=json.getDynamicObject())object->removeProperty("metadataSha256");else return {};
    auto metadataText=juce::JSON::toString(json,true);
    if(juce::SHA256(metadataText.toRawUTF8(),metadataText.getNumBytesAsUTF8()).toHexString()!=metadataHash)return {};
    if((int)json["format"]!=2 || (int)json["recordBytes"]!=(int)sizeof(PackedRegion) ||
        json["catalog"].toString()!=current.catalog || json["model"].toString()!=modelId() || json["preprocessing"].toString()!=preprocessingId())return {};
    auto next=std::make_shared<Snapshot>();next->state.catalog=current.catalog;next->state.sequence=(int64_t)json["sequence"];
    if(next->state.sequence>current.sequence || next->state.sequence<current.oldest-1)return {};
    auto name=json["file"].toString();if(juce::File::createFileWithoutCheckingPath(name).getFileName()!=name || !name.startsWith("regions-"))return {};
    next->file=directory.getChildFile(name);next->count=(int64_t)json["count"];
    if(next->count<0 || next->count>1000000 || next->file.getSize()!=next->count*(int64_t)sizeof(PackedRegion))return {};
    auto stream=next->file.createInputStream();if(!stream || juce::SHA256(*stream).toHexString()!=json["sha256"].toString())return {};
    auto *blocks=json["blocks"].getArray();if(!blocks || blocks->size()>1000000)return {};
    int64_t cursor=0;
    for(auto &item:*blocks)
    {
        auto *v=item.getArray();if(!v || v->size()!=5)return {};
        Block b{(*v)[0].toString(),(*v)[1].toString(),(int64_t)(*v)[2],(int64_t)(*v)[3],(int64_t)(*v)[4]};
        if(b.asset.isEmpty() || b.root.isEmpty() || b.revision<1 || b.first!=cursor || b.count<1 || b.count>next->count-cursor)return {};
        cursor+=b.count;next->blocks.push_back(std::move(b));
    }
    if(cursor!=next->count)return {};
    if(next->count)
    {
        next->mapped=std::make_unique<juce::MemoryMappedFile>(next->file,juce::MemoryMappedFile::readOnly);
        if(!next->mapped->getData() || next->mapped->getSize()!=(size_t)next->file.getSize())return {};
    }
    return next;
}
std::shared_ptr<RetrievalIndex::Snapshot> RetrievalIndex::build(Catalog &db)
{
    directory.createDirectory();auto next=std::make_shared<Snapshot>();
    db.exec("BEGIN");
    try
    {
        next->state=db.indexState();
        next->file=directory.getChildFile("regions-"+juce::Uuid().toString()+".bin");
        juce::TemporaryFile temporary(next->file);auto out=temporary.getFile().createOutputStream();
        if(!out)throw std::runtime_error("Cannot create search-index cache; exact retrieval remains available");
        db.visitRegions(modelId(),preprocessingId(),{},[&](const StoredRegion &row) {
            if(stopping)throw QueryCancelled();
            if((next->count%1024)==0 && pauseRequested && pauseRequested())throw BuildPaused{};
            if(next->count>=1000000)throw std::runtime_error("Cache capacity is one million regions; using the exact database path");
            if(next->blocks.empty() || next->blocks.back().asset!=row.asset)
                next->blocks.push_back({row.asset,row.root,row.revision,next->count,0});
            PackedRegion value{row.first,row.last,row.vector};
            double norm=0;for(float f:value.vector) {if(!std::isfinite(f))throw std::runtime_error("Nonfinite authoritative embedding");norm+=f*f;}
            if(norm<1e-20 || value.first<0 || value.last<=value.first)throw std::runtime_error("Invalid authoritative embedding/range");
            if(!out->write(&value,sizeof(value)))throw std::runtime_error("Search cache write failed. Check free disk space.");
            ++next->count;++next->blocks.back().count;
        });
        db.exec("COMMIT");out->flush();out.reset();if(stopping)throw QueryCancelled();
        auto input=temporary.getFile().createInputStream();if(!input)throw std::runtime_error("Cannot validate search cache");
        auto checksum=juce::SHA256(*input).toHexString();input.reset();
        if(temporary.getFile().getSize()!=next->count*(int64_t)sizeof(PackedRegion))throw std::runtime_error("Incomplete search cache");
        if(stopping)throw QueryCancelled();
        if(!temporary.overwriteTargetFileWithTemporary())throw std::runtime_error("Cannot publish search-cache data");
        if(next->count)
        {
            next->mapped=std::make_unique<juce::MemoryMappedFile>(next->file,juce::MemoryMappedFile::readOnly);
            if(!next->mapped->getData())throw std::runtime_error("Cannot map search-cache data");
        }
        auto *o=new juce::DynamicObject();juce::var json(o);
        o->setProperty("format",2);o->setProperty("recordBytes",(int)sizeof(PackedRegion));o->setProperty("catalog",next->state.catalog);
        o->setProperty("sequence",(juce::int64)next->state.sequence);o->setProperty("model",modelId());o->setProperty("preprocessing",preprocessingId());
        o->setProperty("file",next->file.getFileName());o->setProperty("count",(juce::int64)next->count);o->setProperty("sha256",checksum);
        juce::Array<juce::var> blocks;
        for(auto &b:next->blocks) blocks.add(juce::var(juce::Array<juce::var>{b.asset,b.root,(juce::int64)b.revision,(juce::int64)b.first,(juce::int64)b.count}));
        o->setProperty("blocks",blocks);auto metadataText=juce::JSON::toString(json,true);
        o->setProperty("metadataSha256",juce::SHA256(metadataText.toRawUTF8(),metadataText.getNumBytesAsUTF8()).toHexString());
        juce::TemporaryFile meta(directory.getChildFile("current.json"));
        if(!meta.getFile().replaceWithText(juce::JSON::toString(json,true)) || !meta.overwriteTargetFileWithTemporary())
            throw std::runtime_error("Cannot publish validated search generation");
        // Retain the current and previous data files. Existing query mappings remain owned until released.
        juce::File previous;{std::lock_guard lock(mutex);if(active)previous=active->file;}
        for(auto &f:directory.findChildFiles(juce::File::findFiles,false,"regions-*.bin"))
            if(f!=next->file && f!=previous)f.deleteFile();
        return next;
    }
    catch(...){db.rollback();throw;}
}
void RetrievalIndex::run()
{
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
    try
    {
        Catalog db(database,true);bool opened=false;
        double lastBuild=0,lastChange=monotonicMs();int64_t observed=-1;
        while(!stopping)
        {
            if(pauseRequested && pauseRequested())
            {
                IndexStats copy;{std::lock_guard lock(mutex);state.paused=true;state.building=false;state.status="Sound index paused";copy=state;}
                if(changed)changed(copy);std::unique_lock lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping.load() || !pauseRequested();});continue;
            }
            if(!opened)
            {
                IndexStats copy;{std::lock_guard lock(mutex);state.paused=false;state.building=true;state.status="Opening sound index";copy=state;}
                if(changed)changed(copy);auto started=monotonicMs();
                if(auto saved=load(db))publish(saved,true,monotonicMs()-started);
                opened=true;
            }
            auto catalog=db.indexState();std::shared_ptr<Snapshot> previous;
            if(observed!=catalog.sequence){observed=catalog.sequence;lastChange=monotonicMs();}
            {std::lock_guard lock(mutex);previous=active;}
            bool due=previous ? catalog.sequence!=previous->state.sequence &&
                (catalog.sequence-previous->state.sequence>=4096 || monotonicMs()-lastChange>=1500) :
                lastBuild==0 || monotonicMs()-lastBuild>=30000;
            if(due && monotonicMs()-lastBuild>=500)
            {
                IndexStats copy;{std::lock_guard lock(mutex);state.paused=false;state.building=true;state.status="Preparing sound index; search stays available";copy=state;}
                if(changed)changed(copy);auto began=monotonicMs();
                bool pausedBuild=false;
                try {auto next=build(db);if(!stopping)publish(next,false,monotonicMs()-began);}
                catch(const BuildPaused &){pausedBuild=true;IndexStats copy;{std::lock_guard lock(mutex);state.building=false;state.paused=true;state.status="Sound index paused";copy=state;}if(changed)changed(copy);}
                catch(const QueryCancelled &){if(stopping)return;}
                catch(const std::exception &e){IndexStats copy;{std::lock_guard lock(mutex);state.status=e.what();state.building=false;copy=state;}if(changed)changed(copy);}
                lastBuild=pausedBuild ? 0 : monotonicMs();
            }
            std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(500),[&]{return stopping.load();});
        }
    }
    catch(const std::exception &e){IndexStats copy;{std::lock_guard lock(mutex);state.status=e.what();state.building=false;copy=state;}if(changed)changed(copy);}
}
std::vector<Asset> RetrievalIndex::search(Catalog &db,const std::vector<float> &query,const juce::String &root,
    const juce::String &excluded,int limit,RetrievalMetrics *metrics,const std::function<bool()> &cancelled)
{
    auto began=monotonicMs();std::shared_ptr<Snapshot> base;{std::lock_guard lock(mutex);base=active;}
    auto fallback=[&]{return db.semanticSearch(query,modelId(),preprocessingId(),root,limit,metrics,excluded);};
    if(!base)return fallback();
    if(query.size()!=512)throw std::runtime_error("Invalid query vector dimensions");
    db.exec("BEGIN");
    try
    {
        auto live=db.indexState();
        if(live.catalog!=base->state.catalog || base->state.sequence>live.sequence || base->state.sequence<live.oldest-1)
        {db.exec("COMMIT");return fallback();}
        if(deltaBase.lock()!=base){delta.clear();deltaBase=base;deltaSequence=base->state.sequence;}
        if(deltaSequence<live.oldest-1){db.exec("COMMIT");return fallback();}
        size_t deltaRows=0;for(auto &[id,rows]:delta)deltaRows+=rows.size();
        for(auto &id:db.indexChanges(deltaSequence))
        {
            if(cancelled())throw QueryCancelled();auto &rows=delta[id];deltaRows-=rows.size();rows.clear();
            db.visitRegions(modelId(),preprocessingId(),id,[&](const StoredRegion &r){if(deltaRows>=32768)throw DeltaOverflow{};rows.push_back(r);++deltaRows;});
        }
        deltaSequence=live.sequence;db.exec("COMMIT");
    }
    catch(const DeltaOverflow &){db.rollback();delta.clear();deltaBase.reset();return fallback();}
    catch(...){db.rollback();throw;}
    size_t deltaCount=0;for(auto &[id,rows]:delta)deltaCount+=rows.size();
    if(deltaCount>32768)return fallback();
    RetrievalMetrics measured;
    std::vector<const Block *> eligible;
    for(auto &b:base->blocks)
        if((root.isEmpty() || root==b.root) && b.asset!=excluded && !delta.contains(b.asset))eligible.push_back(&b);
    measured.fetchFilterMs=monotonicMs()-began;
    RegionRanking ranking(limit);
    auto score=[&](const juce::String &id,int64_t revision,int64_t first,int64_t last,const float *v) {
        float similarity=0;vDSP_dotpr(query.data(),1,v,1,&similarity,512);++measured.vectors;
        ranking.add(id,revision,first,last,similarity);
    };
    auto scoreBegan=monotonicMs();auto *data=base->mapped ? (const PackedRegion *)base->mapped->getData() : nullptr;
    for(auto *b:eligible)
    {
        if(cancelled())throw QueryCancelled();
        for(int64_t i=b->first;i<b->first+b->count;++i)
        {if((i%1024)==0 && cancelled())throw QueryCancelled();const auto &v=data[i];score(b->asset,b->revision,v.first,v.last,v.vector.data());}
    }
    for(auto &[id,rows]:delta)for(auto &r:rows)
        if((root.isEmpty() || r.root==root) && r.asset!=excluded){if(cancelled())throw QueryCancelled();score(r.asset,r.revision,r.first,r.last,r.vector.data());}
    measured.vectorScoreMs=monotonicMs()-scoreBegan;auto grouping=monotonicMs();
    auto result=ranking.finish(db,root);
    measured.rankGroupMs=monotonicMs()-grouping;if(metrics)*metrics=measured;return result;
}
}
