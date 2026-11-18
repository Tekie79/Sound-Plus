#include "DesignAudio.h"
#include "DesignLive.h"
#include "PerformanceState.h"
#include <random>
#include <juce_dsp/juce_dsp.h>
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
namespace sp
{
namespace
{
constexpr double pi = juce::MathConstants<double>::pi;
void sourceUnchanged(const Asset &a)
{
    FileStamp s;
    if (!fileStamp(juce::File(a.path), s))
        throw std::runtime_error("Missing source: " + a.name.toStdString());
    if (s.size != a.size || s.modified != a.modified || s.inode != a.inode)
        throw std::runtime_error("Changed source: " + a.name.toStdString());
}
// This insert boundary deliberately separates saved parameters from processing state.
class EffectProcessor
{
  public:
    virtual ~EffectProcessor() = default;
    virtual void process(juce::AudioBuffer<float> &, int) = 0;
    virtual void update(const StockEffect &) = 0;
};
class StockProcessor final : public EffectProcessor
{
    StockEffect effect;
    juce::Reverb reverb;
    juce::dsp::IIR::Filter<float> high[2], low[2];
    juce::AudioBuffer<float> delay;
    int write = 0;
    double sampleRate;
    juce::SmoothedValue<float> gain, feedback, wet, delayFrames;
    juce::SmoothedValue<double> highFrequency, lowFrequency;

