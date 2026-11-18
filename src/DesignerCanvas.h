#pragma once
#include "CanvasEditing.h"
#include "DesignSession.h"
#include "Waveform.h"
namespace sp
{
class DesignerCanvas : public juce::Component, private juce::Timer
{
  public:
    using Peaks = std::function<std::shared_ptr<const WaveformData>(const Asset &)>;
    DesignerCanvas(DesignSession &, Peaks, juce::PropertiesFile &);
    ~DesignerCanvas() override;
    TimelineViewport viewport;
    SnapSettings snapping;
    bool audibleScrub = false;
    void syncSelectors();
    void fitDesign();
    void fitSelection();
    void zoom(double factor);
    void zoomVertical(double factor);
    void chooseFadeShape(const juce::String &layer, bool in, juce::Component &anchor,
                         juce::Rectangle<int> area);
    std::function<void()> onRowsChanged;
    int contentHeight() const
    {
        return (int)std::ceil(viewport.contentHeight((int)session.document.activeMix().layers.size()));
    }
    void cancelDrag();
    void setDropGhost(std::optional<int64_t>);
    int64_t frameAt(float x) const
    {
        return viewport.frame(x, session.document.sampleRate);
    }
    bool dragging() const
    {
        return pointerDown;
    }
    juce::var interactionMetrics() const;
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    void mouseMagnify(const juce::MouseEvent &, float) override;
    void focusLost(FocusChangeType) override;

  private:
    struct Header;
    juce::OwnedArray<Header> headers;
    DesignSession &session;
    Peaks peaks;
    juce::PropertiesFile &preferences;
    juce::String headerSignature, documentId, feedback;
    bool pointerDown = false, moved = false, slip = false;
    bool needsInitialFit = true, restoredViewport = false;
    bool clipSelection = true, originalClipSelection = true;
    double verticalScale = 1, originalHeight = 78;
    CanvasHit hit, hover;
    DesignDocument original;
    std::set<juce::String> selection;
    TimelineViewport captured;
    juce::Point<float> anchor, pointer;
    juce::ModifierKeys modifiers;
    std::optional<int64_t> dropGhost;
    std::optional<bool> hoverFade;
    int insertion = -1;
    double lastTick = 0, inputTime = 0;
    std::vector<double> visualSamples;
    void applyDrag();
    void updateHover(juce::Point<float>);
    void timerCallback() override;
    void saveViewport();
    void updateRows();
    juce::String heightKey(const juce::String &) const;
    void showColours(const juce::String &);
};
} // namespace sp
