#pragma once
#include "DesignSession.h"
#include "WaveformPreview.h"
#include "DesignControls.h"
namespace sp
{
class SourcePoolView final : public juce::Component,
                             private juce::ListBoxModel,
                             private juce::Timer,
                             public juce::DragAndDropTarget,
                             public juce::FileDragAndDropTarget
{
  public:
    SourcePoolView(DesignSession &, std::shared_ptr<WaveformPool>, juce::PropertiesFile &,
                   int demandOwner = 6);
    ~SourcePoolView() override;
    void bind();
    void resized() override;
    void paint(juce::Graphics &) override;
    int preferredHeight() const;
    void expandForPopup()
    {
        expanded = true;
        bind();
    }
    std::function<void()> onHeightChanged, onAdd;
    std::function<void(const juce::String &)> onReview, onRelocate;
    std::optional<Asset> foregroundSource() const;
    juce::var metrics() const;
    void navigate(int);
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;

  private:
    friend struct SourcePoolViewTestAccess;
    friend class Benchmark;
    DesignSession &session;
    std::shared_ptr<WaveformPool> peaks;
    juce::PropertiesFile &preferences;
    IconButton disclosure{"Alternatives", ActionIcon::chevron, true},
        add{"Add alternatives", ActionIcon::addLayer}, remove{"Remove alternative", ActionIcon::trash},
        previous{"Previous alternative", ActionIcon::previous}, next{"Next alternative", ActionIcon::next},
        lock{"Lock source choice for variations", ActionIcon::lock},
        more{"Alternative options", ActionIcon::settings};
    juce::ListBox list{"Source alternatives", this};
    bool expanded = false, updating = false, readOnly = false;
    juce::String binding, layerId, signature;
    int cachedHeight = 30;
    struct Preview
    {
        std::shared_ptr<const WaveformData> data;
        std::shared_ptr<const PeakColumns> geometry;
        juce::String state;
        double painted = 0;
    };
    std::map<juce::String, Preview> rows;
    std::vector<double> selectedMs, viewportMs;
    double selectionStarted = 0, viewportStarted = 0;
    uint64_t cancellations = 0, bindings = 0;
    int demandOwner = 6;
    juce::String measuredSelection, measuredViewport;
    DesignLayer *layer() const;
    const SourcePoolEntry *entry(int) const;
    int activeIndex() const;
    juce::String rowName(int) const;
    void activate(int);
    void removeSelected();
    void options();
    int getNumRows() override;
    juce::String getNameForRow(int row) override
    {
        return rowName(row);
    }
    void paintListBoxItem(int, juce::Graphics &, int, int, bool) override;
    juce::String getTooltipForRow(int) override;
    void listBoxItemClicked(int, const juce::MouseEvent &) override;
    void returnKeyPressed(int row) override
    {
        activate(row);
    }
    void deleteKeyPressed(int) override
    {
        removeSelected();
    }
    void timerCallback() override;
};
} // namespace sp
