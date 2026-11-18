#include "SemanticTests.h"
#include "LibraryService.h"
#include "AudioStream.h"
#include "AudioIPC.h"
#include "Waveform.h"
#include "Performance.h"
#include "AIProcess.h"
#include <signal.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <iostream>
#include <thread>
namespace
{
void check(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void wait(F condition, const char *reason, int timeout=30000)
{
    auto until=sp::monotonicMs()+timeout;
    while (!condition()) { if (sp::monotonicMs()>until) throw std::runtime_error(reason); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
void signal(const juce::File &file, int rate)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat format; auto stream=file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.release(),rate,1,16,{},0));
    juce::AudioBuffer<float> audio(1,rate*3); audio.clear();
    for (int at : {4138,rate/2,rate*13/10,rate*21/10})
        for (int n=0;n<100;++n) audio.setSample(0,at+n,0.5f);
    check(writer && writer->writeFromAudioSampleBuffer(audio,0,audio.getNumSamples()),"Write region signal");
}
void nonfiniteWav(const juce::File &file)
{
    auto out=file.createOutputStream();check(out!=nullptr,"Nonfinite fixture stream");
    out->write("RIFF",4);out->writeInt(36+192000);out->write("WAVEfmt ",8);out->writeInt(16);
    out->writeShort(3);out->writeShort(1);out->writeInt(48000);out->writeInt(192000);
    out->writeShort(4);out->writeShort(32);out->write("data",4);out->writeInt(192000);
    for(int i=0;i<48000;++i)out->writeFloat(std::numeric_limits<float>::quiet_NaN());
}
sp::Asset add(sp::Catalog &db,const sp::Root &root,const juce::File &file,int rate)
{
    sp::FileStamp stamp; check(sp::fileStamp(file,stamp),"Source stamp");
    db.discover(root,file.getRelativePathFrom(juce::File(root.path)),stamp,1);
    auto a=db.existing(root.id,file.getRelativePathFrom(juce::File(root.path)));
    db.metadata(a,3,1,rate,{},3*rate); return db.asset(a.id);
}
sp::AnalysisUpdate update(const sp::Asset &a,int64_t first,int64_t last,int basis=0)
{
    sp::AnalysisUpdate u;u.asset=a;u.start=first;u.end=last;u.nextStart=last;u.done=last==a.samples;
    u.model="model-A";u.preprocessing="pre-A";u.embedding.assign(512,0);u.embedding[(size_t)basis]=1;
    return u;
}
}
void runSemanticIntegrity(const juce::File &base,const juce::File &executable)
{
    check(base.getFileName()=="semantic-test-output","Use isolated semantic-test-output");
    base.deleteRecursively();base.createDirectory();
    for (int rate : {44100,48000,96000})
    {
        auto file=base.getChildFile("signal-"+juce::String(rate)+".wav");signal(file,rate);
        for (auto bounds : {std::pair<int64_t,int64_t>{rate,2*rate}, {rate*15/10,2*rate}, {37,4138}})
        {
            sp::PcmStream pcm(file,48000,bounds.first,bounds.second);
            juce::AudioBuffer<float> block(2,257);float gain=1;double energy=0;int64_t count=0;
            check(std::abs(pcm.seconds()-(double)bounds.first/rate)<1e-9,"Source-relative initial time");
            while (!pcm.ended())
            {
                pcm.fill();block.clear();auto reads=pcm.decodedReads();
                int n=pcm.read(block.getArrayOfWritePointers(),2,257,gain,1);count+=n;
                check(pcm.decodedReads()==reads,"Region callback has no decoder I/O");
                for(int j=0;j<n;++j) energy+=std::abs(block.getSample(0,j));
            }
            check(count==(int64_t)std::ceil((bounds.second-bounds.first)*48000.0/rate),"Region output frame count");
            check(pcm.sourceSample()==bounds.second,"Half-open source end reported exactly");
            check(bounds.first==rate ? energy>10 : energy==0,"Region includes only intended source event");
        }
    }
    auto dbFile=base.getChildFile("catalog/library.sqlite");dbFile.getParentDirectory().createDirectory();
    sp::Asset a,b;
    {
        sp::Catalog db(dbFile);auto root=db.addRoot(base);
        a=add(db,root,base.getChildFile("signal-44100.wav"),44100);
        b=add(db,root,base.getChildFile("signal-48000.wav"),48000);
        db.seedAnalysis("model-A","pre-A");
        auto task=db.nextAnalysis("model-A","pre-A",a.id);
        check(task && task->asset.id==a.id && task->nextStart==0,"Selected-recording priority and pending checkpoint");
        auto first=update(a,0,44100);db.begin();check(db.commitAnalysis(first),"Publish first original-source interval");db.commit();
        task=db.nextAnalysis("model-A","pre-A",a.id);check(task && task->nextStart==44100,"Resume checkpoint advances");
        auto overlap=update(a,22050,66150);db.begin();db.commitAnalysis(overlap);db.commit();
        auto failedWindow=update(a,66150,88200);failedWindow.error="Transient decoder failure";
        db.begin();db.commitAnalysis(failedWindow);db.commit();db.retryAnalysis("model-A","pre-A");
        task=db.nextAnalysis("model-A","pre-A",a.id);
        check(task && task->nextStart==failedWindow.start,"Retry restarts the failed window without skipping source ranges");
        auto second=update(b,0,b.samples,1);db.begin();db.commitAnalysis(second);db.commit();
        auto otherFile=base.getChildFile("other/source.wav");signal(otherFile,48000);
        auto otherRoot=db.addRoot(otherFile.getParentDirectory());
        auto c=add(db,otherRoot,otherFile,48000);auto other=update(c,0,c.samples);
        db.begin();db.commitAnalysis(other);db.commit();
        auto filtered=db.semanticSearch(first.embedding,"model-A","pre-A",root.id);
        check(std::all_of(filtered.begin(),filtered.end(),[&](const auto &v){return v.rootId==root.id;}),"Source filter shared with semantic retrieval");
        root.online=false;db.rootState(root);
        filtered=db.semanticSearch(first.embedding,"model-A","pre-A",root.id);
        check(!filtered.empty() && std::none_of(filtered.begin(),filtered.end(),[](const auto &v){return v.online;}),"Offline semantic entries retained and marked unavailable");
        root.online=true;db.rootState(root);
        auto result=db.semanticSearch(first.embedding,"model-A","pre-A",root.id);
        check(!result.empty() && result[0].id==a.id && result[0].matches.size()==1 && result[0].matches[0].groupedWindows==2,
            "Overlapping windows group into one source moment");
        check(db.semanticSearch(first.embedding,"other-model","pre-A").empty(),"Different model space never searched");
        check(db.semanticSearch(first.embedding,"model-A","other-preprocessing").empty(),"Different preprocessing never searched");
        auto before=a;
        sp::FileStamp changed{a.size+1,a.modified+1000000000,a.inode};
        db.discover(root,a.relative,changed,2);a=db.asset(a.id);
        check(a.revision>before.revision,"Source revision advances");
        db.metadata(a,3,1,44100,{},132300);a=db.asset(a.id);
        db.begin();check(!db.commitAnalysis(first),"Late prior-revision output rejected");db.commit();
        result=db.semanticSearch(first.embedding,"model-A","pre-A");
        check(std::none_of(result.begin(),result.end(),[&](const auto &v){return v.id==a.id;}),"Obsolete vectors invalidated");
        check(db.search(a.id).size()==1,"Exact stable-ID lookup preserved");
    }
    {
        auto file=base.getChildFile("signal-96000.wav");sp::FileStamp stamp;sp::fileStamp(file,stamp);
        sp::Asset source;source.id="waveform-fixture";source.path=file.getFullPathName();source.revision=1;
        source.samples=288000;source.sampleRate=96000;source.size=stamp.size;source.modified=stamp.modified;source.inode=stamp.inode;
        sp::WaveformCache cache(base.getChildFile("wave-cache"));auto request=cache.request(source);
        wait([&]{auto d=cache.snapshot();return d && d->generation==request;},"Waveform background completion");
        auto wave=cache.snapshot();check(wave->error.isEmpty() && !wave->peaks.empty() && !wave->cacheHit,"First waveform generated");
        request=cache.request(source);wait([&]{auto d=cache.snapshot();return d && d->generation==request;},"Cached waveform reload");
        check(cache.snapshot()->cacheHit,"Waveform derivative reused");
        auto old=request;request=cache.request(a);request=cache.request(source);
        wait([&]{auto d=cache.snapshot();return d && d->generation==request;},"Latest waveform request wins");
        check(cache.snapshot()->assetId==source.id && cache.snapshot()->generation>old,"Stale waveform cannot replace selection");
        sp::AudioClient audio(executable);
        wait([&]{return (double)audio.status()["sampleRate"]>0;},"Region device ready");
        sp::PlaybackRegion region;region.startSample=96000;region.endSample=192000;
        region.size=stamp.size;region.modified=stamp.modified;region.inode=stamp.inode;region.revision=1;
        auto generation=audio.playRegion(source.id,file,region);
        wait([&]{auto state=audio.status();return (uint64_t)(juce::int64)state["playGeneration"]==generation && (bool)state["playing"];},"Live range reaches device");
        auto state=audio.status();check((int64_t)state["startSample"]==96000 && (int64_t)state["endSample"]==192000 && (double)state["seconds"]>=1,"Device reports original source bounds");
        for(int n=0;n<100;++n){region.positionSample=100000+n;generation=audio.playRegion(source.id,file,region);}
        wait([&]{return (uint64_t)(juce::int64)audio.status()["playGeneration"]==generation;},"Latest seek generation reaches device");
        check((int64_t)audio.status()["startSample"]==100099,"Obsolete queued seeks discarded");
        audio.stop();
        region.modified+=1;generation=audio.playRegion(source.id,file,region);
        wait([&]{auto s=audio.status();return (uint64_t)(juce::int64)s["playGeneration"]==generation && s["error"].toString().isNotEmpty();},"Stale source playback rejected");
        auto bad=base.getChildFile("bad-float.wav");nonfiniteWav(bad);sp::FileStamp badStamp;sp::fileStamp(bad,badStamp);
        sp::PlaybackRegion badRegion;badRegion.size=badStamp.size;badRegion.modified=badStamp.modified;badRegion.inode=badStamp.inode;
        auto oldProcess=(int64_t)audio.status()["processGeneration"];
        generation=audio.playRegion("bad-float",bad,badRegion);
        wait([&]{auto s=audio.status();return (uint64_t)(juce::int64)s["playGeneration"]==generation && s["error"].toString().containsIgnoreCase("Nonfinite");},"Nonfinite samples rejected off callback");
        check((int64_t)audio.status()["processGeneration"]==oldProcess,"Bad recording does not crash the audio worker");
        audio.stop();
    }
    std::cout<<"PASS semantic revision/space integrity, window grouping, source ranges, waveform cache and seek generations\n";
}

