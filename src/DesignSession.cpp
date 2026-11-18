#include "DesignSession.h"
#include "Performance.h"
#include <cmath>
namespace sp
{
namespace
{
DesignLayer *layerById(DesignDocument &d,const juce::String &id){for(auto &l:d.activeMix().layers)if(l.id==id)return &l;return nullptr;}
void ensureAvailable(const DesignDocument &d){bool solo=std::any_of(d.activeMix().layers.begin(),d.activeMix().layers.end(),[](auto &l){return l.solo;});for(auto &l:d.activeMix().layers)if(!l.mute && (!solo || l.solo) && l.source.error.isNotEmpty())throw std::runtime_error((l.name+": "+l.source.error).toStdString());}
}
DesignSession::DesignSession(juce::File file,AudioClient &client):catalog(std::move(file)),directory(catalog.getParentDirectory()),audio(client)
{
    outputFolder=juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("SoundPlus/Renders");
    worker=std::thread([this]{run();});startTimer(100);
    juce::Component::SafePointer<DesignSession> safe(this);auto recovery=directory.getChildFile("designer-autosave/recovery.soundplus");
    enqueue([this,safe,recovery]{if(!recovery.existsAsFile())return;auto d=loadDesign(recovery);resolveDesignSources(d,catalog);juce::MessageManager::callAsync([safe,d=std::move(d)]()mutable{if(safe && !safe->dirty && safe->document.original.layers.empty()){safe->document=std::move(d);if(safe->document.outputFolder.isNotEmpty())safe->outputFolder=juce::File(safe->document.outputFolder);safe->dirty=true;safe->message="Recovered autosave; Save chooses a design file";if(!safe->document.outputs.empty())safe->lastOutput=juce::File(safe->document.outputs.back().path);safe->notify();}});});
}
DesignSession::~DesignSession(){stopTimer();quitting=true;renderCancelled=true;++previewGeneration;wake.notify_all();if(renderWorker.joinable())renderWorker.join();if(worker.joinable())worker.join();}
void DesignSession::notify(){if(onChange)onChange();}
void DesignSession::enqueue(std::function<void()> job){{std::lock_guard lock(mutex);if(jobs.size()<32){jobs.push_back(std::move(job));wake.notify_all();return;}}message="Background queue is busy; try again";notify();}
void DesignSession::run()
{
    while(!quitting){std::function<void()> job;std::optional<Preview> next;
        {std::unique_lock lock(mutex);wake.wait(lock,[&]{return quitting || !jobs.empty() || preview.has_value();});if(quitting){lock.unlock();if(dirty)try{saveDesignAtomic(directory.getChildFile("designer-autosave/recovery.soundplus"),document);}catch(...){}return;}if(!jobs.empty()){job=std::move(jobs.front());jobs.pop_front();}else{next=std::move(preview);preview.reset();}}
        try{if(job)job();else if(next){auto request=std::move(*next);if(request.generation!=previewGeneration)continue;resolveDesignSources(request.document,catalog);ensureAvailable(request.document);
                auto file=directory.getChildFile("designer-preview").getChildFile(juce::Uuid().toString()+".soundplus");saveDesignAtomic(file,request.document,[&]{return quitting || request.generation!=previewGeneration;});
                juce::Component::SafePointer<DesignSession> safe(this);juce::MessageManager::callAsync([safe,file,request]{if(safe && request.generation==safe->previewGeneration){safe->audio.playDesign("design:"+request.document.id,file,request.position);safe->previewWaiting=false;safe->message.clear();safe->notify();}});}}
        catch(const QueryCancelled &){}
        catch(const std::exception &e){juce::Component::SafePointer<DesignSession> safe(this);auto error=juce::String(e.what());juce::MessageManager::callAsync([safe,error]{if(safe){safe->working=false;safe->previewWaiting=false;safe->message=error;safe->notify();}});}}
}
void DesignSession::changed(){dirty=true;lastEdit=monotonicMs();if(ownsPlayback())previewDue=lastEdit+100;notify();}
void DesignSession::edit(const juce::String &label,const std::function<void(DesignDocument &)> &fn)
{
    auto before=document;try{fn(document);++document.revision;validateDesign(document);}catch(const std::exception &e){document=std::move(before);message=e.what();notify();return;}
    if(!gesture){undoHistory.push_back({std::move(before),label});if(undoHistory.size()>100)undoHistory.erase(undoHistory.begin());redoHistory.clear();}changed();
}
void DesignSession::beginGesture(){if(!gesture)gesture=document;}
void DesignSession::endGesture(){if(gesture){if(document.revision!=gesture->revision){undoHistory.push_back({std::move(*gesture),"Adjust layer"});redoHistory.clear();}gesture.reset();}}
void DesignSession::undo(){stop();endGesture();if(undoHistory.empty())return;auto revision=document.revision+1;redoHistory.push_back({document,undoHistory.back().label});document=std::move(undoHistory.back().document);undoHistory.pop_back();document.revision=revision;changed();}
void DesignSession::redo(){if(redoHistory.empty())return;stop();auto revision=document.revision+1;undoHistory.push_back({document,redoHistory.back().label});document=std::move(redoHistory.back().document);redoHistory.pop_back();document.revision=revision;changed();}
DesignLayer *DesignSession::selectedLayer(){return layerById(document,document.selectedLayer);}
void DesignSession::newDesign(){if(rendering)return;stop();document={};savedFile=juce::File();lastOutput=juce::File();undoHistory.clear();redoHistory.clear();dirty=false;autoRevision=0;message.clear();notify();}
void DesignSession::open(const juce::File &file)
{
    if(rendering)return;working=true;message="Opening design…";notify();juce::Component::SafePointer<DesignSession> safe(this);
    enqueue([this,safe,file]{auto d=loadDesign(file);resolveDesignSources(d,catalog);juce::MessageManager::callAsync([safe,file,d=std::move(d)]()mutable{if(safe){safe->stop();safe->document=std::move(d);if(safe->document.outputFolder.isNotEmpty())safe->outputFolder=juce::File(safe->document.outputFolder);safe->lastOutput=juce::File();safe->savedFile=file;safe->undoHistory.clear();safe->redoHistory.clear();safe->dirty=false;safe->working=false;safe->message="Design opened";if(!safe->document.outputs.empty())safe->lastOutput=juce::File(safe->document.outputs.back().path);safe->notify();}});});
}
void DesignSession::save(const juce::File &file)
{
    auto d=document;working=true;message="Saving design…";notify();juce::Component::SafePointer<DesignSession> safe(this);
    enqueue([this,safe,file,d]{saveDesignAtomic(file,d,[&]{return quitting.load();});juce::MessageManager::callAsync([safe,file,d]{if(safe){safe->savedFile=file;safe->working=false;if(safe->document.id==d.id && safe->document.revision==d.revision)safe->dirty=false;safe->message="Saved "+file.getFileName();safe->notify();}});});
}
void DesignSession::add(const Asset &source,int64_t first,int64_t last,bool external,int pair)
{
    edit("Add layer",[&](auto &d){auto layer=makeDesignLayer(source,first,last,external);layer.region.auditionPair=pair;d.selectedLayer=layer.id;d.activeMix().layers.push_back(std::move(layer));});
}
void DesignSession::addExternal(const juce::File &file)
{
    auto id=document.id;working=true;message="Reading source…";notify();juce::Component::SafePointer<DesignSession> safe(this);
    enqueue([safe,file,id]{auto a=inspectDesignSource(file);juce::MessageManager::callAsync([safe,a,id]{if(safe && safe->document.id==id){safe->working=false;safe->message.clear();safe->add(a,0,a.samples,true);}});});
}
void DesignSession::replaceSource(const juce::String &layer,const juce::File &file)
{
    auto id=document.id;juce::Component::SafePointer<DesignSession> safe(this);enqueue([safe,file,layer,id]{auto a=inspectDesignSource(file);juce::MessageManager::callAsync([safe,a,layer,id]{if(safe && safe->document.id==id)safe->edit("Relink source",[&](auto &d){if(auto *l=layerById(d,layer)){l->source=a;l->external=true;l->region.startSample=std::clamp<int64_t>(l->region.startSample,0,a.samples-1);l->region.endSample=std::clamp<int64_t>(l->region.endSample,l->region.startSample+1,a.samples);l->sync=std::clamp(l->sync,l->region.startSample,l->region.endSample-1);l->region.size=a.size;l->region.modified=a.modified;l->region.inode=a.inode;l->region.revision=a.revision;if(l->region.auditionPair*2>=a.channels)l->region.auditionPair=0;}});});});
}
bool DesignSession::ownsPlayback(){auto state=audio.status();return previewWaiting || (state["id"].toString()=="design:"+document.id && ((bool)state["playing"] || (bool)state["pendingPlayback"]));}
void DesignSession::schedulePreview(){auto state=audio.status();auto head=document.playhead;if(state["id"].toString()=="design:"+document.id && (bool)state["playing"])head=(int64_t)state["sourceSample"];auto duration=designPreviewEnd(document);head=std::clamp<int64_t>(head,0,duration-1);auto generation=++previewGeneration;{std::lock_guard lock(mutex);preview=Preview{document,head,generation};}previewWaiting=true;wake.notify_all();}
void DesignSession::play(){if(ownsPlayback()){stop();return;}if(document.activeMix().layers.empty())return;document.playhead=designPlayStart(document,document.playhead);if(onBeforePlay)onBeforePlay();message="Preparing mix…";schedulePreview();notify();}
void DesignSession::stop(){++previewGeneration;previewWaiting=false;previewDue=0;if(audio.status()["id"].toString().startsWith("design:"))audio.stop();notify();}
void DesignSession::seek(int64_t frame){document.playhead=std::clamp<int64_t>(frame,0,mixDuration(document.activeMix(),document.sampleRate)-1);if(ownsPlayback()){auto generation=++previewGeneration;{std::lock_guard lock(mutex);preview=Preview{document,document.playhead,generation};}previewWaiting=true;wake.notify_all();}notify();}
void DesignSession::selectLayer(const juce::String &id){document.selectedLayer=id;notify();}
void DesignSession::selectVariation(const juce::String &id){edit("Select variation",[&](auto &d){d.activeVariation=id;if(!d.activeMix().layers.empty())d.selectedLayer=d.activeMix().layers.front().id;});}
void DesignSession::generate(uint64_t seed){edit("Generate variations",[&](auto &d){auto generated=generateVariations(d,seed);if(d.variations.size()+generated.size()>32)throw std::runtime_error("Discard candidates before generating more (32 maximum)");for(auto &v:generated)d.variations.push_back(std::move(v));});}
void DesignSession::cancelRender(){renderCancelled=true;}
void DesignSession::startOutput(DesignDocument snapshot,bool copy,const juce::File &folder,juce::String kind)
{
    if(rendering)return;if(renderWorker.joinable())renderWorker.join();renderCancelled=false;rendering=true;renderProgress=0;message=copy ? "Copying verified original…" : "Rendering immutable revision "+juce::String(snapshot.revision)+"…";notify();juce::Component::SafePointer<DesignSession> safe(this);
    renderWorker=std::thread([this,safe,snapshot=std::move(snapshot),copy,folder,kind]()mutable{try{resolveDesignSources(snapshot,catalog);if(!copy)ensureAvailable(snapshot);DesignOutput output;
            if(copy){auto *layer=layerById(snapshot,snapshot.selectedLayer);if(!layer)throw std::runtime_error("Select a layer");output=copyDesignOriginal(*layer,folder,snapshot.revision,[&]{return quitting || renderCancelled;});}
            else {output=renderDesign(snapshot,folder,snapshot.renderSettings,[&]{return quitting || renderCancelled;},[&](double p){renderProgress=p;});output.kind=kind;}
            juce::MessageManager::callAsync([safe,output,id=snapshot.id]{if(safe){safe->rendering=false;safe->renderProgress=1;safe->lastOutput=juce::File(output.path);safe->message="Ready to drag: "+safe->lastOutput.getFileName();if(safe->document.id==id)safe->edit("Record completed output",[&](auto &d){d.outputs.push_back(output);});safe->notify();}});
        }catch(const std::exception &e){auto error=dynamic_cast<const QueryCancelled *>(&e) ? juce::String("Cancelled - no output published") : juce::String(e.what());juce::MessageManager::callAsync([safe,error]{if(safe){safe->rendering=false;safe->message=error;safe->notify();}});}});
}
void DesignSession::render(bool selection)
{
    auto d=document;if(selection){auto *layer=selectedLayer();if(!layer){message="Select a layer to render";notify();return;}auto chosen=*layer;chosen.placement=0;chosen.mute=false;chosen.solo=false;d.activeMix().layers={chosen};d.activeMix().duration=0;d.renderSettings.first=0;d.renderSettings.last=-1;}
    startOutput(std::move(d),false,outputFolder,selection ? "layer-render" : "design-render");
}
void DesignSession::copyOriginal(const juce::File &folder){if(!selectedLayer())return;startOutput(document,true,folder);}
void DesignSession::recoverAutosave(){open(directory.getChildFile("designer-autosave/recovery.soundplus"));}
void DesignSession::timerCallback()
{
    auto state=audio.status();if(state["id"].toString()=="design:"+document.id && !(bool)state["pendingPlayback"] && (bool)state["design"])document.playhead=(int64_t)state["sourceSample"];
    if(previewDue>0 && monotonicMs()>=previewDue){previewDue=0;schedulePreview();}
    if(dirty && document.revision!=autoRevision && monotonicMs()-lastEdit>500){auto d=document;autoRevision=d.revision;auto path=directory.getChildFile("designer-autosave/recovery.soundplus");enqueue([this,d,path]{saveDesignAtomic(path.getSiblingFile(d.id+".soundplus"),d,[&]{return quitting.load();});saveDesignAtomic(path,d,[&]{return quitting.load();});});}
}
}
