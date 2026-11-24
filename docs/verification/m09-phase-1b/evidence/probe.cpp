// Out-of-tree acceptance instrument. Link only to objects from the frozen test build.
#include "LibraryView.h"
#include "WorkspaceTransfer.h"
#include "TrackModel.h"
#include "NativeEffectsDSP.h"
#include "DesignSession.h"
#include "ProjectPackage.h"
#include "SfxGeneration.h"
#include "AudioStream.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace sp;
using File = juce::File;
using String = juce::String;
const auto never = [] { return false; };
void check(bool condition, const char* label)
{
    if (!condition) throw std::runtime_error(label);
}
void save(const File& dest, const juce::var& value)
{
    check(dest.replaceWithText(juce::JSON::toString(value)), "write evidence JSON");
}
void dump(const File& file, const juce::AudioBuffer<float>& buffer, int n)
{
    std::ofstream out(file.getFullPathName().toStdString(), std::ios::binary);
    for (int i=0; i<n; ++i)
        for (int c=0; c<buffer.getNumChannels(); ++c) {
            const float v=buffer.getSample(c,i);
            out.write(reinterpret_cast<const char*>(&v),sizeof(v));
        }
    check(out.good(), "write captured PCM");
}
void consume(PcmStream& producer, const File& dest, int length)
{
    juce::AudioBuffer<float> pcm(2,length);
    float unity=1;
    int offset=0;
    while (offset<length) {
        producer.fill(997);
        float* outputs[]={pcm.getWritePointer(0,offset),pcm.getWritePointer(1,offset)};
        int count=producer.read(outputs,2,std::min(257,length-offset),unity,1);
        check(count>0,"producer ended early");
        offset+=count;
    }
    check(producer.consumedOutputFrames()==length,"consumed count");
    dump(dest,pcm,length);
}
int captureHelper()
{
    // A controlled sink for the actual LibraryView -> AudioClient IPC messages.
    // Use the production file decoder, producer ring and consumer at unity gain.
    const File dir(juce::SystemStats::getEnvironmentVariable("SP_PHASE1B_WORK",{}));
    std::string line;
    while (std::getline(std::cin,line)) {
        auto cmd=juce::JSON::parse(String(line));
        auto action=cmd["command"].toString();
        if (action=="quit") break;
        auto reply=message("status");
        if (action=="play") {
            int64_t first=(juce::int64)cmd["startSample"],last=(juce::int64)cmd["endSample"];
            String stem="audition-"+String(first);
            auto stream=std::make_unique<PcmStream>(File(cmd["path"].toString()),48000,first,last,
                                                    (int)cmd["auditionPair"]);
            consume(*stream,dir.getChildFile(stem+".f32"),int(last-first));
            stream->fill();check(stream->ended(),"audition read beyond selected endpoint");
            save(dir.getChildFile(stem+".json"),cmd);
            reply.getDynamicObject()->setProperty("id",cmd["id"]);
            reply.getDynamicObject()->setProperty("playGeneration",cmd["playGeneration"]);
        }
        if (!sendMessage(0,reply)) break;
    }
    return 0;
}
namespace sp {
struct LibraryPreviewTestAccess {
    static void compare(const File& dir) {
        auto view=std::make_unique<LibraryView>(dir.getChildFile("library.sqlite"),
                                               dir.getChildFile("MacOS/QA"),false);
        view->stopTimer();
        auto a=inspectDesignSource(dir.getChildFile("roles.wav"));
        view->selectedAsset=a;
        view->rangeAssetId=a.id; view->rangeRevision=a.revision;
        view->pinnedReference.source.id="pinned-reference-1b";
        view->auditionChannels.clear(juce::dontSendNotification);
        view->auditionChannels.addItem("Surround pair",4);
        view->auditionChannels.setSelectedId(4,juce::dontSendNotification);
        for (bool refined : {false,true}) {
            view->suggestions=EventSuggestions{{100,47000},{{30000,30241},{37200,37513}},"Two events"};
            view->chooseRefinement(101);
            view->chooseRefinement(refined ? 1 : 2);
            int first=refined?37200:100,last=refined?37513:47000;
            auto receipt=dir.getChildFile("audition-"+String(first)+".json");
            for(int i=0;i<500 && !receipt.existsAsFile();++i) juce::Thread::sleep(10);
            check(receipt.existsAsFile(),"Library command did not reach capture helper");
            auto cmd=juce::JSON::parse(receipt);
            check(cmd["id"].toString()==a.id && (int64_t)(juce::int64)cmd["playGeneration"]==(int64_t)view->playingGeneration
                  && (int)cmd["auditionPair"]==2,"audition session and pair");
            view->chooseRefinement(refined?3:4);
            auto delivery=view->transferSelection();
            check(delivery && delivery->first==first && delivery->last==last &&
                  view->pinnedReference.source.id=="pinned-reference-1b","delivery bounds and pinned session");
            TransferRequest request;
            request.source=a;request.scope=TransferScope::sourceRange;
            request.first=delivery->first;request.last=delivery->last;
            request.durableFolder=dir.getChildFile("delivery");
            auto artifact=prepareTransfer(request,never);
            verifyTransferArtifact(artifact);
            save(dir.getChildFile("transfer-"+String(first)+".json"),transferReceipt(request,artifact));
            check(artifact.file.copyFileTo(dir.getChildFile("excerpt-"+String(first)+".wav")),"archive excerpt");
        }
    }
};
}
DesignDocument mixer(const File& source,int rate,String& track,String& aux)
{
    DesignDocument d; d.sampleRate=rate;d.original.layout=channelLayout("stereo-v1");d.original.masterDb=0;
    track=addTrack(d.original,TrackKind::audio);
    aux=addTrack(d.original,TrackKind::fxReturn);
    auto asset=inspectDesignSource(source);
    auto layer=makeDesignLayer(asset,0,asset.samples,true);
    initialiseRouting(layer,d.original.layout);layer.fadeIn=layer.fadeOut=0;
    d.original.layers.push_back(layer);attachClip(d,d.original.layers.back(),track);
    auto& send=findTrack(d.original,track)->sends.emplace_back();
    send.destination=aux;send.enabled=true;send.levelDb=0;send.postFader=false;
    return d;
}
void delay(const File& dir)
{
    for (int rate:{44100,48000,96000}) {
        String track,aux;
        auto d=mixer(dir.getChildFile("pulses-"+String(rate)+".wav"),rate,track,aux);
        auto& rack=findTrack(d.original,aux)->inserts;
        auto delayFx=makeStockEffect("delay");delayFx.parameters={10,.5,1,0};
        auto limiter=makeStockEffect("peak-limiter");
        NativeEffectProcessor latencyProbe(limiter,d.original.layout,rate);
        std::cout<<"rate "<<rate<<" declared lookahead frames "<<latencyProbe.latency()<<std::endl;
        check(latencyProbe.latency()==std::llround(rate*.005),"lookahead declaration");
        rack.stock={delayFx,limiter};normaliseInsertChain(rack);
        for(int mode:{0,1,2}){
            rack.stock.front().bypass=mode==1;
            findTrack(d.original,aux)->mute=mode==2;
            for(int partition:{1,32,61,257,512}){
                DesignGraph graph(d);
                juce::AudioBuffer<float> out(2,12288);
                for(int i=0;i<12288;i+=partition) graph.render(out,i,std::min(partition,12288-i));
                dump(dir.getChildFile("delay-"+String(rate)+"-"+String(mode)+"-"+String(partition)+".f32"),out,12288);
            }
        }
    }
}
void automation(const File& dir)
{
    String track,aux;
    auto d=mixer(dir.getChildFile("pulses-48000.wav"),48000,track,aux);
    auto& source=*findTrack(d.original,track);auto& send=source.sends.front();
    source.volumeDb=0;send.levelDb=-12;
    const AutomationTarget volume{{},"volume"},level{send.id,"send-level"};
    addAutomationPoint(d.original,track,volume,0,-6,48000);
    addAutomationPoint(d.original,track,level,0,-3,48000);
    int i=0;
    for(int mask:{0,1,3,2,0}) {
        auto& lane=source.automation;
        automationCurve(lane,volume)->read=(mask&1)!=0;automationCurve(lane,level)->read=(mask&2)!=0;
        String prefix="automation-"+String(i++)+"-"+String(mask);
        auto file=dir.getChildFile(prefix+".soundplus");saveDesignAtomic(file,d);auto opened=loadDesign(file);
        auto& saved=findTrack(opened.original,track)->automation;
        check(saved.curves.size()==2 && !saved.expanded && saved.shown==volume &&
              automationCurve(saved,volume)->read==bool(mask&1) && automationCurve(saved,level)->read==bool(mask&2),"persistent lane and independent reads");
        auto producer=std::make_unique<PcmStream>(opened,48000,0);
        consume(*producer,dir.getChildFile(prefix+".f32"),8192);
        DesignRenderSettings output;output.layout="same";output.tailSeconds=0;
        auto result=renderDesign(opened,dir.getChildFile("exports"),output,never,[](double){});
        check(File(result.path).copyFileTo(dir.getChildFile(prefix+".wav")),"archive finalized mix");
    }
}
void credentialArtifacts(const File& dir, const File& tone)
{
    auto key=sfx::keychain("qa-phase1b-"+juce::Uuid().toString());
    struct Cleanup {sfx::Credentials& k; ~Cleanup(){try {k.erase();}catch(...) {}}} cleanup{*key};
    check(key->read().isEmpty(),"isolated QA Keychain account");
    key->save("fixture-secret-NEVER-PERSIST");
    check(key->read()=="fixture-secret-NEVER-PERSIST","fixture Keychain readback");
    DesignSessionServices s;s.automaticRecovery=false;s.timers=false;s.now=[] {return 0.;};
    s.submit=[](auto){return true;};s.stop=[]{};s.status=[]{return message("status");};s.parameters=[](auto&,auto,auto){};
    DesignSession session(dir.getChildFile("generated.sqlite"),s);
    auto a=inspectDesignSource(tone);
    session.add(a,0,a.samples,false,0,-1,true,2);
    auto snapshot=[&](String name,size_t count){
        check(session.document.activeMix().layers.size()==count,"generated clip undo/redo");
        auto p=dir.getChildFile("generated-"+name+".soundplus");saveDesignAtomic(p,session.document);
        check(loadDesign(p).activeMix().layers.size()==count,"reopen generated project");
    };
    snapshot("edited",1);session.undo();snapshot("undo",0);session.redo();snapshot("redo",1);
    const auto plan=planPackage(session.document);
    std::set<String> approved;
    for(const auto& file:plan.files) approved.insert(file.original.getFullPathName());
    auto pack=dir.getChildFile("generated-package");
    collectPackage(plan,approved,pack);
    check(openPackage(pack).activeMix().layers.size()==1,"reopen exported portable project");
    key->erase();check(key->read().isEmpty(),"erased QA key");
    DesignRenderSettings output;output.tailSeconds=0;
    auto artifact=renderDesign(session.document,dir.getChildFile("exports"),output,never,[](double){});
    check(File(artifact.path).copyFileTo(dir.getChildFile("generated-export.wav")),"finalized generated mix");
}
int main(int argc,char** argv)
{
    try {
        if(argc==1)return captureHelper();
        juce::ScopedJuceInitialiser_GUI gui;
        const File dir(argv[1]);
        if(String(argv[2])=="delay")delay(dir);
        else if(String(argv[2])=="automation")automation(dir);
        else if(String(argv[2])=="library")LibraryPreviewTestAccess::compare(dir);
        else if(String(argv[2])=="credentials")credentialArtifacts(dir,File(argv[3]));
        else throw std::runtime_error("unknown test");
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
