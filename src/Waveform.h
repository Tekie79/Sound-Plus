#pragma once
#include "Catalog.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <condition_variable>
#include <thread>
namespace sp
{
struct WaveformData
{
    juce::String assetId, error;
    int64_t revision = 0, samples = 0;
    double rate = 0;
    uint64_t generation = 0;
    std::vector<std::pair<float,float>> peaks;
    bool cacheHit = false;
};
class WaveformCache
{
  public:
    explicit WaveformCache(juce::File directory);
    ~WaveformCache();
    uint64_t request(const Asset &);
    std::shared_ptr<const WaveformData> snapshot();
  private:
    void run();
    juce::File directory;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Asset> pending;
    std::shared_ptr<const WaveformData> result;
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> stopping{false};
};
class WaveformView final : public juce::Component
{
  public:
    WaveformView();
    void setData(std::shared_ptr<const WaveformData>);
    void setRange(int64_t start, int64_t end, bool zoom = false);
    void setCursor(int64_t sample);
    void showWhole();
    void zoomRange();
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    int64_t startSample() const { return first; }
    int64_t endSample() const { return last; }
    std::shared_ptr<const WaveformData> data() const { return waveform; }
    std::function<void(int64_t,int64_t)> onRange;
    std::function<void(int64_t)> onSeek;
  private:
    int64_t sampleAt(float x) const;
    float xAt(int64_t sample) const;
    std::shared_ptr<const WaveformData> waveform;
    int64_t first = 0, last = 0, viewFirst = 0, viewLast = 1, cursor = -1, anchor = 0;
    int dragMode = 0;
    bool dragging = false;
    juce::TextButton zoom{"Zoom Range"}, all{"Show All"};
};
} // namespace sp
