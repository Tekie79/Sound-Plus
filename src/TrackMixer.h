#pragma once
#include "DesignControls.h"
#include "MultitrackEditor.h"

namespace sp
{
class TrackMixer final : public juce::Component
{
  public:
    TrackMixer(DesignSession &, MultitrackEditor &, juce::PropertiesFile &);
    ~TrackMixer() override;
    void refresh();
    void updateMeters(const juce::var &status);
    void stemMenu(juce::Component &anchor);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    std::function<void(const juce::String &)> showInserts;
    std::function<void(int)> resizeDock;

  private:
    struct Strip;
    DesignSession &session;
    MultitrackEditor &timeline;
    juce::PropertiesFile &preferences;
    juce::Viewport viewport;
    juce::Component stripRow;
    juce::OwnedArray<Strip> strips;
    std::unique_ptr<Strip> master;
    ui::TextActionButton density{"Compact", true}, stems{"Stems"};
    juce::StringArray ids;
    uint64_t incarnation = 0;
    juce::String variation;
    int dragHeight = 0;
    bool compact = false;
    void sendMenu(const juce::String &owner, juce::Component &anchor);
};
} // namespace sp
