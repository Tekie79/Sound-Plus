#pragma once
#include "IconButton.h"
#include "Catalog.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <condition_variable>
#include <thread>
namespace sp
{
// Markers are original-source samples; OUT stays exclusive and every range contains a sample.
std::pair<int64_t, int64_t> markRangeAtPlayhead(int64_t first, int64_t last, int64_t cursor, int64_t total,
                                                bool markIn);
struct WaveformData
{
    enum class Status { ready, offline, changed, decodeError };
    Status status = Status::ready;
    juce::String assetId, error;
    int64_t revision = 0, samples = 0;
    double rate = 0;
    uint64_t generation = 0;
    std::vector<std::pair<float, float>> peaks;
    bool cacheHit = false;
};
class WaveformCache
{
  public:
    using Checkpoint = std::function<void(const Asset &, int64_t, const std::function<bool()> &)>;
    explicit WaveformCache(juce::File directory, std::function<void()> beforeRead = {}, Checkpoint = {});
    ~WaveformCache();
    uint64_t request(const Asset &, std::shared_ptr<const WaveformData> validate = {});
    uint64_t requestRange(const Asset &, juce::Range<int64_t> samples, int bins);
    uint64_t cancel();
    std::shared_ptr<const WaveformData> snapshot();

  private:
    struct State;
    static void run(const std::shared_ptr<State> &);
    std::shared_ptr<State> state;
    std::thread worker;
};
class WaveformView final : public juce::Component, private juce::Timer
{
  public:
    WaveformView();
    void setData(std::shared_ptr<const WaveformData>, const Asset *source = nullptr);
    void setRange(int64_t start, int64_t end, bool zoom = false, bool selected = true);
    bool isRangeSelected() const
    {
        return rangeSelected;
    }
    void setCursor(int64_t sample);
    void markAtCursor(bool markIn);
    void showWhole();
    void zoomRange();
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    int64_t startSample() const
    {
        return first;
    }
    int64_t endSample() const
    {
        return last;
    }
    int64_t cursorSample() const
    {
        return std::max<int64_t>(0, cursor);
    }
    IconButton &markInButton()
    {
        return in;
    }
    IconButton &markOutButton()
    {
        return out;
    }
    std::shared_ptr<const WaveformData> data() const
    {
        return waveform;
    }
    std::function<void(int64_t, int64_t)> onRange;
    std::function<void()> onClearRange;
    std::function<void(int64_t)> onSeek;
    std::function<void()> onEditRange;

  private:
    friend struct LibraryPreviewTestAccess;
    void refreshDetail();
    void timerCallback() override;
    int64_t sampleAt(float x) const;
    float xAt(int64_t sample) const;
    std::shared_ptr<const WaveformData> waveform;
    std::optional<Asset> detailSource;
    std::unique_ptr<WaveformCache> detailWorker;
    juce::Path detailOutline;
    uint64_t detailRequest = 0;
    juce::Range<int64_t> detailRange;
    int detailBins = 0;
    int64_t first = 0, last = 0, viewFirst = 0, viewLast = 1, cursor = -1, anchor = 0;
    int dragMode = 0;
    bool dragging = false;
    bool rangeSelected = false;
    IconButton zoom{"Zoom Range", ActionIcon::zoom}, all{"Show All", ActionIcon::fit};
    IconButton in{"Set IN at playhead", ActionIcon::markIn}, out{"Set OUT at playhead", ActionIcon::markOut};
};
} // namespace sp
