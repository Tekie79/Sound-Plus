#pragma once
#include "LibraryView.h"
#include "DesignerView.h"
#include "PluginProcessPanel.h"
#include "WorkspaceSizing.h"
class Workstation final : public juce::Component,public juce::DragAndDropContainer
{
  public:
    juce::PropertiesFile preferences;
    sp::LibraryView library;
    sp::DesignerView designer;
    Workstation(const juce::File &db,bool enableAI,const juce::File &modelDirectory,bool benchmark)
        : preferences(db.getParentDirectory().getChildFile("interface.settings"),sp::ui::preferenceOptions()),
          library(db,juce::File::getSpecialLocation(juce::File::currentExecutableFile),enableAI,modelDirectory),
          designer(db,library.audioClient(),preferences,library.waveformProvider()),divider(*this)
    {
        library.onProcess=[this,db](const sp::Asset &a,int64_t first,int64_t last) { sp::PluginInput input; input.source=a;input.first=first;input.last=last;input.layout=a.layoutOverride.id.isNotEmpty()?a.layoutOverride:a.detectedChannels; if(a.channels<=16)for(int c=0;c<a.channels;++c)input.inputs.push_back(c); showProcessing(input,db); };
        designer.onProcessOutput=[this,db](const juce::File &file) { try { sp::PluginInput input;input.source=sp::inspectDesignSource(file);input.last=input.source.samples;input.layout=input.source.detectedChannels;input.scope="Finalized design/take output";for(int c=0;c<input.source.channels;++c)input.inputs.push_back(c);showProcessing(input,db); } catch(const std::exception &e) { designer.session.message=e.what(); } };
        library.bindPreferences(preferences);
        browserOpen=preferences.getBoolValue("browserOpen",true);
        addAndMakeVisible(library);addChildComponent(designer);addChildComponent(divider);
        library.onShowDesigner=[this]{showDesigner(true);};library.onBeforeAudition=[this]{designer.session.stop();};
        library.onAddToDesigner=[this](const sp::Asset &a,int64_t first,int64_t last,bool external,int pair){designer.addSource(a,first,last,external,pair);showDesigner(true);};
        designer.onLibrary=[this]{showDesigner(false);};
        designer.onToggleBrowser=[this]{browserOpen=!browserOpen;preferences.setValue("browserOpen",browserOpen);resized();};
        designer.onSearch=[this]{browserOpen=true;resized();library.focusSearch();};
        designer.session.onBeforePlay=[this]{library.parkPlayback();};
        library.audioClient().gain((float)preferences.getDoubleValue("monitorVolume",.7));
        showDesigner(!benchmark && preferences.getBoolValue("designerActive",false));
    }
    ~Workstation() override {preferences.setValue("monitorVolume",library.audioClient().monitorGain());preferences.saveIfNeeded();}
    void paint(juce::Graphics &g) override {g.fillAll(sp::ui::background);}
    void resized() override
    {
        library.setCompact(designerActive);
        bool both=designerActive && browserOpen;designer.setBrowserVisible(both);
        library.setVisible(!designerActive || both);designer.setVisible(designerActive);divider.setVisible(both);
        if(both){constexpr int width=sp::ui::workspace::sidebarWidth;library.setBounds(0,0,width,getHeight());divider.setBounds(width,0,6,getHeight());designer.setBounds(width+6,0,getWidth()-width-6,getHeight());}
        else {library.setBounds(getLocalBounds());designer.setBounds(getLocalBounds());}
    }
    void showDesigner(bool yes)
    {
        designerActive=yes;preferences.setValue("designerActive",yes);resized();
        if(isShowing())(yes ? static_cast<juce::Component &>(designer) : static_cast<juce::Component &>(library)).grabKeyboardFocus();
    }
    bool keyPressed(const juce::KeyPress &key) override
    {
        if(library.hasKeyboardFocus(true) && key.getModifiers().isCommandDown() && key.isKeyCode('Z'))return false;
        if(designerActive && key.getModifiers().isCommandDown())
        {
            auto command=juce::CharacterFunctions::toUpperCase((juce::juce_wchar)key.getKeyCode());
            if(command=='S' || command=='R' || command=='Z' || command=='D' || command=='O' || command=='N')
                return designer.commandKey(key);
        }
        return false;
    }
    bool shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails &details,juce::StringArray &files,bool &canMove) override
    {
        try{auto d=sp::designFromJson(juce::JSON::parse(details.description.toString()));if(d.original.layers.size()!=1)return false;auto &l=d.original.layers.front();if(l.region.startSample!=0 || l.region.endSample!=l.source.samples || l.source.error.isNotEmpty() || !juce::File(l.source.path).existsAsFile())return false;files.add(l.source.path);canMove=false;return true;}catch(...){return false;}
    }
  private:
    struct ProcessWindow final : juce::DocumentWindow
    {
        std::function<void()> onClose;
        ProcessWindow():DocumentWindow("Process with Plug-in",sp::ui::background,juce::DocumentWindow::closeButton){setUsingNativeTitleBar(true);}
        void closeButtonPressed() override {if(onClose)onClose();}
    };
    std::unique_ptr<ProcessWindow> processWindow;
    void showProcessing(sp::PluginInput input,const juce::File &db)
    {
        if(designer.session.takeBusy())return;
        processWindow.reset();auto panel=std::make_unique<sp::PluginProcessPanel>(input,db.getParentDirectory().getChildFile("plugin-processing/"+juce::Uuid().toString()),library.audioClient(),library.waveformProvider());
        auto id=designer.session.document.id,variation=designer.session.document.activeVariation,take=designer.session.document.activeTake;auto revision=designer.session.document.revision;
        juce::Component::SafePointer<Workstation> safe(this);
        panel->session.contextValid=[safe,id,variation,take,revision] {return safe&&safe->designer.session.document.id==id&&safe->designer.session.document.activeVariation==variation&&safe->designer.session.document.activeTake==take&&safe->designer.session.document.revision==revision&&!safe->designer.session.takeBusy()&&!safe->designer.session.transitionPending();};
        panel->beforeAudition=[safe]{if(safe){safe->designer.session.stop();safe->library.parkPlayback();}};
        panel->onPrint=[safe](auto &file){if(safe&&!safe->designer.session.takeBusy()){if(safe->designer.session.document.activeTake.isNotEmpty())safe->designer.session.selectTake({});safe->designer.session.addExternal(file);safe->showDesigner(true);}};
        panel->onIndex=[safe](auto &folder){if(safe)safe->library.addFolder(folder);};
        processWindow=std::make_unique<ProcessWindow>();processWindow->onClose=[safe]{juce::MessageManager::callAsync([safe]{if(safe)safe->processWindow.reset();});};
        processWindow->setContentOwned(panel.release(),true);processWindow->centreWithSize(700,650);processWindow->setVisible(true);processWindow->toFront(true);
    }
    bool designerActive=false,browserOpen=true;
    struct Divider : juce::Component
    {
        explicit Divider(Workstation &){setTitle("Library sidebar divider");}
        void paint(juce::Graphics &g) override{g.fillAll(sp::ui::background);g.setColour(sp::ui::line);g.drawVerticalLine(2,12,(float)getHeight()-12);}
    } divider;
    juce::TooltipWindow tooltips{this,600};
};