void runSemanticWorkflow(const juce::File &executable,const juce::File &model,const juce::File &corpus,const juce::File &base)
{
    check(base.getFileName()=="semantic-workflow-output","Use isolated semantic-workflow-output");
    base.deleteRecursively();base.createDirectory();
    auto media=base.getChildFile("media");check(corpus.copyDirectoryTo(media),"Copy real-effect workflow fixture");
    media.getChildFile("broken-header.wav").replaceWithText("not a supported audio header");
    nonfiniteWav(media.getChildFile("nonfinite.wav"));
    auto db=base.getChildFile("catalog/library.sqlite");
    {
        sp::AIProcess worker(executable,model);auto command=sp::message("text");command.getDynamicObject()->setProperty("text","A wooden door creaking.");
        auto first=worker.call(command,true);check(first["embedding"].isArray(),"Real paired model ready");
        auto pid=(int)first["workerPid"];check(pid>0,"Owned model worker PID");kill(pid,SIGKILL);
        bool failed=false;try { worker.call(command,true); } catch (const std::exception &) { failed=true; }
        check(failed,"Worker loss detected");
        auto recovered=worker.call(command,true);
        check(recovered["embedding"].isArray() && (int64_t)recovered["processGeneration"]>(int64_t)first["processGeneration"],
            "Supervised inference restarts in the same model space");
    }
    int partial=0;
    {
        sp::LibraryService service(db,executable,model);service.addFolder(media);
        wait([&]{return service.snapshot().semantic.coverage.regions>=3;},"Real audio embeddings publish progressively",120000);
        service.pause(true);wait([&]{return service.snapshot().progress.paused;},"Pause includes current semantic window",30000);
        sp::Catalog read(db,true);partial=read.analysisCounts(sp::modelId(),sp::preprocessingId()).regions;
        check(partial>0 && partial<74,"Partial embeddings committed before interruption");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(read.analysisCounts(sp::modelId(),sp::preprocessingId()).regions==partial,"Pause holds semantic commits");
        auto id=service.search("A wooden door slowly creaking as it moves.");service.semanticNow();
        wait([&]{return service.snapshot().semanticCompletedRequest==id;},"Semantic query remains available while paused",30000);
    }
    {
        sp::LibraryService service(db,executable,model);
        wait([&]{return service.snapshot().semantic.coverage.complete==43;},"Resume every real recording",180000);
        wait([&]{return service.snapshot().semantic.coverage.failed==1;},"One nonfinite recording fails without stopping healthy jobs");
        auto snapshot=service.snapshot();check(snapshot.semantic.coverage.regions==74 && snapshot.semantic.coverage.failed==1,"Expected short/final/overlapping windows and isolated failure");
        for(int n=0;n<300;++n)service.search("obsolete semantic query "+juce::String(n));
        auto id=service.search("Someone knocking on a wooden door.");service.semanticNow();
        wait([&]{return service.snapshot().semanticCompletedRequest==id;},"Newest semantic query wins",30000);
        snapshot=service.snapshot();check(!snapshot.results.empty() && !snapshot.results[0].matches.empty(),"Real anonymous sound results through fused catalog path");
        auto asset=snapshot.results[0];id=service.search(asset.id);service.semanticNow();
        wait([&]{return service.snapshot().semanticCompletedRequest==id;},"Asset ID with AI enabled");
        check(service.snapshot().total==1 && service.snapshot().results[0].id==asset.id,"AI preserves exact identifier lookup");
        auto report=juce::var(new juce::DynamicObject());auto *r=report.getDynamicObject();
        r->setProperty("passed",true);r->setProperty("partialRegionsBeforeInterruption",partial);
        r->setProperty("completedFiles",43);r->setProperty("regions",74);
        r->setProperty("isolatedNonfiniteFailure",true);
        r->setProperty("windowsAfterResume",snapshot.semantic.windowsThisSession);
        r->setProperty("modelLoadMs",snapshot.semantic.modelLoadMs);
        r->setProperty("analysisWallMs",snapshot.semantic.analysisWallMs);
        base.getChildFile("workflow.json").replaceWithText(juce::JSON::toString(report));
    }
    {
        sp::LibraryService service(db,executable,model);
        wait([&]{return service.snapshot().semantic.ready && service.snapshot().semantic.coverage.complete==43;},"Completed analysis reopen",30000);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(service.snapshot().semantic.windowsThisSession==0 && service.snapshot().headersRead==0,"Unchanged reopen reuses embeddings and headers");
        auto id=service.search("Glass breaking and scattering in sharp fragments.");service.semanticNow();
        wait([&]{return service.snapshot().semanticCompletedRequest==id;},"Installed local model searches after reopen");
        check(!service.snapshot().results.empty(),"Reopened local semantic results");
    }
    std::cout<<"PASS actual local inference, progressive windows, interrupted resume, pause/query generations and unchanged reuse\n";
}
