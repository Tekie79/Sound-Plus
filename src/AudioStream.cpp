#include "AudioStream.h"
#include "Performance.h"
#include <cmath>
#include <stdexcept>
namespace sp
{
static_assert(std::atomic<PcmStream *>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
PcmStream::PcmStream(const juce::File &file, double outputRate, int64_t first, int64_t last, int auditionPair,
                     ChannelLayout interpretation, bool loop, int64_t initialPosition)
    : repeating(loop), rate(outputRate), pair(auditionPair)
{
    pcm.resize(2, std::vector<float>(capacity));
    busLayout = channelLayout("stereo-v1");
    monitor[0][0] = monitor[1][1] = 1;
    formats.registerBasicFormats();
    reader.reset(formats.createReaderFor(file));
    if (!reader || reader->sampleRate <= 0 || reader->lengthInSamples <= 0)
        throw std::runtime_error("Audio is offline, empty or unsupported");
    if (rate <= 0)
        throw std::runtime_error("No audio output device");
    sourceRate = reader->sampleRate;
    if (reader->numChannels == 0 || reader->numChannels > 64 || pair < -2 || pair >= 32 ||
        (pair >= 0 && pair * 2 >= (int)reader->numChannels))
        throw std::runtime_error("Audition channel pair is unavailable for this recording");
    if (pair == -2)
    {
        if (reader->numChannels > 16)
            throw std::runtime_error("Full source monitor supports1–16 channels; select an explicit pair");
        busLayout = interpretation.id.isEmpty() ? detectedLayout(*reader) : interpretation;
        validateLayout(busLayout);
        if (busLayout.size() != (int)reader->numChannels)
            throw std::runtime_error("Source interpretation count mismatch");
        if (busLayout.id.startsWith("unknown-"))
            throw std::runtime_error(
                "Unknown layout: assign a source interpretation or choose an audition pair");
        pcmChannels = (int)reader->numChannels;
        pcm.assign((size_t)pcmChannels, std::vector<float>(capacity));
        monitor = {};
        if (busLayout.id.startsWith("discrete-"))
        {
            monitor[0][0] = 1;
            if (pcmChannels > 1)
                monitor[1][1] = 1;
        }
        else
            monitor = conversionMatrix(busLayout, channelLayout("stereo-v1"));
    }
    cache.setSize((int)reader->numChannels, 8192);
    sourceStart = first;
    sourceEnd = last < 0 ? reader->lengthInSamples : std::min(last, reader->lengthInSamples);
    if (sourceStart < 0 || sourceStart >= sourceEnd)
        throw std::runtime_error("Invalid source playback range");
    ratio = sourceRate / rate;
    outputLength = (int64_t)std::ceil((double)(sourceEnd - sourceStart) / ratio);
    if (repeating && initialPosition >= 0)
    {
        initialOffset = (uint64_t)std::clamp<int64_t>((int64_t)((initialPosition - sourceStart) / ratio), 0,
                                                      outputLength - 1);
        outputFrame = (int64_t)initialOffset;
    }
}
PcmStream::PcmStream(const DesignDocument &d, double outputRate, int64_t first, const juce::File &captureFile,
                     const std::function<bool()> &cancelled, int64_t warmup, int64_t last, bool grain)
    : rate(outputRate), sourceStart(first), sourceRate(d.sampleRate)
{
    designReader = std::make_unique<DesignReader>(
        d, outputRate, first, last, grain ? 0 : d.renderSettings.tailSeconds, cancelled, warmup);
    if (auto *t = selectedTake(d))
        sourceRate = t->sampleRate;
    pcmChannels = designChannels(d);
    busLayout = designLayout(d);
    pcm.resize((size_t)pcmChannels, std::vector<float>(capacity));
    designBlock.setSize(pcmChannels, 4096);
    captureData.resize((size_t)pcmChannels * 4096);
    if (busLayout.id.startsWith("discrete-") || busLayout.id.startsWith("unknown-"))
    {
        monitor[0][0] = 1;
        if (pcmChannels > 1)
            monitor[1][1] = 1;
    }
    else
        monitor = conversionMatrix(busLayout, channelLayout("stereo-v1"));
    previewGrain = grain;
    sourceEnd = designReader->endFrame();
    ratio = sourceRate / rate;
    outputLength = designReader->length();
    if (captureFile != juce::File())
    {
        captureFile.getParentDirectory().createDirectory();
        capture = captureFile.createOutputStream();
        if (!capture)
            throw std::runtime_error("Cannot capture pre-monitor preview");
        capture->setPosition(0);
        capture->truncate();
    }
}
void PcmStream::fill(int budget, const std::function<bool()> &cancelled)
{
    auto began = monotonicMs();
    auto write = writeIndex.load(std::memory_order_relaxed);
    auto free = capacity - (write - readIndex.load(std::memory_order_acquire));
    auto count =
        std::min<int64_t>({budget, (int64_t)free, repeating ? (int64_t)free : outputLength - outputFrame});
    if (designReader)
    {
        while (count > 0)
        {
            auto n = designReader->read(designBlock, (int)std::min<int64_t>(4096, count), cancelled);
            if (n <= 0)
                break;
            for (int i = 0; i < n; ++i, ++write)
                for (int c = 0; c < pcmChannels; ++c)
                {
                    auto v = designBlock.getSample(c, i);
                    if (previewGrain)
                    {
                        auto frame = outputFrame + i;
                        auto ramp = std::max(1.0, rate * .005);
                        v *= std::min({1.0, frame / ramp, (outputLength - frame - 1) / ramp});
                    }
                    pcm[(size_t)c][write % capacity] = v;
                    editTags[write % capacity] = editSequence;
                    editEvents[write % capacity] = editEvent;
                    if (capture)
                        captureData[(size_t)i * pcmChannels + c] = v;
                }
            if (capture && !capture->write(captureData.data(), (size_t)n * pcmChannels * sizeof(float)))
                throw std::runtime_error("Preview capture write failed");
            outputFrame += n;
            count -= n;
        }
        writeIndex.store(write, std::memory_order_release);
        if (outputFrame >= outputLength)
        {
            eof.store(true, std::memory_order_release);
            if (capture)
            {
                capture->flush();
                capture.reset();
            }
        }
        maxFillMs = std::max(maxFillMs, monotonicMs() - began);
        return;
    }
    for (int64_t n = 0; n < count; ++n, ++write, ++outputFrame)
    {
        if (repeating && outputFrame >= outputLength)
            outputFrame = 0;
        const double pos = sourceStart + (double)outputFrame * ratio;
        const auto i = (int64_t)pos;
        if (cacheStart < 0 || i < cacheStart || i + 1 >= cacheStart + cache.getNumSamples())
        {
            cacheStart = i;
            cache.clear();
            ++reads;
            if (!reader->read(&cache, 0, cache.getNumSamples(), cacheStart, true, true))
            {
                if (repeating)
                    throw std::runtime_error("Cannot read the looping recording");
                outputLength = outputFrame;
                eof = true;
                break;
            }
        }
        const int local = (int)(i - cacheStart);
        const auto fraction = (float)(pos - (double)i);
        for (int c = 0; c < pcmChannels; ++c)
        {
            auto valueAt = [&](int at)
            {
                if (pair == -2)
                    return cache.getSample(c, at);
                if (pair >= 0)
                    return cache.getSample(std::min((int)reader->numChannels - 1, pair * 2 + c), at);
                double sum = 0;
                for (int channel = 0; channel < (int)reader->numChannels; ++channel)
                    sum += cache.getSample(channel, at);
                return (float)(sum / reader->numChannels);
            };
            float a = valueAt(local);
            float b = i + 1 < sourceEnd ? valueAt(local + 1) : a;
            if (!std::isfinite(a) || !std::isfinite(b))
                throw std::runtime_error("Nonfinite audio samples; replace or repair this recording");
            pcm[(size_t)c][write % capacity] = a + (b - a) * fraction;
        }
    }
    writeIndex.store(write, std::memory_order_release);
    if (!repeating && outputFrame >= outputLength)
        eof.store(true, std::memory_order_release);
    maxFillMs = std::max(maxFillMs, monotonicMs() - began);
}
int PcmStream::read(float *const *output, int channels, int frames, float &gain, float target,
                    const std::array<std::atomic<int>, 16> *mapping, int solo) noexcept
{
    auto read = readIndex.load(std::memory_order_relaxed);
    const int count =
        (int)std::min<uint64_t>((uint64_t)frames, writeIndex.load(std::memory_order_acquire) - read);
    float rawPeak = 0;
    std::array<float, 16> peaks{};
    for (int i = 0; i < count; ++i, ++read)
    {
        auto tag = editTags[read % capacity];
        if (tag > heardEdit.load(std::memory_order_relaxed))
        {
            editLatencyMs.store(monotonicMs() - editEvents[read % capacity]);
            heardEdit.store(tag);
        }
        gain += (target - gain) * 0.002f;
        for (int k = 0; k < pcmChannels; ++k)
        {
            auto v = std::abs(pcm[(size_t)k][read % capacity]);
            peaks[k] = std::max(peaks[k], v);
            rawPeak = std::max(rawPeak, v);
        }
        for (int c = 0; c < channels; ++c)
            if (output[c])
            {
                float value = 0;
                int assignment = mapping && c < 16 ? (*mapping)[c].load(std::memory_order_relaxed) : -2;
                if (assignment >= 0 && assignment < pcmChannels && (solo < 0 || solo == assignment))
                    value = pcm[(size_t)assignment][read % capacity];
                else if (assignment == -2 && c < 2)
                    for (int k = 0; k < pcmChannels; ++k)
                        if (solo < 0 || solo == k)
                            value += pcm[(size_t)k][read % capacity] * monitor[c][k];
                output[c][i] = value * gain;
            }
    }
    readIndex.store(read, std::memory_order_release);
    preMonitorPeak.store(rawPeak, std::memory_order_relaxed);
    for (int c = 0; c < 16; ++c)
        channelPeaks[c].store(peaks[c], std::memory_order_relaxed);
    return count;
}
void PcmStream::updateParameters(const juce::var &parameters, uint64_t sequence, double event)
{
    if (designReader)
    {
        designReader->updateParameters(parameters);
        outputLength = designReader->length();
        sourceEnd = designReader->endFrame();
        if (outputFrame < outputLength)
            eof.store(false, std::memory_order_release);
        editSequence = sequence;
        editEvent = event;
    }
}
void PcmStream::discardTo(int64_t sourceFrame)
{
    auto target = (uint64_t)std::max<int64_t>(0, (int64_t)std::ceil((sourceFrame - sourceStart) / ratio));
    readIndex.store(std::min(target, writeIndex.load()));
    while (writeIndex.load() < target && !eof.load())
    {
        fill((int)std::min<uint64_t>(4096, target - writeIndex.load()));
        readIndex.store(std::min(target, writeIndex.load()));
    }
    readIndex.store(std::min(target, writeIndex.load()));
}
void AuditionCore::adopt(std::unique_ptr<PcmStream> prepared)
{
    stop();
    stream = std::move(prepared);
    active.store(stream.get());
}
void AuditionCore::open(const juce::File &file, double rate, int64_t start, int64_t end, int auditionPair)
{
    stop(); // Discard the old FIFO at the seek/switch command boundary.
    auto next = std::make_unique<PcmStream>(file, rate, start, end, auditionPair);
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
int64_t AuditionCore::finishTake(bool success)
{
    if (!stream)
        return 0;
    active.store(nullptr);
    // Producer-side quiescence. Callback only publishes/clears its hazard pointer.
    while (hazard.load() == stream.get())
        std::this_thread::yield();
    auto end = stream ? stream->acceptedFrame() : 0;
    if (stream && stream->takeCapture)
        stream->takeCapture->finish(end, success, stream->consumedOutputFrames());
    return end;
}
void AuditionCore::openDesign(const DesignDocument &d, double rate, int64_t first,
                              const juce::File &captureFile)
{
    // Prepare readers/DSP and the initial FIFO while the callback can still use its old graph.
    auto next = std::make_unique<PcmStream>(d, rate, first, captureFile);
    next->fill(32768);
    stop();
    stream = std::move(next);
    active.store(stream.get());
}
void AuditionCore::service(int ahead)
{
    if (stream && stream->available() < ahead)
        stream->fill(ahead - stream->available());
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
        const int read = p->read(out, channels, frames, gain, volume.load(std::memory_order_relaxed),
                                 p->isDesign() ? &monitorMapping : nullptr,
                                 p->isDesign() ? monitorSolo.load(std::memory_order_relaxed) : -1);
        if (read > 0 && p->firstCallbackMs.load(std::memory_order_relaxed) == 0)
            p->firstCallbackMs.store(monotonicMs(), std::memory_order_relaxed);
        if (read < frames && !p->ended())
            underruns.fetch_add(1, std::memory_order_relaxed);
        preMonitorPeak.store(p->preMonitorPeak.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (int c = 0; c < 16; ++c)
            channelPeaks[c].store(p->channelPeaks[c].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    }
    else
        preMonitorPeak.store(0, std::memory_order_relaxed);
    if (p != lastRendered)
    {
        transitionTail = lastOut;
        transitionFrame = 0;
        lastRendered = p;
    }
    // A short monitor-only transition uses the previous sample, never obsolete queued PCM.
    for (int i = 0; i < frames; ++i)
    {
        if (transitionFrame < 240)
            ++transitionFrame;
        float blend = (float)transitionFrame / 240;
        for (int c = 0; c < std::min(channels, 16); ++c)
            if (out[c])
            {
                auto channel = (size_t)std::min(c, 15);
                out[c][i] = out[c][i] * blend + transitionTail[channel] * (1 - blend);
                lastOut[channel] = out[c][i];
            }
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
