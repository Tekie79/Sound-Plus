#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <array>
#include <memory>
#include <vector>
namespace sp
{
// Single producer / single consumer. All decoding and resampling run in fill().
class PcmStream
{
  public:
    PcmStream(const juce::File &, double outputRate, int64_t sourceStart = 0, int64_t sourceEnd = -1, int auditionPair = 0);
    void fill(int budget = 16384);
    int read(float *const *output, int channels, int frames, float &gain, float targetGain) noexcept;
    double seconds() const
    {
        return (double)sourceStart / sourceRate + (double)readIndex.load() / rate;
    }
    int64_t sourceSample() const { return std::min(sourceEnd,sourceStart+(int64_t)(readIndex.load()*ratio)); }
    int64_t startSample() const { return sourceStart; }
    int64_t endSample() const { return sourceEnd; }
    double originalRate() const { return sourceRate; }
    int channelPair() const { return pair; }
    bool ended() const
    {
        return eof.load() && readIndex.load() == writeIndex.load();
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

  private:
    static constexpr uint64_t capacity = 131072;
    std::array<std::array<float, capacity>, 2> pcm{};
    std::atomic<uint64_t> readIndex{0}, writeIndex{0};
    std::atomic<bool> eof{false};
    juce::AudioFormatManager formats;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> cache{2, 8192};
    int64_t cacheStart = -1, outputFrame = 0, outputLength = 0, reads = 0;
    double rate = 48000, ratio = 1;
    int64_t sourceStart = 0, sourceEnd = 0;
    double sourceRate = 48000;
    int pair = 0;
};
class AuditionCore
{
  public:
    void open(const juce::File &, double sampleRate, int64_t startSample = 0, int64_t endSample = -1, int auditionPair = 0);
    void stop();
    void service();
    void render(float *const *, int channels, int frames) noexcept;
    bool playing() const
    {
        return stream && !stream->ended();
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
    int64_t sourceSample() const { return stream ? stream->sourceSample() : 0; }
    int64_t startSample() const { return stream ? stream->startSample() : 0; }
    int64_t endSample() const { return stream ? stream->endSample() : 0; }
    double sourceRate() const { return stream ? stream->originalRate() : 0; }
    int channelPair() const { return stream ? stream->channelPair() : 0; }
    std::atomic<int64_t> callbacks{0}, framesRendered{0}, underruns{0};

  private:
    std::unique_ptr<PcmStream> stream;
    std::vector<std::unique_ptr<PcmStream>> retired;
    std::atomic<PcmStream *> active{nullptr}, hazard{nullptr};
    float gain = 0.7f; // Callback-owned, smoothed monitor gain.
};
} // namespace sp
