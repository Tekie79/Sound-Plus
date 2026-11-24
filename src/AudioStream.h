#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include "DesignAudio.h"
#include "SerialWorker.h"
#include "InputRecording.h"
namespace sp
{
// Monitoring policy only. Preparation and scheduling remain on the PCM producer.
int midiLookaheadFrames(double outputRate, int callbackFrames, double producerMaxMs);
// Single producer / single consumer. All decoding and resampling run in fill().
class PcmStream
{
  public:
    uint64_t transportGeneration = 0;
    PcmStream(const juce::File &, double outputRate, int64_t sourceStart = 0, int64_t sourceEnd = -1,
              int auditionPair = 0, ChannelLayout interpretation = {}, bool loop = false,
              int64_t initialPosition = -1);
    PcmStream(
        const DesignDocument &, double outputRate, int64_t first, const juce::File &capture = {},
        const std::function<bool()> &cancelled = [] { return false; }, int64_t warmup = -1, int64_t last = -1,
        bool grain = false);
    void fill(int budget = 16384, const std::function<bool()> &cancelled = [] { return false; });
    void updateParameters(const juce::var &, uint64_t sequence, double eventMs);
    juce::var automationReceipt(bool close) {return designReader?designReader->automationReceipt(close):juce::var();}
    bool squareTouch(const juce::var &command)
    {
        if(!designReader || takeCapture || previewGrain)return false;
        return command["scalar"].isBool() && (bool)command["scalar"] ? designReader->automationCommand(command) : designReader->squareTouch(command);
    }
    juce::var squareTouchReceipt(bool close = false)
    {
        return designReader ? designReader->squareTouchReceipt(close) : juce::var();
    }
    int64_t injectMidi(const juce::String &owner, MidiPacket p)
    {
        return designReader ? designReader->injectMidi(owner, p) : p.frame;
    }
    void midiPanic(const juce::String &owner)
    {
        if (designReader)
            designReader->midiPanic(owner);
    }
    void startTake(std::shared_ptr<TakeCapture> value)
    {
        takeCapture = std::move(value);
    }
    void updatePerformance(const PerformControls &c)
    {
        if (designReader && takeCapture)
            designReader->updatePerformance(c, *takeCapture);
    }
    std::shared_ptr<TakeCapture> takeCapture;
    int64_t acceptedFrame() const
    {
        auto n = readIndex.load();
        return n ? (int64_t)std::ceil((long double)(n - 1) * sourceRate / rate) + 1 : 0;
    }
    int64_t consumedOutputFrames() const
    {
        return (int64_t)readIndex.load();
    }
    int64_t producedFrame() const
    {
        return designReader ? designReader->producedFrame() : outputFrame;
    }
    juce::var nativeMeters() const
    {
        return designReader ? designReader->nativeMeters() : juce::var();
    }
    juce::var mixerMeters() const
    {
        return designReader ? designReader->mixerMeters() : juce::var();
    }
    void discardTo(int64_t sourceFrame);
    std::atomic<uint64_t> heardEdit{0};
    std::atomic<double> editLatencyMs{0};
    int read(float *const *output, int channels, int frames, float &gain, float targetGain,
             const std::array<std::atomic<int>, 16> *mapping = nullptr, int solo = -1) noexcept;
    int channelCount() const
    {
        return pcmChannels;
    }
    ChannelLayout layout() const
    {
        return busLayout;
    }
    std::array<std::atomic<float>, 16> channelPeaks{};
    double seconds() const
    {
        if (repeating)
            return sourceSample() / sourceRate;
        return (double)sourceStart / sourceRate + (double)readIndex.load() / rate;
    }
    int64_t sourceSample() const
    {
        if (repeating)
            return std::min(sourceEnd - 1,
                            sourceStart + (int64_t)(((readIndex.load() + initialOffset) %
                                                     (uint64_t)std::max<int64_t>(1, outputLength)) *
                                                    ratio));
        return std::min(sourceEnd, sourceStart + (int64_t)(readIndex.load() * ratio));
    }
    int64_t startSample() const
    {
        return sourceStart;
    }
    int64_t endSample() const
    {
        return sourceEnd;
    }
    double originalRate() const
    {
        return sourceRate;
    }
    double outputRate() const
    {
        return rate;
    }
    int channelPair() const
    {
        return pair;
    }
    bool ended() const
    {
        return eof.load() && readIndex.load() == writeIndex.load();
    }
    uint64_t loopCount() const
    {
        return repeating ? (readIndex.load() + initialOffset) / (uint64_t)std::max<int64_t>(1, outputLength)
                         : 0;
    }
    int available() const
    {
        return (int)(writeIndex.load() - readIndex.load());
    }
    int64_t decodedReads() const
    {
        return reads;
    }
    std::atomic<double> firstCallbackMs{0};
    std::atomic<float> preMonitorPeak{0};
    bool isDesign() const
    {
        return designReader != nullptr;
    }
    int designLayers() const
    {
        return designReader ? designReader->layers() : 0;
    }
    bool hasHostedServices() const
    {
        return hostedServices;
    }
    double producerMaxMs() const
    {
        return maxFillMs;
    }
    double producerP95UpperMs() const;
    uint64_t producerFills() const
    {
        return fillCount;
    }

