#include "SimilarTests.h"
#include "LibraryService.h"
#include "RetrievalIndex.h"
#include "AudioIPC.h"
#include "AudioStream.h"
#include "Performance.h"
#include "Waveform.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <iostream>
#include <thread>
namespace
{
void check(bool value,const char *text){if(!value)throw std::runtime_error(text);}
template<class F>void wait(F condition,const char *message,int ms=30000)
{auto end=sp::monotonicMs()+ms;while(!condition()){if(sp::monotonicMs()>end)throw std::runtime_error(message);std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
void multichannel(const juce::File &file,bool opposed)
{
    juce::WavAudioFormat format;auto out=file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(out.release(),48000,4,24,{},0));
    check(writer!=nullptr,"Multichannel fixture writer");juce::AudioBuffer<float> samples(4,48000);samples.clear();
    for(int i=0;i<48000;++i){float v=.3f*std::sin((float)(2*juce::MathConstants<double>::pi*317*i/48000));
        if(opposed){samples.setSample(0,i,v);samples.setSample(1,i,-v);}else samples.setSample(2,i,v);}
    check(writer->writeFromAudioSampleBuffer(samples,0,48000),"Multichannel signal written");
}
double energy(const juce::File &file,int pair)
{
    sp::PcmStream pcm(file,48000,0,-1,pair);juce::AudioBuffer<float> b(2,512);float gain=1;double sum=0;int64_t count=0;
    while(!pcm.ended()){pcm.fill();b.clear();int n=pcm.read(b.getArrayOfWritePointers(),2,512,gain,1);count+=n;for(int i=0;i<n;++i)sum+=b.getSample(0,i)*b.getSample(0,i);}
    return std::sqrt(sum/count);
}
sp::AnalysisUpdate vectorFor(const sp::Asset &a,int n)
{
    sp::AnalysisUpdate u;u.asset=a;u.model=sp::modelId();u.preprocessing=sp::preprocessingId();u.start=n*4800;u.end=u.start+4800;
    u.nextStart=u.end;u.done=n==9;u.embedding.assign(512,0);u.embedding[(size_t)(n%3)]=1;return u;
}
void equivalent(const std::vector<sp::Asset> &exact,const std::vector<sp::Asset> &cached)
{
    check(exact.size()==cached.size(),"Exact/cached grouped asset counts agree");
    for(size_t i=0;i<exact.size();++i){check(exact[i].id==cached[i].id,"Exact/cached asset ordering agrees");
        check(exact[i].matches.size()==cached[i].matches.size(),"Exact/cached moments agree");
        for(size_t j=0;j<exact[i].matches.size();++j){auto &a=exact[i].matches[j];auto &b=cached[i].matches[j];
            check(a.startSample==b.startSample && a.endSample==b.endSample && std::abs(a.similarity-b.similarity)<1e-6,"Exact/cached source ranges and cosine scores agree");}}
}
}
void runSimilarIntegrity(const juce::File &base,const juce::File &executable)
{
    using Range=std::pair<int64_t,int64_t>;
    check(sp::markRangeAtPlayhead(0,48000,12345,48000,true)==Range{12345,48000},"IN marks the exact source playhead");
    check(sp::markRangeAtPlayhead(12345,48000,23456,48000,false)==Range{12345,23456},"OUT is the exclusive source playhead");
    check(sp::markRangeAtPlayhead(1000,2000,3000,48000,true)==Range{3000,48000},"IN after old OUT restores the recording end");
    check(sp::markRangeAtPlayhead(1000,2000,500,48000,false)==Range{0,500},"OUT before old IN restores the recording beginning");
    check(sp::markRangeAtPlayhead(0,48000,48000,48000,true)==Range{47999,48000},"IN at the end retains a sample");
    check(sp::markRangeAtPlayhead(1000,2000,0,48000,false)==Range{0,1},"OUT at zero retains a sample");
    check(sp::markRangeAtPlayhead(0,0,0,0,true)==Range{0,0},"Empty waveforms cannot create an audition range");
    check(base.getFileName()=="similar-test-output","Use isolated similar-test-output");base.deleteRecursively();base.createDirectory();
    auto third=base.getChildFile("third-channel.wav"),opposed=base.getChildFile("opposed.wav");multichannel(third,false);multichannel(opposed,true);
    check(energy(third,0)==0,"Legacy first pair cannot hear third-channel content");
    check(energy(third,1)>.2,"Second pair auditions channel three");
    check(std::abs(energy(third,-1)-.3/std::sqrt(2)/4)<1e-5,"Analysis mix auditions all channels at the encoder's arithmetic gain");
    check(energy(opposed,0)>.2 && energy(opposed,-1)<1e-6,"Opposing polarity cancels in the explicitly supported analysis mix");
    {
        sp::AudioClient audio(executable);sp::PlaybackRegion region;region.startSample=0;region.endSample=48000;region.auditionPair=1;
        auto generation=audio.playRegion("third-channel",third,region);
        wait([&]{auto s=audio.status();return (uint64_t)(juce::int64)s["playGeneration"]==generation && (int)s["auditionPair"]==1 && (double)s["peak"]>.05;},"Native audio helper auditions the second stereo pair");
        region.auditionPair=-1;generation=audio.playRegion("analysis-mix",third,region);
        wait([&]{auto s=audio.status();return (uint64_t)(juce::int64)s["playGeneration"]==generation && (int)s["auditionPair"]==-1 && (double)s["peak"]>.02;},"Native audio helper auditions the all-channel analysis mix");audio.stop();
        auto stopped=audio.status();check(!(bool)stopped["playing"] && !(bool)stopped["pendingPlayback"],"Stop intent is immediately visible to waveform interaction");
    }
    auto windows=sp::referenceWindows(123,123+48000*62+24000,48000);
    check(windows.size()==12 && windows.front().first==123 && windows.back().second==123+48000*62+24000,"Long reference evenly spans exact selected bounds with at most twelve windows");
    check(sp::referenceWindows(37,4138,44100).size()==1,"Short reference keeps its actual selected samples");
    auto sparse=sp::referenceWindows(123,123+48000*300,48000);
    check(sparse.size()==12 && sparse[1].first-sparse[0].first>48000*5 && sparse.back().second==123+48000*300,"Very long references use a bounded even sample across the chosen interval");
    auto dbFile=base.getChildFile("library.sqlite");sp::Catalog writer(dbFile);auto root=writer.addRootRecord(base);
    auto secondRoot=writer.addRootRecord(base.getChildFile("other"));sp::FileStamp stamp;sp::fileStamp(third,stamp);
    juce::File(secondRoot.path).createDirectory();
    std::vector<sp::Asset> assets;
    for(int i=0;i<32;++i){auto r=i<16 ? root : secondRoot;auto name="source-"+juce::String(i)+".wav";
        auto file=juce::File(r.path).getChildFile(name);check(third.copyFileTo(file),"Distinct recording fixture");sp::fileStamp(file,stamp);
        writer.discover(r,name,stamp,1);auto a=writer.existing(r.id,name);writer.metadata(a,1,4,48000,{},48000);a=writer.asset(a.id);assets.push_back(a);
        writer.begin();for(int n=0;n<10;++n)writer.commitAnalysis(vectorFor(a,n));writer.commit();}
    auto dense=*std::min_element(assets.begin(),assets.end(),[](const auto &a,const auto &b){return a.id<b.id;});
    writer.begin();for(int i=0;i<450;++i){auto u=vectorFor(dense,0);u.start=20000+i;u.end=24000+i;writer.commitAnalysis(u);}writer.commit();
    std::vector<float> q(512,0);q[0]=1;auto never=[] {return false;};
    {
        sp::RetrievalIndex cache(dbFile);wait([&]{return cache.stats().ready;},"Exact cache builds");sp::Catalog read(dbFile,true);
        check(read.semanticSearch(q,sp::modelId(),sp::preprocessingId()).size()==32,"Dense windows from one recording cannot hide eligible alternative recordings");
        for(auto filter:{juce::String(),root.id,secondRoot.id,juce::String("empty-root")})
            equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId(),filter),cache.search(read,q,filter,{},100,nullptr,never));
        equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId(),{},100,nullptr,assets[0].id),cache.search(read,q,{},assets[0].id,100,nullptr,never));
        writer.begin();auto added=vectorFor(assets[0],0);added.embedding[0]=0;added.embedding[1]=1;writer.commitAnalysis(added);writer.commit();
        equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId()),cache.search(read,q,{},{},100,nullptr,never));
        writer.exec(("UPDATE assets SET revision=revision+1 WHERE id='"+assets[1].id+"'").toRawUTF8());
        equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId()),cache.search(read,q,{},{},100,nullptr,never));
        writer.exec(("UPDATE roots SET online=0 WHERE id='"+root.id+"'").toRawUTF8());
        equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId()),cache.search(read,q,{},{},100,nullptr,never));
        bool cancelled=false;try{cache.search(read,q,{},{},100,nullptr,[]{return true;});}catch(const sp::QueryCancelled &){cancelled=true;}
        check(cancelled,"Cached scan honours cancellation");
        writer.begin();for(int i=0;i<8300;++i)writer.commitAnalysis(vectorFor(assets[2],i%10));writer.commit();
        equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId()),cache.search(read,q,{},{},100,nullptr,never));
        wait([&]{auto meta=juce::JSON::parse(base.getChildFile("retrieval/current.json"));return (int64_t)meta["sequence"]==writer.indexState().sequence;},"Updated generation atomically published",15000);
    }
    {sp::RetrievalIndex cache(dbFile);wait([&]{return cache.stats().ready;},"Cached index reopens");check(cache.stats().reused,"Unchanged valid index reused");}
    auto meta=juce::JSON::parse(base.getChildFile("retrieval/current.json"));auto data=base.getChildFile("retrieval").getChildFile(meta["file"].toString());
    {juce::FileOutputStream out(data);out.setPosition(5);out.writeByte(71);}
    {sp::RetrievalIndex cache(dbFile);wait([&]{return cache.stats().ready;},"Corrupt derived cache rebuilds");check(!cache.stats().reused,"Checksum rejects corrupted cached generation");}
    {
        auto metadataFile=base.getChildFile("retrieval/current.json");auto metadata=juce::JSON::parse(metadataFile);
        auto *blocks=metadata["blocks"].getArray();check(blocks && !blocks->isEmpty(),"Cached block metadata present");
        auto largest=std::max_element(assets.begin(),assets.end(),[](const auto &a,const auto &b){return a.id<b.id;});
        auto *fields=blocks->getReference(0).getArray();check(fields && (*fields)[0].toString()!=largest->id,"Distinct source association selected for tampering");
        fields->set(0,largest->id);metadataFile.replaceWithText(juce::JSON::toString(metadata,true));
        sp::RetrievalIndex cache(dbFile);wait([&]{return cache.stats().ready;},"Corrupt vector-to-source metadata rebuilds");
        check(!cache.stats().reused,"Metadata checksum protects vector-to-source associations");
    }
    {
        std::atomic<bool> paused{true};sp::RetrievalIndex cache(dbFile,{},[&]{return paused.load();});
        wait([&]{return cache.stats().paused;},"Search-cache work obeys background pause");
        sp::Catalog read(dbFile,true);equivalent(read.semanticSearch(q,sp::modelId(),sp::preprocessingId()),cache.search(read,q,{},{},100,nullptr,never));
        paused=false;wait([&]{return cache.stats().ready;},"Search cache resumes and reuses a valid generation");
    }
    std::cout<<"PASS exact cached retrieval, live deltas, filters, exclusions, invalidation, log gap recovery, cache reuse/integrity, channels and reference windows\n";
}
void runSimilarWorkflow(const juce::File &executable,const juce::File &model,const juce::File &corpus,const juce::File &base)
{
    check(base.getFileName()=="similar-workflow-output","Use isolated similar-workflow-output");base.deleteRecursively();base.createDirectory();
    auto media=base.getChildFile("media");check(corpus.copyDirectoryTo(media),"Copy attributed real effects");
    auto dbFile=base.getChildFile("library.sqlite");sp::AudioReference ref;juce::Array<juce::var> cases;
    {
        sp::LibraryService library(dbFile,executable,model);library.addFolder(media);
        wait([&]{return library.snapshot().semantic.coverage.complete==43;},"Real reference corpus analyzed",120000);
        sp::Catalog read(dbFile,true);auto source=read.search("clip-029.wav")[0];ref.source=source;ref.endSample=source.samples;
        auto query=[&](sp::AudioReference reference){auto id=library.findSimilar(reference);wait([&]{return library.snapshot().semanticCompletedRequest==id;},"Find Similar publishes current generation",30000);return library.snapshot();};
        auto result=query(ref);check(result.reference.error.isEmpty() && !result.results.empty(),"Indexed reference returns alternatives");
        for(auto &a:result.results)check(a.id!=source.id,"Default excludes source recording before grouping");
        ref.includeSource=true;result=query(ref);bool self=false;for(auto &a:result.results)self|=a.id==source.id;check(self,"Include source retrieves its other moments");
        ref.includeSource=false;ref.startSample=4410;ref.endSample=source.samples-2205;result=query(ref);
        check(!result.reference.cacheHit && result.reference.error.isEmpty(),"Edited interval encodes selected audio, not the previous whole-file vector");
        auto uncachedMs=result.reference.encodingMs;result=query(ref);check(result.reference.cacheHit,"Exact interval cache reused");
        auto *timing=new juce::DynamicObject();timing->setProperty("uncachedReferenceEncodingMs",uncachedMs);timing->setProperty("cachedReferenceEncodingMs",result.reference.encodingMs);timing->setProperty("retrievalMs",result.reference.retrievalMs);cases.add(juce::var(timing));
        auto external=base.getChildFile("external-door.wav");check(media.getChildFile("clip-041.wav").copyFileTo(external),"External long reference copy");
        sp::AudioReference longRef;longRef.external=true;longRef.source.path=external.getFullPathName();result=query(longRef);
        check(result.reference.totalWindows==11 && result.reference.completedWindows==11 && result.reference.error.isEmpty(),"Multi-window external reference finishes with progress");
        check(read.count()==43,"External reference is temporary, does not add assets");
        for(int i=0;i<100;++i){auto fast=ref;fast.startSample=i*10;library.findSimilar(fast);}
        result=query(ref);check(result.reference.reference.startSample==ref.startSample,"Newest rapid reference/range wins");
        auto empty=base.getChildFile("empty-source");empty.createDirectory();library.addFolder(empty);wait([&]{return library.snapshot().roots.size()==2;},"Empty source available");
        juce::String emptyId;for(auto &r:library.snapshot().roots)if(r.path==empty.getFullPathName())emptyId=r.id;
        auto id=library.findSimilar(ref,emptyId);wait([&]{return library.snapshot().semanticCompletedRequest==id;},"Empty eligible subset completes");check(library.snapshot().results.empty(),"Source filtering applied before final grouping");
        longRef.startSample=123;longRef.endSample=60*44100-123;id=library.findSimilar(longRef);
        wait([&]{auto s=library.snapshot();return s.reference.completedWindows>0 || !s.reference.error.isEmpty();},"Reference analysis begins");
        {juce::FileOutputStream append(external);append.setPosition(external.getSize());append.writeByte(0);}
        wait([&]{return library.snapshot().semanticCompletedRequest==id;},"Changed reference completes with actionable failure");
        check(library.snapshot().reference.error.containsIgnoreCase("chang"),"Source changed during inference is rejected");
        longRef.startSample=0;longRef.endSample=-1;result=query(longRef);
        check(!result.reference.cacheHit && result.reference.error.isEmpty(),"Changed identity cannot reuse an older reference vector");
        auto emptyAudio=base.getChildFile("empty.wav");emptyAudio.create();sp::AudioReference invalid;invalid.external=true;invalid.source.path=emptyAudio.getFullPathName();
        result=query(invalid);check(result.reference.error.containsIgnoreCase("empty"),"Empty/unreadable external reference gives an actionable error");
        auto relocated=ref;relocated.source.path=base.getChildFile("old-location.wav").getFullPathName();result=query(relocated);
        check(result.reference.cacheHit && result.reference.error.isEmpty() && result.reference.reference.source.path==ref.source.path,"Indexed reference resolves its catalog location by stable ID");
        sp::AudioReference missing;missing.external=true;missing.source.path=base.getChildFile("offline.wav").getFullPathName();result=query(missing);check(result.reference.error.containsIgnoreCase("offline"),"Offline reference actionable");
        auto third=base.getChildFile("third.wav"),opposed=base.getChildFile("opposed.wav");multichannel(third,false);multichannel(opposed,true);
        sp::AudioReference multi;multi.external=true;multi.source.path=third.getFullPathName();result=query(multi);check(result.reference.error.isEmpty(),"All-channel reference hears channel three");
        multi.source={};multi.source.path=opposed.getFullPathName();result=query(multi);check(result.reference.error.containsIgnoreCase("silent"),"Opposing polarity explicitly rejected as silent analysis mix");
        auto gapped=base.getChildFile("sound-with-silence.wav");
        {
            juce::AudioFormatManager formats;formats.registerBasicFormats();std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(third));
            juce::AudioBuffer<float> samples(4,48000*30);samples.clear();reader->read(&samples,0,48000,4800,true,true);
            juce::WavAudioFormat format;auto stream=gapped.createOutputStream();std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.release(),48000,4,24,{},0));
            check(writer && writer->writeFromAudioSampleBuffer(samples,0,samples.getNumSamples()),"Mixed sound/silence long reference fixture");
        }
        multi.source={};multi.source.path=gapped.getFullPathName();result=query(multi);
        check(result.reference.error.isEmpty() && result.reference.silentWindows>0,"Silent windows are skipped without rejecting a long reference that contains sound");
        ref.channelPolicy="unsupported-policy";result=query(ref);check(result.reference.error.containsIgnoreCase("policy"),"Channel policy is part of validated reference identity");ref.channelPolicy=sp::analysisChannelPolicy;
    }
    {
        sp::LibraryService library(dbFile,executable,model);wait([&]{return library.snapshot().semantic.ready;},"Model reopen");
        auto id=library.findSimilar(ref);wait([&]{return library.snapshot().semanticCompletedRequest==id;},"Reference after restart");
        check(library.snapshot().reference.cacheHit && library.snapshot().semantic.windowsThisSession==0,"Reopen reuses exact reference and completed analysis");
    }
    auto *o=new juce::DynamicObject();o->setProperty("passed",true);o->setProperty("timings",cases);base.getChildFile("workflow.json").replaceWithText(juce::JSON::toString(juce::var(o)));
    std::cout<<"PASS actual indexed, edited, cached, external and long references; source filters/exclusion; rapid generations; changed/offline/channel failures; reopen reuse\n";
}
void runCapacityAccuracy(const juce::File &catalog,const juce::File &executable,const juce::File &model,const juce::File &output)
{
    sp::Catalog writer(catalog);sp::Catalog read(catalog,true);sp::RetrievalIndex cache(catalog);
    wait([&]{return cache.stats().ready;},"Capacity cache ready",120000);sp::AIProcess worker(executable,model);
    const juce::StringArray queries{"Rain pattering steadily outside.","A wooden door slowly creaking.","A metal latch rattles.","Water pouring.","A clock ticking.","Glass breaking.","Footsteps on a floor.","A motor engine rumbling.","A hand saw scraping.","A vacuum cleaner humming."};
    int cases=0;double exactMs=0,cachedMs=0;
    for(auto &text:queries)
    {
        auto command=sp::message("text");command.getDynamicObject()->setProperty("text",text);auto reply=worker.call(command,true);
        std::vector<float> v;for(auto &x:*reply["embedding"].getArray())v.push_back((float)(double)x);
        for(auto root:{juce::String(),juce::String("capacity-narrow"),juce::String("capacity-empty")})
        {auto start=sp::monotonicMs();auto exact=read.semanticSearch(v,sp::modelId(),sp::preprocessingId(),root);exactMs+=sp::monotonicMs()-start;
         start=sp::monotonicMs();auto fast=cache.search(read,v,root,{},100,nullptr,[]{return false;});cachedMs+=sp::monotonicMs()-start;equivalent(exact,fast);++cases;}
    }
    auto *o=new juce::DynamicObject();o->setProperty("passed",true);o->setProperty("cases",cases);o->setProperty("topTenAssetAndRangeAgreement",1.0);o->setProperty("exactTotalMs",exactMs);o->setProperty("cachedTotalMs",cachedMs);
    output.replaceWithText(juce::JSON::toString(juce::var(o)));std::cout<<"PASS capacity exact/cached ordering, scores and grouped source ranges: "<<cases<<" cases\n";
}
void runCapacityRecovery(const juce::File &catalog,const juce::File &output)
{
    auto fixtureFolder=catalog.getParentDirectory().getParentDirectory();
    auto fixture=juce::JSON::parse(fixtureFolder.getChildFile("fixture.json"));
    check(fixtureFolder.getFileName().startsWith("capacity-") && fixture["catalog"].toString()==catalog.getFullPathName(),"Recovery mutates only an owned capacity fixture");
    sp::Catalog writer(catalog),read(catalog,true);std::vector<float> query(512,0);query[0]=1;
    juce::var result(new juce::DynamicObject());auto *o=result.getDynamicObject();auto never=[]{return false;};
    auto metaFile=catalog.getParentDirectory().getChildFile("retrieval/current.json");int64_t originalSequence=0;
    {
        sp::RetrievalIndex cache(catalog);
        // A preceding live-update workload may leave a usable older generation plus a delta.
        // Establish a fully published starting generation before interrupting its replacement.
        wait([&]{auto state=cache.stats();return state.ready && state.sequence==writer.indexState().sequence;},
             "Initial capacity index catches up before interruption",120000);
        auto initial=cache.stats();o->setProperty("initialBuildMs",initial.buildMs);o->setProperty("initialOpenMs",initial.openMs);
        auto expected=read.semanticSearch(query,sp::modelId(),sp::preprocessingId());
        originalSequence=writer.indexState().sequence;
        writer.exec("UPDATE embeddings SET vector=vector WHERE rowid=(SELECT min(rowid) FROM embeddings)");
        wait([&]{return cache.stats().building;},"Replacement generation starts",30000);
        equivalent(expected,cache.search(read,query,{},{},100,nullptr,never));
        check(cache.stats().building,"Old generation served a current query during replacement construction");
        o->setProperty("oldGenerationServedWhileBuilding",true);
    }
    check((int64_t)juce::JSON::parse(metaFile)["sequence"]==originalSequence,"Interrupted construction preserves the validated published generation");
    o->setProperty("interruptedBuildPreservedPublishedGeneration",true);
    {
        sp::RetrievalIndex cache(catalog);wait([&]{return cache.stats().ready;},"Index reopens after interruption",30000);
        check(cache.stats().reused,"Valid previous generation reused before replacement completes");
        equivalent(read.semanticSearch(query,sp::modelId(),sp::preprocessingId()),cache.search(read,query,{},{},100,nullptr,never));
        o->setProperty("reopenServedCurrentDelta",true);
        wait([&]{auto sequence=writer.indexState().sequence;return cache.stats().sequence==sequence &&
            (int64_t)juce::JSON::parse(metaFile)["sequence"]==sequence;},"Replacement atomically publishes after recovery",120000);
        o->setProperty("recoveryBuildMs",cache.stats().buildMs);
    }
    o->setProperty("passed",true);o->setProperty("regions",fixture["regions"]);o->setProperty("peakResidentBytes",sp::performance.json()["peakResidentBytes"]);
    output.replaceWithText(juce::JSON::toString(result));std::cout<<"PASS capacity construction interruption, old-generation queries, live delta and recovery\n";
}
