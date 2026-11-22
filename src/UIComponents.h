#pragma once
#include "IconButton.h"
namespace sp::ui
{
class LookAndFeel : public juce::LookAndFeel_V4
{
  public:
    LookAndFeel()
    {
        icons::preload();
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::TextEditor::backgroundColourId, surface);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::TextEditor::focusedOutlineColourId, accent);
        setColour(juce::ComboBox::backgroundColourId, surface);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::outlineColourId, line);
        setColour(juce::Slider::thumbColourId, accent);
        setColour(juce::Slider::trackColourId, accent);
        setColour(juce::Slider::backgroundColourId, raised);
        setColour(juce::PopupMenu::backgroundColourId, surface);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, raised);
        setColour(juce::ListBox::backgroundColourId, surface);
        setColour(juce::ListBox::textColourId, text);
    }
    void drawComboBox(juce::Graphics &g, int width, int height, bool, int, int, int, int,
                      juce::ComboBox &box) override
    {
        g.setColour(surface);
        g.fillRoundedRectangle(.5f, .5f, width - 1.f, height - 1.f, 6);
        g.setColour(box.hasKeyboardFocus(true) ? button::focus : line);
        g.drawRoundedRectangle(.5f, .5f, width - 1.f, height - 1.f, 6, 1);
        const float arrowX = width <= 32 ? (width - 13.f) * .5f : width - 23.f;
        icons::draw(g, ActionIcon::chevronDown, {arrowX, (height - 13.f) * .5f, 13, 13},
                    box.isEnabled() ? icons::Tone::normal : icons::Tone::disabled);
    }
    void drawLinearSlider(juce::Graphics &g, int x, int y, int width, int height, float pos, float minPos,
                          float maxPos, const juce::Slider::SliderStyle style, juce::Slider &s) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, pos, minPos, maxPos, style, s);
            return;
        }
        float middle = y + height * .5f;
        g.setColour(raised);
        g.fillRoundedRectangle((float)x, middle - 2, (float)width, 4, 2);
        g.setColour(accent.withMultipliedAlpha(s.isEnabled() ? 1.f : .25f));
        g.fillRoundedRectangle((float)x, middle - 2, std::max(1.f, pos - x), 4, 2);
        g.fillEllipse(pos - 4, middle - 4, 8, 8);
    }
};
class FileDragButton : public IconButton
{
  public:
    explicit FileDragButton(const juce::String &name) : IconButton(name, ActionIcon::drag)
    {
        auto path = juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_QA_DRAG_LOG", {});
        if (path.isNotEmpty())
            traceFile = juce::File(path);
    }
    juce::File file;
    std::function<juce::File()> resolveFile;
    std::function<juce::StringArray()> resolveFiles;
    juce::StringArray files;
    void mouseDown(const juce::MouseEvent &e) override
    {
        if (resolveFile)
            file = resolveFile();
        files = resolveFiles ? resolveFiles() : juce::StringArray{file.getFullPathName()};
        started = false;
        tracedDrag = false;
        trace("mouse-down", e.getDistanceFromDragStart());
        IconButton::mouseDown(e);
    }
    void mouseDrag(const juce::MouseEvent &e) override
    {
        if (!tracedDrag && e.getDistanceFromDragStart() > 5)
        {
            trace("mouse-drag", e.getDistanceFromDragStart());
            tracedDrag = true;
        }
        if (!started && isEnabled() && !files.isEmpty() && file != juce::File() && file.existsAsFile() &&
            std::all_of(files.begin(), files.end(), [](auto &p) { return juce::File(p).existsAsFile(); }) &&
            e.getDistanceFromDragStart() > 5)
        {
            std::function<void()> ended;
            if (traceFile != juce::File())
            {
                juce::Component::SafePointer<FileDragButton> safe(this);
                ended = [safe]
                {
                    if (safe)
                        safe->trace("native-ended", 0);
                };
            }
            started = juce::DragAndDropContainer::performExternalDragDropOfFiles(files, false, this,
                                                                                 std::move(ended));
            trace(started ? "native-started" : "native-start-failed", e.getDistanceFromDragStart());
        }
    }

  private:
    bool started = false;
    bool tracedDrag = false;
    juce::File traceFile;
    void trace(const char *event, int distance)
    {
        if (traceFile == juce::File())
            return;
        auto row = juce::var(new juce::DynamicObject());
        auto *o = row.getDynamicObject();
        o->setProperty("event", event);
        o->setProperty("time", juce::Time::getCurrentTime().toISO8601(true));
        o->setProperty("control", getButtonText());
        o->setProperty("file", file.getFullPathName());
        o->setProperty("exists", file.existsAsFile());
        o->setProperty("enabled", isEnabled());
        o->setProperty("distance", distance);
        o->setProperty("bounds", getScreenBounds().toString());
        auto p = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition();
        o->setProperty("screenX", p.x);
        o->setProperty("screenY", p.y);
        traceFile.appendText(juce::JSON::toString(row, true) + "\n");
    }
};
inline juce::PropertiesFile::Options preferenceOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "SoundPlus";
    o.filenameSuffix = "settings";
    o.millisecondsBeforeSaving = 1000;
    return o;
}
} // namespace sp::ui