  public:
    StockProcessor(const StockEffect &e, double rate) : effect(e), sampleRate(rate)
    {
        for (auto *value : {&gain, &feedback, &wet, &delayFrames})
            value->reset(rate, .01);
        highFrequency.reset(rate, .02);
        lowFrequency.reset(rate, .02);
        highFrequency.setCurrentAndTargetValue(e.parameters[0]);
        lowFrequency.setCurrentAndTargetValue(e.parameters[1]);
        if (e.type == "filter")
            for (int c = 0; c < 2; ++c)
            {
                high[c].coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(
                    rate, std::clamp(e.parameters[0], 10.0, rate * .45));
                low[c].coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass(
                    rate, std::clamp(e.parameters[1], 20.0, rate * .45));
                high[c].reset();
                low[c].reset();
            }
        if (e.type == "delay")
        {
            // Fixed capacity preserves the delay history when its tap moves.
            delay.setSize(2, (int)std::ceil(rate * 2) + 2);
            delay.clear();
        }
        if (e.type == "reverb")
        {
            reverb.setSampleRate(rate);
            juce::Reverb::Parameters p;
            p.roomSize = (float)e.parameters[0];
            p.damping = (float)e.parameters[1];
            p.wetLevel = (float)e.parameters[2];
            p.dryLevel = 1 - p.wetLevel;
            p.width = (float)e.parameters[3];
            p.freezeMode = 0;
            reverb.setParameters(p);
            reverb.reset();
        }
        update(e);
        for (auto *value : {&gain, &feedback, &wet, &delayFrames})
            value->setCurrentAndTargetValue(value->getTargetValue());
    }
    void update(const StockEffect &e) override
    {
        effect = e;
        gain.setTargetValue(e.type == "gain" ? juce::Decibels::decibelsToGain((float)e.parameters[0]) : 1.f);
        feedback.setTargetValue((float)e.parameters[1]);
        wet.setTargetValue((float)e.parameters[2]);
        delayFrames.setTargetValue((float)std::llround(sampleRate * e.parameters[0] / 1000));
        if (e.type == "filter")
        {
            highFrequency.setTargetValue(std::clamp(e.parameters[0], 10., sampleRate * .45));
            lowFrequency.setTargetValue(std::clamp(e.parameters[1], 20., sampleRate * .45));
        }
        if (e.type == "reverb")
        {
            juce::Reverb::Parameters p;
            p.roomSize = (float)e.parameters[0];
            p.damping = (float)e.parameters[1];
            p.wetLevel = (float)e.parameters[2];
            p.dryLevel = 1 - p.wetLevel;
            p.width = (float)e.parameters[3];
            reverb.setParameters(p);
        }
    }
    void process(juce::AudioBuffer<float> &b, int count) override
    {
        if (effect.bypass)
            return;
        if (effect.type == "gain")
        {
            for (int n = 0; n < count; ++n)
            {
                auto value = gain.getNextValue();
                for (int c = 0; c < 2; ++c)
                    b.setSample(c, n, b.getSample(c, n) * value);
            }
            return;
        }
        if (effect.type == "filter")
        {
            // Producer-worker DSP only: bounded sub-block coefficient updates smooth
            // continuous cutoff motion while preserving filter history and graph identity.
            for (int offset = 0; offset < count; offset += 32)
            {
                int frames = std::min(32, count - offset);
                if (highFrequency.isSmoothing() || lowFrequency.isSmoothing())
                {
                    auto hp = highFrequency.skip(frames), lp = lowFrequency.skip(frames);
                    auto hc = juce::dsp::IIR::Coefficients<float>::makeHighPass(
                        sampleRate, std::clamp(hp, 10., sampleRate * .45));
                    auto lc = juce::dsp::IIR::Coefficients<float>::makeLowPass(
                        sampleRate, std::clamp(lp, 20., sampleRate * .45));
                    for (int c = 0; c < 2; ++c)
                    {
                        *high[c].coefficients = *hc;
                        *low[c].coefficients = *lc;
                    }
                }
                for (int c = 0; c < 2; ++c)
                    for (int n = offset; n < offset + frames; ++n)
                        b.setSample(c, n, low[c].processSample(high[c].processSample(b.getSample(c, n))));
            }
            return;
        }
        if (effect.type == "reverb")
        {
            reverb.processStereo(b.getWritePointer(0), b.getWritePointer(1), count);
            return;
        }
        for (int n = 0; n < count; ++n)
        {
            float tap = write - delayFrames.getNextValue();
            if (tap < 0)
                tap += delay.getNumSamples();
            int first = (int)tap, second = (first + 1) % delay.getNumSamples();
            float fraction = tap - first, mix = wet.getNextValue(), repeat = feedback.getNextValue();
            for (int c = 0; c < 2; ++c)
            {
                float dry = b.getSample(c, n);
                float echo = juce::jmap(fraction, delay.getSample(c, first), delay.getSample(c, second));
                delay.setSample(c, write, dry + echo * repeat);
                b.setSample(c, n, dry * (1 - mix) + echo * mix);
            }
            write = (write + 1) % delay.getNumSamples();
        }
    }
};
using Rack = std::vector<std::unique_ptr<EffectProcessor>>;
Rack prepareRack(const std::vector<StockEffect> &effects, double rate)
{
    Rack rack;
    for (auto &e : effects)
        rack.push_back(std::make_unique<StockProcessor>(e, rate));
    return rack;
}
void processRack(Rack &rack, juce::AudioBuffer<float> &buffer, int n)
{
    for (auto &effect : rack)
        effect->process(buffer, n);
}
void updateRack(Rack &rack, const std::vector<StockEffect> &effects)
{
    for (size_t i = 0; i < rack.size(); ++i)
        rack[i]->update(effects[i]);
}
class Voice
{
    DesignLayer layer;
    double ratio;
    int64_t duration;
    juce::AudioFormatManager formats;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> scratch;
    struct Cache
    {
        int64_t start = -1;
        uint64_t age = 0;
        juce::AudioBuffer<float> data{2, 8192};
    };
    Cache cache[2];
    uint64_t clock = 0;
    std::array<std::array<float, 32>, 1024> coefficients{};
    Rack rack;
    juce::AudioBuffer<float> block{2, 512};
    bool mono = false, identity = false;
    double designRate;
    juce::SmoothedValue<float> leftGain, rightGain;
    juce::SmoothedValue<double> inFrames, outFrames;
    juce::SmoothedValue<double> inShapeMix, outShapeMix;
    PreparedFade previousIn, previousOut, currentIn, currentOut;
    void gainTargets(bool initial)
    {
        float gain = juce::Decibels::decibelsToGain((float)layer.gainDb) * (float)layer.controlGain;
        float left = mono            ? (float)std::cos((layer.pan + 1) * pi / 4)
                     : layer.pan > 0 ? (float)std::cos(layer.pan * pi / 2)
                                     : 1;
        float right = mono            ? (float)std::sin((layer.pan + 1) * pi / 4)
                      : layer.pan < 0 ? (float)std::cos(layer.pan * pi / 2)
                                      : 1;
        if (initial)
        {
            leftGain.setCurrentAndTargetValue(gain * left);
            rightGain.setCurrentAndTargetValue(gain * right);
            inFrames.setCurrentAndTargetValue(layer.fadeIn);
            outFrames.setCurrentAndTargetValue(layer.fadeOut);
        }
        else
        {
            leftGain.setTargetValue(gain * left);
            rightGain.setTargetValue(gain * right);
            inFrames.setTargetValue(layer.fadeIn);
            outFrames.setTargetValue(layer.fadeOut);
        }
    }
    void buildCoefficients()
    {
        identity = std::abs(ratio - 1) < 1e-12;
        auto cutoff = std::min(1.0, 1.0 / ratio) * .94;
        for (int p = 0; p < 1024; ++p)
        {
            double total = 0;
            for (int n = 0; n < 32; ++n)
            {
                double x = n - 15 - p / 1024.0;
                double sinc = std::abs(x) < 1e-12 ? cutoff : std::sin(pi * x * cutoff) / (pi * x);
                double window = .5 + .5 * std::cos(pi * x / 16);
                double value = std::abs(x) < 16 ? sinc * window : 0;
                coefficients[(size_t)p][(size_t)n] = (float)value;
                total += value;
            }
            for (auto &v : coefficients[(size_t)p])
                v = (float)(v / total);
        }
    }
    float sample(int channel, int64_t frame)
    {
        frame = std::clamp(frame, layer.region.startSample, layer.region.endSample - 1);
        Cache *entry = nullptr;
        for (auto &c : cache)
            if (c.start >= 0 && frame >= c.start && frame < c.start + 8192)
            {
                entry = &c;
                break;
            }
        if (!entry)
        {
            entry = cache[0].age <= cache[1].age ? &cache[0] : &cache[1];
            entry->start = std::max<int64_t>(0, (frame / 4096) * 4096 - 32);
            scratch.clear();
            if (!reader->read(&scratch, 0, 8192, entry->start, true, true))
                throw std::runtime_error("Cannot read source: " + layer.source.name.toStdString());
            for (int n = 0; n < 8192; ++n)
                for (int c = 0; c < 2; ++c)
                {
                    float value = 0;
                    if (layer.region.auditionPair < 0)
                    {
                        double sum = 0;
                        for (int k = 0; k < scratch.getNumChannels(); ++k)
                            sum += scratch.getSample(k, n);
                        value = (float)(sum / scratch.getNumChannels());
                    }
                    else
                        value = scratch.getSample(
                            std::min(scratch.getNumChannels() - 1, layer.region.auditionPair * 2 + c), n);
                    if (!std::isfinite(value))
                        throw std::runtime_error("Nonfinite design source");
                    entry->data.setSample(c, n, value);
                }
        }
        entry->age = ++clock;
        return entry->data.getSample(channel, (int)(frame - entry->start));
    }
    float interpolated(int channel, double position)
    {
        auto frame = (int64_t)std::floor(position);
        if (identity)
            return sample(channel, frame);
        auto phase = std::clamp((int)((position - frame) * 1024), 0, 1023);
        double value = 0;
        for (int n = 0; n < 32; ++n)
            value += sample(channel, frame + n - 15) * coefficients[(size_t)phase][(size_t)n];
        return (float)value;
    }

