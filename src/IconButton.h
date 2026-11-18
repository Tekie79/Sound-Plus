#pragma once
#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp
{
enum class ActionIcon { play,match,whole,previous,next,pause,refresh,folder,download,cancel,similar,file,zoom,fit,reconnect,markIn,markOut,scanFolder,retryAnalysis,addLayer,duplicate,up,down,trash,save,open,undo,redo,variations,lock,render,drag,keep,settings,layers,browser,chevron,mute,solo,newDocument,rename,history,compare,zoomOut,fitSelection,snap,scrub };
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
        else if(k==ActionIcon::zoomOut){outline.addEllipse(3,3,13,13);line(7,9.5f,12,9.5f);line(15,15,22,22);}
        else if(k==ActionIcon::fitSelection){line(4,5,4,19);line(20,5,20,19);line(8,12,16,12);line(8,12,11,9);line(8,12,11,15);line(16,12,13,9);line(16,12,13,15);}
        else if(k==ActionIcon::snap){outline.startNewSubPath(5,5);outline.lineTo(5,13);outline.cubicTo(5,23,19,23,19,13);outline.lineTo(19,5);line(5,9,9,9);line(15,9,19,9);line(9,5,9,13);line(15,5,15,13);}
        else if(k==ActionIcon::scrub){line(5,8,5,17);line(9,5,9,20);line(13,9,13,16);line(17,6,17,19);line(21,10,21,15);}

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
        else if(k==ActionIcon::newDocument){outline.addRectangle(4,3,16,18);line(8,12,16,12);line(12,8,12,16);}
        else if(k==ActionIcon::rename){outline.startNewSubPath(4,20);outline.lineTo(6,13);outline.lineTo(16,3);outline.lineTo(21,8);outline.lineTo(11,18);outline.closeSubPath();line(14,5,19,10);}
        else if(k==ActionIcon::history){outline.addCentredArc(12,12,9,9,0,.1f,5.7f,true);line(12,6,12,12);line(12,12,16,14);line(3,3,3,9);line(3,9,9,9);}
        else if(k==ActionIcon::compare){outline.addRectangle(3,4,18,16);line(12,4,12,20);line(5,8,10,8);line(14,16,19,16);}
        else if(k==ActionIcon::browser){outline.addRectangle(3,4,18,16);line(9,4,9,20);line(5,8,7,8);line(5,12,7,12);}
        else if(k==ActionIcon::chevron){line(8,5,15,12);line(15,12,8,19);if(getToggleState())outline.applyTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi,12,12));}
        else if(k==ActionIcon::mute){outline.startNewSubPath(4,9);outline.lineTo(8,9);outline.lineTo(13,5);outline.lineTo(13,19);outline.lineTo(8,15);outline.lineTo(4,15);outline.closeSubPath();line(17,8,22,16);line(22,8,17,16);}
        else if(k==ActionIcon::solo){outline.addCentredArc(12,12,8,8,0,-juce::MathConstants<float>::halfPi,juce::MathConstants<float>::halfPi,true);outline.addRoundedRectangle(3,11,4,9,2);outline.addRoundedRectangle(17,11,4,9,2);}
        else if(k==ActionIcon::layers){outline.addRectangle(3,3,18,4);outline.addRectangle(3,10,18,4);outline.addRectangle(3,17,18,4);}
        else if(k==ActionIcon::addLayer){line(3,5,16,5);line(3,10,16,10);line(3,15,11,15);line(17,13,17,21);line(13,17,21,17);}
        else if(k==ActionIcon::duplicate){outline.addRectangle(4,4,11,11);outline.addRectangle(9,9,11,11);}
        else if(k==ActionIcon::up || k==ActionIcon::down){line(6,13,12,7);line(12,7,18,13);line(12,7,12,20);if(k==ActionIcon::down)outline.applyTransform(juce::AffineTransform::scale(1,-1).translated(0,24));}
        else if(k==ActionIcon::trash){line(4,6,20,6);line(9,3,15,3);line(7,7,8,21);line(8,21,16,21);line(16,21,17,7);line(10,10,10,17);line(14,10,14,17);}
        else if(k==ActionIcon::save){outline.addRectangle(4,3,16,18);outline.addRectangle(8,3,8,6);outline.addRectangle(8,14,8,7);}
        else if(k==ActionIcon::open){outline.startNewSubPath(3,19);outline.lineTo(3,5);outline.lineTo(10,5);outline.lineTo(12,8);outline.lineTo(20,8);outline.lineTo(20,11);outline.startNewSubPath(3,19);outline.lineTo(7,11);outline.lineTo(22,11);outline.lineTo(18,19);outline.closeSubPath();}
        else if(k==ActionIcon::undo || k==ActionIcon::redo){outline.startNewSubPath(5,9);outline.cubicTo(20,3,25,22,10,20);line(5,9,5,3);line(5,9,11,11);if(k==ActionIcon::redo)outline.applyTransform(juce::AffineTransform::scale(-1,1).translated(24,0));}
        else if(k==ActionIcon::variations){outline.addRoundedRectangle(3,3,18,18,3);for(auto p:{juce::Point<float>(7,7),{17,7},{12,12},{7,17},{17,17}})fill.addEllipse(p.x-1,p.y-1,2,2);}
        else if(k==ActionIcon::lock){outline.addRoundedRectangle(5,10,14,11,2);outline.addCentredArc(12,10,4,6,0,juce::MathConstants<float>::pi*1.5f,juce::MathConstants<float>::pi*2.5f,true);line(12,14,12,17);}
        else if(k==ActionIcon::render){line(12,3,12,16);line(7,11,12,16);line(17,11,12,16);line(4,16,4,21);line(4,21,20,21);line(20,21,20,16);}
        else if(k==ActionIcon::drag){outline.addRectangle(3,10,11,11);line(10,14,21,3);line(14,3,21,3);line(21,3,21,10);}
        else if(k==ActionIcon::keep){fill.addStar({12,12},5,4,10,0);}
        else if(k==ActionIcon::settings){outline.addEllipse(4,4,16,16);outline.addEllipse(9,9,6,6);for(int i=0;i<8;++i){float a=i*juce::MathConstants<float>::pi/4;line(12+8*std::cos(a),12+8*std::sin(a),12+11*std::cos(a),12+11*std::sin(a));}}
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
        g.setColour(juce::Colour(primary ? 0xff101c25 : hasKeyboardFocus(false) || getToggleState() ? 0xff77bfff : 0xffd2e9f5).withMultipliedAlpha(isEnabled()?1.0f:0.3f));
        g.fillPath(fill,transform);g.strokePath(outline,juce::PathStrokeType(1.8f),transform);
        if(showLabel){g.setFont(font);g.drawText(getButtonText(),juce::Rectangle<float>(start+iconSize+gap,0,labelWidth,(float)getHeight()),juce::Justification::centredLeft,false);}
    }
  private:
    ActionIcon kind;
    bool showLabel;
    bool primary=false;
};
}
