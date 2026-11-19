#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "Reference.h"
namespace sp
{
class LibraryView;
class DesignerView;
class Benchmark final : private juce::Timer
{
  public:
    Benchmark(LibraryView &, const juce::File &plan, const juce::File &output,
              DesignerView *designer = nullptr);
    void painted(uint64_t request, int rows, double dispatchMs, double appliedMs);

  private:
    void timerCallback() override;
    void save(bool success, const juce::String &error = {});
    void dispatch();
    LibraryView &view;
    DesignerView *designer = nullptr;
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
    void selectionAuditionTick(double now);
    void semanticTick(double now);
    void referenceTick(double now);
    Asset exampleSource;
    AudioReference exampleLast;
    bool referenceProgressSeen = false;
    double firstSemanticPaint = 0;
    juce::String semanticDemoId;
    int64_t pausedAiWindows = 0, semanticAudioFrames = 0;
    double semanticDemoRate = 0;
    int64_t semanticRangeStart = 0, semanticRangeEnd = 0;
    juce::Array<juce::var> thermalSamples;
    double lastThermalSample = 0;
    uint64_t semanticPageBefore = 0;
    uint64_t transportGeneration = 0;
    int64_t transportPosition = 0;
    int64_t semanticOtherMoment = 0;
    int semanticJobsBefore = 0;
    double lastDesignerInput = 0;
    int64_t lastDesignerFrame = 0;
    bool designerGestureEnded = false;
    int editingIndexOverlap = 0;
    int waveformScrolls = 0;
    int pendingWaveformScrolls = 0;
    int waveformScrollPaints = 0;
    bool waveformScrollAwaitingPaint = false;
    int64_t waveformScrollStartPaints = 0;
    double lastWaveformScroll = 0;
};
} // namespace sp
