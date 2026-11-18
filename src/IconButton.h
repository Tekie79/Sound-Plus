#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp
{
enum class ActionIcon { play,match,whole,previous,next,pause,refresh,folder,download,cancel,similar,file,zoom,fit,reconnect,markIn,markOut,scanFolder,retryAnalysis };
// Keep native button names and keyboard focus; indicate focus with colour, without a border.
class IconButton : public juce::TextButton
{
  public:
    IconButton(const juce::String &name,ActionIcon icon,bool label=false):juce::TextButton(name),kind(icon),showLabel(label)
    { setTitle(name);setTooltip(name);setMouseCursor(juce::MouseCursor::PointingHandCursor); }
    void setButtonText(const juce::String &name) {if(getTooltip()==getButtonText())setTooltip(name);juce::TextButton::setButtonText(name);setTitle(name);}
    void setPrimary(bool value=true) {primary=value;repaint();}
    void paintButton(juce::Graphics &g,bool over,bool down) override
    {
        auto bounds=getLocalBounds().toFloat().reduced(0.5f);
        auto radius=primary ? bounds.getHeight()*0.5f : 6.0f;
        if(primary || showLabel || over || down)
        {
            g.setColour(juce::Colour(primary ? (down ? 0xff92c4e8 : over || hasKeyboardFocus(false) ? 0xffb4dcf5 : 0xff9acceb) :
                down ? 0xff30414e : over ? 0xff24323d : 0xff1b252e).withMultipliedAlpha(isEnabled()?1.0f:0.35f));
            g.fillRoundedRectangle(bounds,radius);
        }
        juce::Path fill,outline;
        auto line=[&](float a,float b,float c,float d){outline.startNewSubPath(a,b);outline.lineTo(c,d);};
        auto triangle=[&](float offset=0) {fill.addTriangle(8+offset,5,8+offset,19,18+offset,12);};
        auto k=kind;
        if (k==ActionIcon::play && getButtonText().startsWith("Stop")) fill.addRoundedRectangle(6,6,12,12,2);
        else if (k==ActionIcon::play) triangle();
        else if (k==ActionIcon::match) {triangle();line(3,5,3,19);line(21,5,21,19);}
        else if (k==ActionIcon::whole) {triangle(1);line(4,5,4,19);}
        else if (k==ActionIcon::previous || k==ActionIcon::next)
        {triangle(-1);line(19,5,19,19);if(k==ActionIcon::previous){auto mirror=juce::AffineTransform::scale(-1,1).translated(24,0);fill.applyTransform(mirror);outline.applyTransform(mirror);} }
        else if (k==ActionIcon::pause)
        { if(getButtonText().startsWith("Resume"))triangle();else {fill.addRoundedRectangle(6,5,4,14,1);fill.addRoundedRectangle(14,5,4,14,1);} }
        else if (k==ActionIcon::cancel) {line(6,6,18,18);line(18,6,6,18);}
        else if (k==ActionIcon::folder)
        {outline.startNewSubPath(3,19);outline.lineTo(3,6);outline.lineTo(10,6);outline.lineTo(12,9);outline.lineTo(21,9);outline.lineTo(21,19);outline.closeSubPath();line(12,11,12,17);line(9,14,15,14);}
        else if (k==ActionIcon::scanFolder)
        {outline.startNewSubPath(11,19);outline.lineTo(3,19);outline.lineTo(3,5);outline.lineTo(10,5);outline.lineTo(12,8);outline.lineTo(20,8);outline.lineTo(20,10);outline.addEllipse(12,11,8,8);line(19,18,23,22);}
        else if (k==ActionIcon::retryAnalysis)
        {line(3,11,3,16);line(7,6,7,20);line(11,9,11,17);outline.addCentredArc(17,11,5,5,0,0.3f,5.6f,true);line(22,4,22,9);line(22,9,17,9);}
        else if (k==ActionIcon::reconnect)
        {line(9,3,9,7);line(15,3,15,7);outline.startNewSubPath(6,7);outline.lineTo(18,7);outline.lineTo(18,11);outline.cubicTo(18,18,6,18,6,11);outline.closeSubPath();line(12,16,12,21);line(12,21,19,21);}
        else if (k==ActionIcon::download)
        {if(getButtonText().contains("Installed")){line(5,12,10,17);line(10,17,20,6);}else{line(12,3,12,16);line(7,11,12,16);line(17,11,12,16);line(4,20,20,20);}}
        else if (k==ActionIcon::similar)
        {outline.addEllipse(3,3,12,12);line(14,14,21,21);line(6,9,6,11);line(9,6,9,12);line(12,8,12,10);}
        else if (k==ActionIcon::file)
        {outline.startNewSubPath(5,3);outline.lineTo(14,3);outline.lineTo(19,8);outline.lineTo(19,21);outline.lineTo(5,21);outline.closeSubPath();line(8,13,8,17);line(12,10,12,18);line(16,12,16,16);}
        else if (k==ActionIcon::zoom)
        {outline.addEllipse(3,3,12,12);line(14,14,21,21);line(6,9,12,9);line(9,6,9,12);}
        else if (k==ActionIcon::fit)
        {line(3,5,3,19);line(21,5,21,19);line(5,12,19,12);line(5,12,9,8);line(5,12,9,16);line(19,12,15,8);line(19,12,15,16);}
        else if (k==ActionIcon::markIn || k==ActionIcon::markOut)
        {
            outline.startNewSubPath(14,3);outline.lineTo(10,3);outline.lineTo(10,21);outline.lineTo(14,21);
            if(k==ActionIcon::markOut)outline.applyTransform(juce::AffineTransform::scale(-1,1).translated(24,0));
        }
        else
        {outline.addCentredArc(12,12,8,8,0,0.4f,5.7f,true);line(19,3,19,9);line(19,9,13,9);}
        auto font=juce::Font(juce::FontOptions(13.0f,juce::Font::bold));
        bool marker=k==ActionIcon::markIn || k==ActionIcon::markOut;
        float iconSize=std::min(marker ? 14.0f : 20.0f,getHeight()-10.0f),gap=showLabel ? 8.0f : 0.0f;
        float labelWidth=showLabel ? std::min(juce::GlyphArrangement::getStringWidth(font,getButtonText()),std::max(0.0f,getWidth()-iconSize-gap-16)) : 0;
        float start=(getWidth()-iconSize-gap-labelWidth)*0.5f;
        auto ink=fill.isEmpty() ? outline.getBounds().expanded(0.9f) : outline.isEmpty() ? fill.getBounds() : fill.getBounds().getUnion(outline.getBounds().expanded(0.9f));
        auto scale=std::min(iconSize/std::max(1.0f,ink.getWidth()),iconSize/std::max(1.0f,ink.getHeight()));
        auto transform=juce::AffineTransform::translation(-ink.getCentreX(),-ink.getCentreY()).scaled(scale).translated(start+iconSize*0.5f,getHeight()*0.5f);
        g.setColour(juce::Colour(primary ? 0xff101c25 : hasKeyboardFocus(false) ? 0xff77bfff : 0xffd2e9f5).withMultipliedAlpha(isEnabled()?1.0f:0.3f));
        g.fillPath(fill,transform);g.strokePath(outline,juce::PathStrokeType(1.8f),transform);
        if(showLabel){g.setFont(font);g.drawText(getButtonText(),juce::Rectangle<float>(start+iconSize+gap,0,labelWidth,(float)getHeight()),juce::Justification::centredLeft,false);}
    }
  private:
    ActionIcon kind;
    bool showLabel;
    bool primary=false;
};
}