  private:
    static constexpr uint64_t capacity = 131072;
    std::vector<std::vector<float>> pcm;
    int pcmChannels = 2;
    ChannelLayout busLayout;
    ChannelMatrix monitor{};
    std::atomic<uint64_t> readIndex{0}, writeIndex{0};
    std::array<uint64_t, capacity> editTags{};
    std::array<double, capacity> editEvents{};
    uint64_t editSequence = 0;
    double editEvent = 0;
    bool previewGrain = false;
    std::atomic<bool> eof{false};
    juce::AudioFormatManager formats;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> cache{2, 8192};
    int64_t cacheStart = -1, outputFrame = 0, outputLength = 0, reads = 0;
    bool repeating = false;
    uint64_t initialOffset = 0;
    double rate = 48000, ratio = 1;
    int64_t sourceStart = 0, sourceEnd = 0;
    double sourceRate = 48000;
    int pair = 0;
    std::unique_ptr<DesignReader> designReader;
    juce::AudioBuffer<float> designBlock{2, 4096};
    std::unique_ptr<juce::FileOutputStream> capture;
    std::vector<float> captureData;
    // Producer and graph-preparation threads own these samples. The device callback never uses them.
    static constexpr double fillBucketMs = .05;
    std::array<uint64_t, 128> fillHistogram{};
    uint64_t fillCount = 0;
    double maxFillMs = 0;
    void recordProducerFill(double ms);
    bool hostedServices = false;
};
class AuditionCore
{
  public:
    InputTimelineClock *inputClock = nullptr;
    AuditionCore()
    {
        for (auto &m : monitorMapping)
            m = -2;
    }
    std::array<std::atomic<int>, 16> monitorMapping;
    std::atomic<int> monitorSolo{-1};
    std::array<std::atomic<float>, 16> channelPeaks{};
    int channelCount() const
    {
        return stream ? stream->channelCount() : 0;
    }
    ChannelLayout layout() const
    {
        return stream ? stream->layout() : ChannelLayout{};
    }
    void open(const juce::File &, double sampleRate, int64_t startSample = 0, int64_t endSample = -1,
              int auditionPair = 0);
    void openDesign(const DesignDocument &, double sampleRate, int64_t first = 0,
                    const juce::File &capture = {});
    void stop();
    void service(int ahead = 1536);
    PcmStream *producerStream()
    {
        return stream.get();
    }
    void adopt(std::unique_ptr<PcmStream>);
    void updateParameters(const juce::var &p, uint64_t sequence, double event)
    {
        if (stream)
            stream->updateParameters(p, sequence, event);
    }
    void updatePerformance(const PerformControls &c)
    {
        if (stream)
            stream->updatePerformance(c);
    }
    std::shared_ptr<TakeCapture> capture() const
    {
        return stream ? stream->takeCapture : nullptr;
    }
    int64_t finishTake(bool success);
    int64_t consumedOutputFrames() const
    {
        return stream ? stream->consumedOutputFrames() : 0;
    }
    int64_t acceptedSourceEnd() const
    {
        return stream ? stream->startSample() + stream->acceptedFrame() : 0;
    }
    uint64_t heardEdit() const
    {
        return stream ? stream->heardEdit.load() : 0;
    }
    double editLatencyMs() const
    {
        return stream ? stream->editLatencyMs.load() : 0;
    }
    void render(float *const *, int channels, int frames, uint64_t outputHostNs = 0) noexcept;
    bool playing() const
    {
        return stream && !stream->ended();
    }
    bool completed() const
    {
        return stream && stream->ended();
    }
    uint64_t loopCount() const
    {
        return stream ? stream->loopCount() : 0;
    }
    double seconds() const
    {
        return stream ? stream->seconds() : 0;
    }
    double firstCallbackMs() const
    {
        return stream ? stream->firstCallbackMs.load() : 0;
    }
    std::atomic<float> volume{0.7f}, peak{0};
    int64_t sourceSample() const
    {
        return stream ? stream->sourceSample() : 0;
    }
    int64_t startSample() const
    {
        return stream ? stream->startSample() : 0;
    }
    int64_t endSample() const
    {
        return stream ? stream->endSample() : 0;
    }
    double sourceRate() const
    {
        return stream ? stream->originalRate() : 0;
    }
    double outputRate() const
    {
        return stream ? stream->outputRate() : 0;
    }
    int channelPair() const
    {
        return stream ? stream->channelPair() : 0;
    }
    bool designPlaying() const
    {
        return stream && stream->isDesign();
    }
    int designLayers() const
    {
        return stream ? stream->designLayers() : 0;
    }
    double producerMaxMs() const
    {
        return stream ? stream->producerMaxMs() : 0;
    }
    double producerP95UpperMs() const
    {
        return stream ? stream->producerP95UpperMs() : 0;
    }
    uint64_t producerFills() const
    {
        return stream ? stream->producerFills() : 0;
    }
    std::atomic<float> preMonitorPeak{0};
    std::atomic<int64_t> callbacks{0}, framesRendered{0}, underruns{0};
    std::atomic<int> callbackFrames{512};

  private:
    std::unique_ptr<PcmStream> stream;
    std::vector<std::unique_ptr<PcmStream>> retired;
    std::unique_ptr<SerialWorker> serviceCleanup;
    std::atomic<PcmStream *> active{nullptr}, hazard{nullptr};
    float gain = 0.7f; // Callback-owned, smoothed monitor gain.
    PcmStream *lastRendered = nullptr;
    std::array<float, 16> lastOut{}, transitionTail{};
    int transitionFrame = 240;
};
} // namespace sp
