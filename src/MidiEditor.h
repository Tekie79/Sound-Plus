#pragma once
#include "DesignSession.h"
#include "UIComponents.h"
namespace sp
{
class MidiEditor final : public juce::Component
{
  public:
    explicit MidiEditor(DesignSession &, juce::File registry = {});
    ~MidiEditor() override;
    void open(const juce::String &, bool focus = true);
    void refresh();
    void instrument(const juce::String &, juce::Component &anchor);
    std::function<void()> onClose;
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void focusLost(FocusChangeType) override;
    void quantize();
    void transpose(int);
    void changeVelocity(int);
    void copyNotes();
    void pasteNotes();
    void eraseNotes();
    void drawNote(int64_t tick, int pitch, int velocity = 100);
    const std::set<juce::String> &selection() const
    {
        return selected;
    }

  private:
    DesignSession &session;
    juce::File registry;
    juce::String clip, document, variation;
    uint64_t incarnation = 0;
    std::set<juce::String> selected;
    std::vector<MidiNote> clipboard;
    MidiClip before;
    juce::Point<float> anchor;
    juce::String dragging;
    bool gesture = false, resizeNote = false, velocityDrag = false, syncing = false;
    int lowest = 48, highest = 72, heldKey = -1;
    juce::String heldOwner;
    double beats = 8, startBeat = 0;
    juce::Rectangle<float> grid, velocities;
    struct CompactLook : ui::LookAndFeel
    {
        juce::Font getComboBoxFont(juce::ComboBox &) override
        {
            return juce::Font(juce::FontOptions(11.f));
        }
    } look;
    juce::Label title;
    juce::ComboBox tool, snap, controller;
    juce::Slider strength;
    IconButton close{"Close piano roll", ActionIcon::cancel}, settings{"Instrument", ActionIcon::settings},
        quantizeButton{"Quantize selected notes", ActionIcon::snap},
        zoomIn{"Zoom piano roll in", ActionIcon::zoom}, zoomOut{"Zoom piano roll out", ActionIcon::zoomOut};
    std::unique_ptr<juce::FileChooser> chooser;
    bool valid() const;
    const MidiClip *model() const;
    int64_t tick(float) const;
    float x(int64_t) const;
    int pitch(float) const;
    juce::Rectangle<float> noteRect(const MidiNote &) const;
    int64_t quantum() const;
    void mutate(const juce::String &, std::function<void(MidiClip &)>);
    void finish();
};
} // namespace sp
