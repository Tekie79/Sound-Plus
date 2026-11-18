#include "AudioStream.h"
#include "Performance.h"
#include <cmath>
#include <stdexcept>
namespace sp
{
static_assert(std::atomic<PcmStream *>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
PcmStream::PcmStream(const juce::File &file, double outputRate, int64_t first, int64_t last, int auditionPair) : rate(outputRate), pair(auditionPair)
{
    formats.registerBasicFormats();
    reader.reset(formats.createReaderFor(file));
    if (!reader || reader->sampleRate <= 0 || reader->lengthInSamples <= 0)
        throw std::runtime_error("Audio is offline, empty or unsupported");
    if (rate <= 0)
        throw std::runtime_error("No audio output device");
    sourceRate = reader->sampleRate;
    if (reader->numChannels==0 || reader->numChannels>64 || pair < -1 || pair>=32 || (pair>=0 && pair*2>=(int)reader->numChannels))
        throw std::runtime_error("Audition channel pair is unavailable for this recording");
    cache.setSize((int)reader->numChannels,8192);
    sourceStart = first; sourceEnd = last < 0 ? reader->lengthInSamples : std::min(last,reader->lengthInSamples);
    if (sourceStart < 0 || sourceStart >= sourceEnd) throw std::runtime_error("Invalid source playback range");
    ratio = sourceRate / rate;
    outputLength = (int64_t)std::ceil((double)(sourceEnd-sourceStart) / ratio);
}
void PcmStream::fill(int budget)
{
    auto write = writeIndex.load(std::memory_order_relaxed);
    auto free = capacity - (write - readIndex.load(std::memory_order_acquire));
    auto count = std::min<int64_t>({budget, (int64_t)free, outputLength - outputFrame});
    for (int64_t n = 0; n < count; ++n, ++write, ++outputFrame)
    {
        const double pos = sourceStart + (double)outputFrame * ratio;
        const auto i = (int64_t)pos;
        if (cacheStart < 0 || i < cacheStart || i + 1 >= cacheStart + cache.getNumSamples())
        {
            cacheStart = i;
            cache.clear();
            ++reads;
            if (!reader->read(&cache, 0, cache.getNumSamples(), cacheStart, true, true))
            {
                outputLength = outputFrame;
                eof = true;
                break;
            }
        }
        const int local = (int)(i - cacheStart);
        const auto fraction = (float)(pos - (double)i);
        for (int c = 0; c < 2; ++c)
        {
            auto valueAt=[&](int at) {
                if (pair>=0) return cache.getSample(std::min((int)reader->numChannels-1,pair*2+c),at);
                double sum=0;for (int channel=0;channel<(int)reader->numChannels;++channel) sum+=cache.getSample(channel,at);
                return (float)(sum/reader->numChannels);
            };
            float a = valueAt(local);
            float b = i+1 < sourceEnd ? valueAt(local+1) : a;
            if (!std::isfinite(a) || !std::isfinite(b))
                throw std::runtime_error("Nonfinite audio samples; replace or repair this recording");
            pcm[(size_t)c][write % capacity] = a + (b - a) * fraction;
        }
    }
    writeIndex.store(write, std::memory_order_release);
    if (outputFrame >= outputLength)
        eof.store(true, std::memory_order_release);
}
int PcmStream::read(float *const *output, int channels, int frames, float &gain, float target) noexcept
{
    auto read = readIndex.load(std::memory_order_relaxed);
    const int count =
        (int)std::min<uint64_t>((uint64_t)frames, writeIndex.load(std::memory_order_acquire) - read);
    for (int i = 0; i < count; ++i, ++read)
    {
        gain += (target - gain) * 0.002f;
        for (int c = 0; c < channels; ++c)
            if (output[c])
                output[c][i] = pcm[(size_t)std::min(c, 1)][read % capacity] * gain;
    }
    readIndex.store(read, std::memory_order_release);
    return count;
}
void AuditionCore::open(const juce::File &file, double rate, int64_t start, int64_t end, int auditionPair)
{
    stop(); // Discard the old FIFO at the seek/switch command boundary.
    auto next = std::make_unique<PcmStream>(file, rate,start,end,auditionPair);
    next->fill(32768);
    if (stream)
        retired.push_back(std::move(stream));
    stream = std::move(next);
    active.store(stream.get());
}
void AuditionCore::stop()
{
    active.store(nullptr);
    if (stream)
        retired.push_back(std::move(stream));
}
void AuditionCore::service()
{
    if (stream)
        stream->fill();
    auto *inUse = hazard.load();
    std::erase_if(retired, [&](const auto &p) { return p.get() != inUse; });
}
void AuditionCore::render(float *const *out, int channels, int frames) noexcept
{
    for (int c = 0; c < channels; ++c)
        if (out[c])
            std::fill_n(out[c], frames, 0.0f);
    ++callbacks;
    framesRendered.fetch_add(frames, std::memory_order_relaxed);
    auto *p = active.load();
    hazard.store(p);
    if (p && active.load() == p)
    {
        const int read = p->read(out, channels, frames, gain, volume.load(std::memory_order_relaxed));
        if (read > 0 && p->firstCallbackMs.load(std::memory_order_relaxed) == 0)
            p->firstCallbackMs.store(monotonicMs(), std::memory_order_relaxed);
        if (read < frames && !p->ended())
            underruns.fetch_add(1, std::memory_order_relaxed);
    }
    hazard.store(nullptr);
    float max = 0;
    for (int c = 0; c < channels; ++c)
        if (out[c])
            for (int i = 0; i < frames; ++i)
                max = std::max(max, std::abs(out[c][i]));
    peak.store(max, std::memory_order_relaxed);
}
} // namespace sp
