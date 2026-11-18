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
PcmStream::PcmStream(const DesignDocument &d,double outputRate,int64_t first,const juce::File &captureFile):rate(outputRate),sourceStart(first),sourceRate(d.sampleRate)
{
    designReader=std::make_unique<DesignReader>(d,outputRate,first,-1,d.renderSettings.tailSeconds);sourceEnd=designReader->endFrame();ratio=sourceRate/rate;outputLength=designReader->length();
    if(captureFile!=juce::File()){captureFile.getParentDirectory().createDirectory();capture=captureFile.createOutputStream();if(!capture)throw std::runtime_error("Cannot capture pre-monitor preview");capture->setPosition(0);capture->truncate();}
}
void PcmStream::fill(int budget)
{
    auto began=monotonicMs();
    auto write = writeIndex.load(std::memory_order_relaxed);
    auto free = capacity - (write - readIndex.load(std::memory_order_acquire));
    auto count = std::min<int64_t>({budget, (int64_t)free, outputLength - outputFrame});
    if(designReader)
    {
        while(count>0){auto n=designReader->read(designBlock,(int)std::min<int64_t>(4096,count));if(n<=0)break;
            for(int i=0;i<n;++i,++write)for(int c=0;c<2;++c){auto v=designBlock.getSample(c,i);pcm[(size_t)c][write%capacity]=v;if(capture)captureData[(size_t)i*2+c]=v;}
            if(capture && !capture->write(captureData.data(),(size_t)n*2*sizeof(float)))throw std::runtime_error("Preview capture write failed");
            outputFrame+=n;count-=n;}
        writeIndex.store(write,std::memory_order_release);if(outputFrame>=outputLength){eof.store(true,std::memory_order_release);if(capture){capture->flush();capture.reset();}}
        maxFillMs=std::max(maxFillMs,monotonicMs()-began);return;
    }
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
    maxFillMs=std::max(maxFillMs,monotonicMs()-began);
}
int PcmStream::read(float *const *output, int channels, int frames, float &gain, float target) noexcept
{
    auto read = readIndex.load(std::memory_order_relaxed);
    const int count =
        (int)std::min<uint64_t>((uint64_t)frames, writeIndex.load(std::memory_order_acquire) - read);
    float rawPeak=0;
    for (int i = 0; i < count; ++i, ++read)
    {
        gain += (target - gain) * 0.002f;
        for (int c = 0; c < channels; ++c)
            if (output[c])
            {auto raw=pcm[(size_t)std::min(c, 1)][read % capacity];rawPeak=std::max(rawPeak,std::abs(raw));output[c][i]=raw*gain;}
    }
    readIndex.store(read, std::memory_order_release);
    preMonitorPeak.store(rawPeak,std::memory_order_relaxed);
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
void AuditionCore::openDesign(const DesignDocument &d,double rate,int64_t first,const juce::File &captureFile)
{
    // Prepare readers/DSP and the initial FIFO while the callback can still use its old graph.
    auto next=std::make_unique<PcmStream>(d,rate,first,captureFile);next->fill(32768);stop();stream=std::move(next);active.store(stream.get());
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
        preMonitorPeak.store(p->preMonitorPeak.load(std::memory_order_relaxed),std::memory_order_relaxed);
    }
    else preMonitorPeak.store(0,std::memory_order_relaxed);
    if(p!=lastRendered){transitionTail=lastOut;transitionFrame=0;lastRendered=p;}
    // A short monitor-only transition uses the previous sample, never obsolete queued PCM.
    for(int i=0;i<frames;++i){if(transitionFrame<240)++transitionFrame;float blend=(float)transitionFrame/240;
        for(int c=0;c<channels;++c)if(out[c]){auto channel=(size_t)std::min(c,1);out[c][i]=out[c][i]*blend+transitionTail[channel]*(1-blend);lastOut[channel]=out[c][i];}}
    hazard.store(nullptr);
    float max = 0;
    for (int c = 0; c < channels; ++c)
        if (out[c])
            for (int i = 0; i < frames; ++i)
                max = std::max(max, std::abs(out[c][i]));
    peak.store(max, std::memory_order_relaxed);
}
} // namespace sp
