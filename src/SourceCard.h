#pragma once
#include "DesignControls.h"
#include "WaveformPreview.h"
namespace sp
{
class SourceCard final : public juce::Component, public juce::SettableTooltipClient
{
  public:
    SourceCard(juce::Label &title, juce::ComboBox &chooser, std::shared_ptr<WaveformPool>);
    void bind(const DesignLayer *, const juce::String &assignmentId, const juce::String &alias, bool selected,
              double contribution);
    void transport(bool owned, int64_t frame, double designRate);
    void refreshPeaks();
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    std::function<void(const juce::String &)> onSelect, onEdit;
    juce::var dragPayload;
    juce::String stateText() const
    {
        return state;
    }
    juce::String layerId() const
    {
        return assigned;
    }
    std::optional<double> cursor() const
    {
        return playhead;
    }
    std::shared_ptr<const PeakColumns> displayedColumns() const
    {
        return geometry;
    }

  private:
    juce::Label &title;
    juce::ComboBox &chooser;
    std::shared_ptr<WaveformPool> pool;
    std::optional<DesignLayer> layer;
    juce::String assigned, binding, alias, state, filename;
    std::shared_ptr<const WaveformData> peaks;
    std::shared_ptr<const PeakColumns> geometry;
    std::optional<double> playhead;
    juce::var capturedDrag;
    bool selected = false, silent = false;
    double weight = 0;
    juce::Rectangle<float> waveBounds() const;
};
} // namespace sp
