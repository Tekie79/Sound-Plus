#pragma once
#include "LibraryView.h"
#include "DesignerView.h"
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
        library.bindPreferences(preferences);
        browserOpen=preferences.getBoolValue("browserOpen",true);browserWidth=preferences.getIntValue("browserWidth",300);
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
        if(both){int width=std::clamp(browserWidth,260,std::max(260,getWidth()/3));library.setBounds(0,0,width,getHeight());divider.setBounds(width,0,6,getHeight());designer.setBounds(width+6,0,getWidth()-width-6,getHeight());}
        else {library.setBounds(getLocalBounds());designer.setBounds(getLocalBounds());}
    }
    void showDesigner(bool yes)
    {
        designerActive=yes;preferences.setValue("designerActive",yes);resized();
        if(isShowing())(yes ? static_cast<juce::Component &>(designer) : static_cast<juce::Component &>(library)).grabKeyboardFocus();
    }
    bool keyPressed(const juce::KeyPress &key) override
    {
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
    bool designerActive=false,browserOpen=true;int browserWidth=300;
    struct Divider : juce::Component
    {
        Workstation &owner;int anchor=0;
        explicit Divider(Workstation &o):owner(o){setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);setTitle("Resize sound browser");}
        void paint(juce::Graphics &g) override{g.fillAll(sp::ui::background);g.setColour(isMouseOver() ? sp::ui::accent : sp::ui::line);g.drawVerticalLine(2,12,(float)getHeight()-12);}
        void mouseDown(const juce::MouseEvent &e) override{anchor=e.getEventRelativeTo(&owner).x-owner.browserWidth;}
        void mouseDrag(const juce::MouseEvent &e) override{owner.browserWidth=std::clamp(e.getEventRelativeTo(&owner).x-anchor,260,std::max(260,owner.getWidth()/3));owner.preferences.setValue("browserWidth",owner.browserWidth);owner.resized();}
    } divider;
    juce::TooltipWindow tooltips{this,600};
};
