#pragma once
#include "IconButton.h"
namespace sp::ui
{
inline const juce::Colour background{0xff101419},surface{0xff171d25},raised{0xff202b35},line{0xff293440},text{0xffe6ecf2},muted{0xff98a8b8},accent{0xff91cbe8};
inline constexpr int gap=8,control=30,radius=6;
class LookAndFeel : public juce::LookAndFeel_V4
{
  public:
    LookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId,background);
        setColour(juce::TextEditor::backgroundColourId,surface);setColour(juce::TextEditor::textColourId,text);
        setColour(juce::TextEditor::outlineColourId,juce::Colours::transparentBlack);setColour(juce::TextEditor::focusedOutlineColourId,accent);
        setColour(juce::ComboBox::backgroundColourId,surface);setColour(juce::ComboBox::textColourId,text);setColour(juce::ComboBox::outlineColourId,line);
        setColour(juce::Slider::thumbColourId,accent);setColour(juce::Slider::trackColourId,accent);setColour(juce::Slider::backgroundColourId,raised);
        setColour(juce::PopupMenu::backgroundColourId,surface);setColour(juce::PopupMenu::textColourId,text);setColour(juce::PopupMenu::highlightedBackgroundColourId,raised);
        setColour(juce::ListBox::backgroundColourId,surface);setColour(juce::ListBox::textColourId,text);
    }
    void drawLinearSlider(juce::Graphics &g,int x,int y,int width,int height,float pos,float minPos,float maxPos,const juce::Slider::SliderStyle style,juce::Slider &s) override
    {
        if(style!=juce::Slider::LinearHorizontal){LookAndFeel_V4::drawLinearSlider(g,x,y,width,height,pos,minPos,maxPos,style,s);return;}
        float middle=y+height*.5f;g.setColour(raised);g.fillRoundedRectangle((float)x,middle-2,(float)width,4,2);g.setColour(accent.withMultipliedAlpha(s.isEnabled()?1.f:.25f));g.fillRoundedRectangle((float)x,middle-2,std::max(1.f,pos-x),4,2);g.fillEllipse(pos-4,middle-4,8,8);
    }
};
class FileDragButton : public IconButton
{
  public:
    explicit FileDragButton(const juce::String &name):IconButton(name,ActionIcon::drag){}
    juce::File file;
    void mouseDown(const juce::MouseEvent &e) override {started=false;IconButton::mouseDown(e);}
    void mouseDrag(const juce::MouseEvent &e) override
    {
        if(!started && isEnabled() && file!=juce::File() && e.getDistanceFromDragStart()>5)
            started=juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()},false,this);
    }
  private:bool started=false;
};
inline juce::PropertiesFile::Options preferenceOptions(){juce::PropertiesFile::Options o;o.applicationName="SoundPlus";o.filenameSuffix="settings";o.millisecondsBeforeSaving=1000;return o;}
}
