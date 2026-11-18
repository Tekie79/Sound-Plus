#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
namespace sp
{
class LibraryView;
class Benchmark final : private juce::Timer
{
  public:
    Benchmark(LibraryView &, const juce::File &plan, const juce::File &output);
    void painted(uint64_t request, int rows, double dispatchMs, double appliedMs);

  private:
    void timerCallback() override;
    void save(bool success, const juce::String &error = {});
    void dispatch();
    LibraryView &view;
    juce::File output;
    juce::var plan;
    juce::Array<juce::var> samples, auditions, changes;
    double startMs = 0, phaseStart = 0, firstPaint = 0, firstNonempty = 0, sendMs = 0, appliedMs = 0;
    uint64_t waiting = 0;
    int phase = 0, index = 0, run = 0, peakPending = 0;
    bool completed = false;
    juce::String workload;
    double audioDispatch = 0;
    juce::var importStats;
    juce::var controls{new juce::DynamicObject()};
    uint64_t lastPainted = 0, controlRequest = 0;
    int lastPaintedOffset = -1, pausedCount = 0;
    juce::int64 pausedFrames = 0;
    juce::String controlSelectedId;
    void controlTick(int total, int done, double now);
};
} // namespace sp