  public:
    Voice(const DesignLayer &l, double rate)
        : layer(l), ratio(l.source.sampleRate * l.rate / rate), duration(layerDuration(l, rate)),
          rack(prepareRack(l.effects, rate)), designRate(rate), previousIn(l.fadeInShape, l.fadeInPoints),
          previousOut(l.fadeOutShape, l.fadeOutPoints), currentIn(previousIn), currentOut(previousOut)
    {
        if (l.source.error.isNotEmpty())
            throw std::runtime_error((l.name + ": " + l.source.error).toStdString());
        sourceUnchanged(l.source);
        formats.registerBasicFormats();
        reader.reset(formats.createReaderFor(juce::File(l.source.path)));
        if (!reader || reader->sampleRate != l.source.sampleRate ||
            reader->lengthInSamples != l.source.samples || (int)reader->numChannels != l.source.channels)
            throw std::runtime_error("Source header changed: " + l.source.name.toStdString());
        scratch.setSize(l.source.channels, 8192);
        mono = l.region.auditionPair < 0 || l.region.auditionPair * 2 + 1 >= l.source.channels;
        identity = std::abs(ratio - 1) < 1e-12;
        leftGain.reset(rate, .005);
        rightGain.reset(rate, .005);
        inFrames.reset(rate, .005);
        outFrames.reset(rate, .005);
        inShapeMix.reset(rate, .005);
        outShapeMix.reset(rate, .005);
        inShapeMix.setCurrentAndTargetValue(1);
        outShapeMix.setCurrentAndTargetValue(1);
        gainTargets(true);
        buildCoefficients();
    }
    void add(juce::AudioBuffer<float> &output, int offset, int frames, int64_t frame)
    {
        block.clear();
        for (int n = 0; n < frames; ++n)
        {
            auto local = frame + n - layer.placement;
            auto left = leftGain.getNextValue(), right = rightGain.getNextValue();
            auto fadeIn = inFrames.getNextValue(), fadeOut = outFrames.getNextValue();
            auto inBlend = inShapeMix.getNextValue(), outBlend = outShapeMix.getNextValue();
            if (local < 0 || local >= duration)
                continue;
            double position = layer.reverse ? layer.region.endSample - 1 - local * ratio
                                            : layer.region.startSample + local * ratio;
            position =
                std::clamp(position, (double)layer.region.startSample, (double)(layer.region.endSample - 1));
            float envelope = 1;
            if (fadeIn > 0)
            {
                auto progress = (double)local / fadeIn;
                envelope *= juce::jmap(inBlend, previousIn.value(progress), currentIn.value(progress));
            }
            if (fadeOut > 0)
            {
                auto progress = (double)(duration - local - 1) / fadeOut;
                envelope *= juce::jmap(outBlend, previousOut.value(progress), currentOut.value(progress));
            }
            block.setSample(0, n, interpolated(0, position) * left * envelope);
            block.setSample(1, n, interpolated(1, position) * right * envelope);
        }
        processRack(rack, block, frames);
        for (int c = 0; c < 2; ++c)
            output.addFrom(c, offset, block, c, 0, frames);
    }
    const juce::String &id() const
    {
        return layer.id;
    }
    void update(const DesignLayer &updated)
    {
        bool rateChanged = layer.rate != updated.rate;
        auto changeShape = [](FadeShape before, FadeShape after, const auto &oldPoints, const auto &newPoints,
                              PreparedFade &previous, PreparedFade &current, auto &mix)
        {
            if (before != after || oldPoints != newPoints)
            {
                previous = current;
                current.reset(after, newPoints);
                mix.setCurrentAndTargetValue(0);
                mix.setTargetValue(1);
            }
        };
        changeShape(layer.fadeInShape, updated.fadeInShape, layer.fadeInPoints, updated.fadeInPoints,
                    previousIn, currentIn, inShapeMix);
        changeShape(layer.fadeOutShape, updated.fadeOutShape, layer.fadeOutPoints, updated.fadeOutPoints,
                    previousOut, currentOut, outShapeMix);
        layer = updated;
        duration = layerDuration(layer, designRate);
        ratio = layer.source.sampleRate * layer.rate / designRate;
        if (rateChanged)
            buildCoefficients();
        gainTargets(false);
        updateRack(rack, layer.effects);
    }
    void verify()
    {
        sourceUnchanged(layer.source);
    }
};
void syncFile(const juce::File &file)
{
    int fd = ::open(file.getFullPathName().toRawUTF8(), O_RDONLY);
    if (fd < 0 || ::fsync(fd) != 0)
    {
        if (fd >= 0)
            ::close(fd);
        throw std::runtime_error("Cannot finalize output on this storage");
    }
    ::close(fd);
}
} // namespace
struct DesignGraph::Impl
{
    double sampleRate;
    int64_t cursor = 0, lastVerified = -1;
    double master;
    DesignDocument document;
    juce::SmoothedValue<float> masterGain;
    std::vector<std::unique_ptr<Voice>> voices;
    Rack masterRack;
    juce::AudioBuffer<float> block{2, 512};
    explicit Impl(const DesignDocument &d, const std::function<bool()> &cancelled)
        : sampleRate(d.sampleRate), master(d.activeMix().masterDb), document(d)
    {
        validateDesign(d);
        masterGain.reset(d.sampleRate, .005);
        masterGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain((float)master));
        masterRack = prepareRack(d.activeMix().masterEffects, d.sampleRate);
        bool solo = std::any_of(d.activeMix().layers.begin(), d.activeMix().layers.end(),
                                [](auto &l) { return l.solo; });
        for (auto &l : d.activeMix().layers)
            if (!l.mute && (!solo || l.solo))
            {
                if (cancelled())
                    throw QueryCancelled();
                voices.push_back(std::make_unique<Voice>(l, d.sampleRate));
            }
    }
    void render(juce::AudioBuffer<float> &out, int offset, int count)
    {
        for (int done = 0; done < count;)
        {
            int n = std::min(512, count - done);
            block.clear();
            if (lastVerified < 0 || cursor - lastVerified >= sampleRate)
            {
                for (auto &voice : voices)
                    voice->verify();
                lastVerified = cursor;
            }
            for (auto &voice : voices)
                voice->add(block, 0, n, cursor);
            processRack(masterRack, block, n);
            for (int i = 0; i < n; ++i)
            {
                auto gain = masterGain.getNextValue();
                for (int c = 0; c < 2; ++c)
                    block.setSample(c, i, block.getSample(c, i) * gain);
            }
            for (int c = 0; c < 2; ++c)
            {
                for (int i = 0; i < n; ++i)
                    if (!std::isfinite(block.getSample(c, i)))
                        throw std::runtime_error("Stock graph produced nonfinite audio");
                out.copyFrom(c, offset + done, block, c, 0, n);
            }
            cursor += n;
            done += n;
        }
    }
};
DesignGraph::DesignGraph(const DesignDocument &d, const std::function<bool()> &cancelled)
    : impl(std::make_unique<Impl>(resolvePerformance(d), cancelled))
{
}
void DesignGraph::updateParameters(const juce::var &parameters)
{
    applyDesignParameters(impl->document, parameters);
    for (auto &layer : impl->document.activeMix().layers)
        for (auto &voice : impl->voices)
            if (voice->id() == layer.id)
                voice->update(layer);
    impl->masterGain.setTargetValue(
        juce::Decibels::decibelsToGain((float)impl->document.activeMix().masterDb));
    updateRack(impl->masterRack, impl->document.activeMix().masterEffects);
}
DesignGraph::~DesignGraph() = default;
void DesignGraph::render(juce::AudioBuffer<float> &out, int offset, int count)
{
    impl->render(out, offset, count);
}
void DesignGraph::seek(int64_t frame, const std::function<bool()> &cancelled, int64_t warmup)
{
    if (warmup >= 0 && impl->cursor == 0)
        impl->cursor = std::max<int64_t>(0, frame - warmup);
    if (frame < impl->cursor)
        throw std::runtime_error("A seek requires fresh processing state");
    juce::AudioBuffer<float> discard(2, 512);
    while (impl->cursor < frame)
    {
        if (cancelled())
            throw QueryCancelled();
        impl->render(discard, 0, (int)std::min<int64_t>(512, frame - impl->cursor));
    }
}
int DesignGraph::layers() const
{
    return (int)impl->voices.size();
}
int64_t DesignGraph::position() const
{
    return impl->cursor;
}
double DesignGraph::rate() const
{
    return impl->sampleRate;
}
struct DesignReader::Impl : juce::AudioSource
{
    DesignGraph graph;
    DesignDocument document;
    int64_t firstFrame, lastOverride;
    double tailSeconds;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    int64_t count = 0, total = 0, end;
    double outputRate;
    Impl(const DesignDocument &d, double rate, int64_t first, int64_t last, double tail,
         const std::function<bool()> &cancelled, int64_t warmup)
        : graph(d, cancelled), document(d), firstFrame(first), lastOverride(last), tailSeconds(tail),
          end((last < 0 ? mixDuration(d.activeMix(), d.sampleRate) : last) +
              (int64_t)std::llround(tail * d.sampleRate)),
          outputRate(rate)
    {
        if (first < 0 || end <= first || end > d.sampleRate * 3630 || rate < 8000 || rate > 192000)
            throw std::runtime_error("Invalid design playback/render range");
        graph.seek(first, cancelled, warmup);
        total = (int64_t)std::ceil((long double)(end - first) * rate / d.sampleRate);
        if (rate != d.sampleRate)
        {
            resampler = std::make_unique<juce::ResamplingAudioSource>(this, false, 2);
            resampler->setResamplingRatio(d.sampleRate / rate);
            resampler->prepareToPlay(4096, rate);
        }
    }
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo &b) override
    {
        graph.render(*b.buffer, b.startSample, b.numSamples);
    }
};
DesignReader::DesignReader(const DesignDocument &d, double outputRate, int64_t first, int64_t last,
                           double tail, const std::function<bool()> &cancelled, int64_t warmup)
    : impl(std::make_unique<Impl>(d, outputRate, first, last, tail, cancelled, warmup))
{
}
void DesignReader::updateParameters(const juce::var &parameters)
{
    applyDesignParameters(impl->document, parameters);
    impl->graph.updateParameters(parameters);
    impl->end = (impl->lastOverride < 0 ? mixDuration(impl->document.activeMix(), impl->document.sampleRate)
                                        : impl->lastOverride) +
                (int64_t)std::llround(impl->tailSeconds * impl->document.sampleRate);
    impl->total = std::max<int64_t>(0, (int64_t)std::ceil((long double)(impl->end - impl->firstFrame) *
                                                          impl->outputRate / impl->document.sampleRate));
}
DesignReader::~DesignReader() = default;
int DesignReader::read(juce::AudioBuffer<float> &buffer, int frames, const std::function<bool()> &cancelled)
{
    if (cancelled())
        throw QueryCancelled();
    int n = (int)std::min<int64_t>(frames, impl->total - impl->count);
    buffer.clear();
    if (n > 0)
    {
        juce::AudioSourceChannelInfo info(&buffer, 0, n);
        if (impl->resampler)
            impl->resampler->getNextAudioBlock(info);
        else
            impl->getNextAudioBlock(info);
        impl->count += n;
    }
    return n;
}
int64_t DesignReader::length() const
{
    return impl->total;
}
int64_t DesignReader::consumed() const
{
    return impl->count;
}
int64_t DesignReader::endFrame() const
{
    return impl->end;
}
int DesignReader::layers() const
{
    return impl->graph.layers();
}
DesignOutput renderDesign(const DesignDocument &d, const juce::File &folder,
                          const DesignRenderSettings &settings, const std::function<bool()> &cancelled,
                          const std::function<void(double)> &progress)
{
    auto checked = d;
    checked.renderSettings = settings;
    validateDesign(checked);
    if (!folder.createDirectory())
        throw std::runtime_error("Cannot create output folder");
    auto name = juce::File::createLegalFileName(d.name) + (d.activeVariation.isEmpty() ? "" : "-variation") +
                "-r" + juce::String(d.revision) + "-" + juce::Uuid().toString().substring(0, 8) + ".wav";
    auto target = folder.getNonexistentChildFile(name, {}, false);
    juce::TemporaryFile staging(target, juce::TemporaryFile::useHiddenFile);
    juce::AudioBuffer<float> block(2, 4096);
    double normalize = 1;
    if (settings.normalize)
    {
        DesignReader meter(d, settings.sampleRate, settings.first, settings.last, settings.tailSeconds,
                           cancelled);
        double peak = 0;
        while (auto n = meter.read(block, 4096, cancelled))
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    peak = std::max(peak, std::abs(settings.channels == 1
                                                       ? (block.getSample(0, i) + block.getSample(1, i)) * .5
                                                       : block.getSample(c, i)));
            progress(.4 * (double)meter.consumed() / meter.length());
        }
        if (peak > 0)
            normalize = std::pow(10.0, -1.0 / 20) / peak;
    }
    DesignReader reader(d, settings.sampleRate, settings.first, settings.last, settings.tailSeconds,
                        cancelled);
    juce::WavAudioFormat format;
    auto stream = staging.getFile().createOutputStream();
    if (!stream)
        throw std::runtime_error("Cannot stage output");
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(
        stream.release(), settings.sampleRate, (unsigned)settings.channels, settings.bits, {}, 0));
    if (!writer)
        throw std::runtime_error("Cannot create WAV writer");
    DesignOutput result;
    result.path = target.getFullPathName();
    result.samples = reader.length();
    result.revision = d.revision;
    result.variation = d.activeVariation;
    result.settings = settings;
    result.created = juce::Time::getCurrentTime().toISO8601(true);
    std::mt19937 rng(170526);
    double lsb = std::ldexp(1.0, 1 - settings.bits);
    while (auto n = reader.read(block, 4096, cancelled))
    {
        if (settings.channels == 1)
            for (int i = 0; i < n; ++i)
                block.setSample(0, i, (block.getSample(0, i) + block.getSample(1, i)) * .5f);
        for (int c = 0; c < settings.channels; ++c)
            for (int i = 0; i < n; ++i)
            {
                double value = block.getSample(c, i) * normalize;
                result.peak = std::max(result.peak, std::abs(value));
                if (std::abs(value) > 1)
                    ++result.clipped;
                if (settings.dither && settings.bits < 32)
                    value += ((double)rng() / rng.max() - (double)rng() / rng.max()) * lsb;
                block.setSample(c, i, (float)value);
            }
        if (!writer->writeFromAudioSampleBuffer(block, 0, n))
            throw std::runtime_error("Output write failed; no file was published");
        progress((settings.normalize ? .4 : 0) +
                 (settings.normalize ? .6 : 1) * (double)reader.consumed() / reader.length());
    }
    writer.reset();
    bool solo =
        std::any_of(d.activeMix().layers.begin(), d.activeMix().layers.end(), [](auto &l) { return l.solo; });
    for (auto &l : d.activeMix().layers)
        if (!l.mute && (!solo || l.solo))
            sourceUnchanged(l.source);
    syncFile(staging.getFile());
    if (cancelled())
        throw QueryCancelled();
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> verify(fm.createReaderFor(staging.getFile()));
    if (!verify || verify->lengthInSamples != result.samples || verify->sampleRate != settings.sampleRate ||
        (int)verify->numChannels != settings.channels)
        throw std::runtime_error("Finalized WAV validation failed");
    verify.reset();
    auto bytes = staging.getFile().createInputStream();
    if (!bytes)
        throw std::runtime_error("Cannot verify rendered output");
    result.sha256 = juce::SHA256(*bytes).toHexString();
    bytes.reset();
    if (cancelled())
        throw QueryCancelled();
    if (!staging.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot publish durable WAV");
    progress(1);
    return result;
}
DesignOutput copyDesignOriginal(const DesignLayer &l, const juce::File &folder, int64_t revision,
                                const std::function<bool()> &cancelled)
{
    if (l.source.error.isNotEmpty())
        throw std::runtime_error(l.source.error.toStdString());
    sourceUnchanged(l.source);
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> header(fm.createReaderFor(juce::File(l.source.path)));
    if (!header)
        throw std::runtime_error("Cannot read original source header");
    int sourceBits = (int)header->bitsPerSample;
    header.reset();
    if (!folder.createDirectory())
        throw std::runtime_error("Cannot create project media folder");
    juce::File source(l.source.path);
    auto target = folder.getNonexistentChildFile(source.getFileNameWithoutExtension() + "-" +
                                                     juce::Uuid().toString().substring(0, 8),
                                                 source.getFileExtension());
    juce::TemporaryFile staging(target, juce::TemporaryFile::useHiddenFile);
    auto in = source.createInputStream();
    auto out = staging.getFile().createOutputStream();
    if (!in || !out)
        throw std::runtime_error("Cannot copy original");
    std::array<char, 65536> data;
    while (!in->isExhausted())
    {
        if (cancelled())
            throw QueryCancelled();
        int n = in->read(data.data(), (int)data.size());
        if (n <= 0 || !out->write(data.data(), (size_t)n))
            throw std::runtime_error("Original copy interrupted");
    }
    out->flush();
    out.reset();
    in.reset();
    sourceUnchanged(l.source);
    auto first = source.createInputStream(), second = staging.getFile().createInputStream();
    if (!first || !second)
        throw std::runtime_error("Cannot verify original copy");
    auto digest = juce::SHA256(*first).toHexString();
    if (digest != juce::SHA256(*second).toHexString())
        throw std::runtime_error("Original copy checksum mismatch");
    first.reset();
    second.reset();
    sourceUnchanged(l.source);
    syncFile(staging.getFile());
    if (cancelled())
        throw QueryCancelled();
    if (!staging.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot publish original copy");
    DesignOutput result;
    result.path = target.getFullPathName();
    result.sha256 = digest;
    result.samples = l.source.samples;
    result.revision = revision;
    result.kind = "original-copy";
    result.settings.sampleRate = l.source.sampleRate;
    result.settings.channels = l.source.channels;
    result.settings.bits = sourceBits;
    result.settings.tailSeconds = 0;
    result.settings.last = l.source.samples;
    result.created = juce::Time::getCurrentTime().toISO8601(true);
    return result;
}
} // namespace sp
