#include "DesignAudio.h"
#include "SourcePools.h"
#include "ProcessorBusContract.h"
#include "DesignLive.h"
#include "PluginProcessing.h"
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
    juce::dsp::IIR::Filter<float> high[16], low[16];
    std::vector<std::unique_ptr<juce::Reverb>> monoReverbs;
    int channels = 2;
    bool layoutAware = false;
    juce::AudioBuffer<float> delay;
    int write = 0;
    double sampleRate;
    juce::SmoothedValue<float> gain, feedback, wet, delayFrames;
    juce::SmoothedValue<double> highFrequency, lowFrequency;
    bool partitionInvariant = false;
    int coefficientRemaining = 0;

  public:
    StockProcessor(const StockEffect &e, double rate, bool invariant = false, int width = 2,
                   bool aware = false)
        : effect(e), channels(width), layoutAware(aware), sampleRate(rate), partitionInvariant(invariant)
    {
        for (auto *value : {&gain, &feedback, &wet, &delayFrames})
            value->reset(rate, .01);
        highFrequency.reset(rate, .02);
        lowFrequency.reset(rate, .02);
        highFrequency.setCurrentAndTargetValue(e.parameters[0]);
        lowFrequency.setCurrentAndTargetValue(e.parameters[1]);
        if (e.type == "filter")
            for (int c = 0; c < channels; ++c)
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
            delay.setSize(channels, (int)std::ceil(rate * 2) + 2);
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
            if (layoutAware)
                for (int c = 0; c < channels; ++c)
                {
                    auto r = std::make_unique<juce::Reverb>();
                    r->setSampleRate(rate);
                    r->setParameters(p);
                    r->reset();
                    monoReverbs.push_back(std::move(r));
                }
        }
        update(e);
        for (auto *value : {&gain, &feedback, &wet, &delayFrames})
            value->setCurrentAndTargetValue(value->getTargetValue());
    }
    void update(const StockEffect &e) override
    {
        if (effect.parameters[0] != e.parameters[0] || effect.parameters[1] != e.parameters[1])
            coefficientRemaining = 0;
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
            for (auto &r : monoReverbs)
                r->setParameters(p);
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
                for (int c = 0; c < channels; ++c)
                    b.setSample(c, n, b.getSample(c, n) * value);
            }
            return;
        }
        if (effect.type == "filter")
        {
            if (partitionInvariant)
            {
                for (int n = 0; n < count; ++n)
                {
                    if (coefficientRemaining == 0)
                    {
                        coefficientRemaining = 32;
                        auto hp = highFrequency.skip(32), lp = lowFrequency.skip(32);
                        auto hc = juce::dsp::IIR::Coefficients<float>::makeHighPass(
                            sampleRate, std::clamp(hp, 10., sampleRate * .45));
                        auto lc = juce::dsp::IIR::Coefficients<float>::makeLowPass(
                            sampleRate, std::clamp(lp, 20., sampleRate * .45));
                        for (int c = 0; c < channels; ++c)
                        {
                            *high[c].coefficients = *hc;
                            *low[c].coefficients = *lc;
                        }
                    }
                    for (int c = 0; c < channels; ++c)
                        b.setSample(c, n, low[c].processSample(high[c].processSample(b.getSample(c, n))));
                    --coefficientRemaining;
                }
                return;
            }
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
                    for (int c = 0; c < channels; ++c)
                    {
                        *high[c].coefficients = *hc;
                        *low[c].coefficients = *lc;
                    }
                }
                for (int c = 0; c < channels; ++c)
                    for (int n = offset; n < offset + frames; ++n)
                        b.setSample(c, n, low[c].processSample(high[c].processSample(b.getSample(c, n))));
            }
            return;
        }
        if (effect.type == "reverb")
        {
            if (layoutAware)
                for (int c = 0; c < channels; ++c)
                    monoReverbs[(size_t)c]->processMono(b.getWritePointer(c), count);
            else
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
            for (int c = 0; c < channels; ++c)
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
Rack prepareRack(const std::vector<StockEffect> &effects, double rate, bool invariant = false,
                 int channels = 2, bool aware = false)
{
    negotiateStockBus({discreteLayout(channels), discreteLayout(channels), false});
    Rack rack;
    for (auto &e : effects)
        rack.push_back(std::make_unique<StockProcessor>(e, rate, invariant, channels, aware));
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
    std::unique_ptr<HostedRackClient> hosted;
    juce::AudioBuffer<float> alignment;
    int alignmentPosition = 0, alignmentFrames = 0;
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
    bool mono = false, identity = false, layoutAware = false;
    int inputChannels = 2;
    int routingVersion = 2;
    ChannelLayout bus;
    ChannelMatrix matrix{};
    std::array<std::array<juce::SmoothedValue<float>, 16>, 16> matrixGains;
    juce::SmoothedValue<double> azimuth, width;
    juce::dsp::IIR::Filter<float> lfeFilter;
    int lfeIndex = -1;
    double designRate;
    juce::SmoothedValue<float> leftGain, rightGain;
    juce::SmoothedValue<double> inFrames, outFrames;
    juce::SmoothedValue<double> inShapeMix, outShapeMix;
    PreparedFade previousIn, previousOut, currentIn, currentOut;
    void gainTargets(bool initial)
    {
        float gain = juce::Decibels::decibelsToGain((float)layer.gainDb) * (float)layer.controlGain;
        if (layoutAware)
        {
            if (initial)
                leftGain.setCurrentAndTargetValue(gain);
            else
                leftGain.setTargetValue(gain);
        }
        float left = mono            ? (float)std::cos((layer.pan + 1) * pi / 4)
                     : layer.pan > 0 ? (float)std::cos(layer.pan * pi / 2)
                                     : 1;
        float right = mono            ? (float)std::sin((layer.pan + 1) * pi / 4)
                      : layer.pan < 0 ? (float)std::cos(layer.pan * pi / 2)
                                      : 1;
        if (initial)
        {
            if (!layoutAware)
                leftGain.setCurrentAndTargetValue(gain * left);
            rightGain.setCurrentAndTargetValue(gain * right);
            inFrames.setCurrentAndTargetValue(layer.fadeIn);
            outFrames.setCurrentAndTargetValue(layer.fadeOut);
        }
        else
        {
            if (!layoutAware)
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
                for (int c = 0; c < inputChannels; ++c)
                {
                    float value = 0;
                    if (layoutAware && layer.routing.legacyAnalysisMean)
                    {
                        double sum = 0;
                        for (int physical : layer.routing.inputs)
                            sum += scratch.getSample(physical, n);
                        value = (float)(sum / layer.routing.inputs.size());
                    }
                    else if (layoutAware)
                        value = scratch.getSample(layer.routing.inputs[(size_t)c], n);
                    else if (layer.region.auditionPair < 0)
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
    Voice(
        const DesignLayer &l, double rate, bool invariant = false, ChannelLayout layout = {}, int version = 2,
        const std::function<bool()> &cancelled = [] { return false; })
        : layer(l), ratio(l.source.sampleRate * l.rate / rate), duration(layerDuration(l, rate)),
          rack(prepareRack(l.effects, rate, invariant,
                           layout.id.isEmpty() ? 2 : l.routing.interpretation.size(), !layout.id.isEmpty())),
          layoutAware(!layout.id.isEmpty()), inputChannels(layoutAware ? l.routing.interpretation.size() : 2),
          routingVersion(version), bus(layout), designRate(rate), previousIn(l.fadeInShape, l.fadeInPoints),
          previousOut(l.fadeOutShape, l.fadeOutPoints), currentIn(previousIn), currentOut(previousOut)
    {
        if (!l.hosted.slots.empty())
            hosted = std::make_unique<HostedRackClient>(
                l.hosted, layout.id.isEmpty() ? channelLayout("stereo-v1") : l.routing.interpretation, rate,
                cancelled);
        if (l.source.error.isNotEmpty())
            throw std::runtime_error((l.name + ": " + l.source.error).toStdString());
        sourceUnchanged(l.source);
        formats.registerBasicFormats();
        reader.reset(formats.createReaderFor(juce::File(l.source.path)));
        if (!reader || reader->sampleRate != l.source.sampleRate ||
            reader->lengthInSamples != l.source.samples || (int)reader->numChannels != l.source.channels)
            throw std::runtime_error("Source header changed: " + l.source.name.toStdString());
        scratch.setSize(l.source.channels, 8192);
        block.setSize(inputChannels, 512);
        for (auto &c : cache)
            c.data.setSize(inputChannels, 8192);
        if (layoutAware)
        {
            matrix = routingMatrix(l.routing, bus, l.pan);
            for (int c = 0; c < bus.size(); ++c)
                for (int k = 0; k < inputChannels; ++k)
                {
                    matrixGains[c][k].reset(rate, .005);
                    matrixGains[c][k].setCurrentAndTargetValue(matrix[c][k]);
                }
            azimuth.reset(rate, .01);
            width.reset(rate, .01);
            azimuth.setCurrentAndTargetValue(l.routing.azimuth);
            width.setCurrentAndTargetValue(l.routing.width);
            lfeIndex = bus.roles.indexOf("LFE");
            lfeFilter.coefficients =
                juce::dsp::IIR::Coefficients<float>::makeLowPass(rate, l.routing.lfeCutoff);
            lfeFilter.reset();
        }
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
    void add(juce::AudioBuffer<float> &output, int offset, int frames, int64_t frame, int64_t inputEnd = -1)
    {
        block.clear();
        for (int n = 0; n < frames; ++n)
        {
            auto local = frame + n - layer.placement;
            auto left = leftGain.getNextValue(), right = rightGain.getNextValue();
            auto fadeIn = inFrames.getNextValue(), fadeOut = outFrames.getNextValue();
            auto inBlend = inShapeMix.getNextValue(), outBlend = outShapeMix.getNextValue();
            if (local < 0 || local >= duration || (inputEnd >= 0 && frame + n >= inputEnd))
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
            if (layoutAware)
                for (int c = 0; c < inputChannels; ++c)
                    block.setSample(c, n, interpolated(c, position) * left * envelope);
            else
            {
                block.setSample(0, n, interpolated(0, position) * left * envelope);
                block.setSample(1, n, interpolated(1, position) * right * envelope);
            }
        }
        processRack(rack, block, frames);
        if (hosted)
            hosted->process(block, frames);
        if (alignmentFrames)
            for (int n = 0; n < frames; ++n)
            {
                for (int c = 0; c < inputChannels; ++c)
                {
                    auto sample = block.getSample(c, n);
                    block.setSample(c, n, alignment.getSample(c, alignmentPosition));
                    alignment.setSample(c, alignmentPosition, sample);
                }
                alignmentPosition = (alignmentPosition + 1) % alignmentFrames;
            }
        if (!layoutAware)
        {
            for (int c = 0; c < 2; ++c)
                output.addFrom(c, offset, block, c, 0, frames);
        }
        else
            for (int n = 0; n < frames; ++n)
            {
                if (layer.routing.spatial)
                {
                    auto angle = azimuth.getNextValue(), spread = width.getNextValue();
                    for (int k = 0; k < inputChannels; ++k)
                    {
                        auto g = spatialGains(
                            bus, angle + (inputChannels == 2 ? (k == 0 ? -spread : spread) * .5 : 0),
                            layer.routing.divergence, routingVersion);
                        float value = block.getSample(k, n) * (inputChannels == 2 ? .70710678f : 1.f);
                        for (int c = 0; c < bus.size(); ++c)
                            output.addSample(c, offset + n, value * g[c]);
                    }
                    if (lfeIndex >= 0 && layer.routing.lfeDb > -90)
                    {
                        float value = 0;
                        for (int k = 0; k < inputChannels; ++k)
                            value += block.getSample(k, n) / inputChannels;
                        output.addSample(lfeIndex, offset + n,
                                         lfeFilter.processSample(value) *
                                             (float)juce::Decibels::decibelsToGain(layer.routing.lfeDb));
                    }
                }
                else
                    for (int c = 0; c < bus.size(); ++c)
                        for (int k = 0; k < inputChannels; ++k)
                            output.addSample(c, offset + n,
                                             block.getSample(k, n) * matrixGains[c][k].getNextValue());
            }
    }
    int latency() const
    {
        return hosted ? hosted->latency() : 0;
    }
    void compensate(int frames)
    {
        alignmentFrames = frames;
        if (frames)
        {
            alignment.setSize(inputChannels, frames);
            alignment.clear();
        }
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
        if (layoutAware)
        {
            matrix = routingMatrix(updated.routing, bus, updated.pan);
            for (int c = 0; c < bus.size(); ++c)
                for (int k = 0; k < inputChannels; ++k)
                    matrixGains[c][k].setTargetValue(matrix[c][k]);
            double target = updated.routing.azimuth, current = azimuth.getCurrentValue();
            if (routingVersion == 2)
            {
                double delta = std::fmod(target - current, 360.);
                if (delta > 180.)
                    delta -= 360.;
                if (delta < -180.)
                    delta += 360.;
                target = current + delta;
            }
            else
            {
                while (target - current > 180)
                    target -= 360;
                while (target - current < -180)
                    target += 360;
            }
            azimuth.setTargetValue(target);
            width.setTargetValue(updated.routing.width);
        }
        layer = updated;
        duration = layerDuration(layer, designRate);
        ratio = layer.source.sampleRate * layer.rate / designRate;
        if (rateChanged)
            buildCoefficients();
        gainTargets(false);
        updateRack(rack, layer.effects);
        if (hosted)
            hosted->update(layer.hosted);
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
    DesignDocument takeBase;
    std::vector<TakeEvent> events;
    size_t nextEvent = 0;
    int64_t inputEnd = -1;
    juce::SmoothedValue<float> masterGain;
    std::vector<std::unique_ptr<Voice>> voices;
    Rack masterRack;
    std::unique_ptr<HostedRackClient> hostedMaster;
    int pipelineLatency = 0;
    bool primed = false;
    juce::AudioBuffer<float> block{2, 512};
    explicit Impl(const DesignDocument &d, const std::function<bool()> &cancelled,
                  const DesignDocument *effectiveInput)
        : sampleRate(d.sampleRate), master(d.activeMix().masterDb),
          document(resolvePerformance(audioSourceProjection(d)))
    {
        validateDesign(d);
        if (auto *t = selectedTake(d))
        {
            takeBase = effectiveInput ? *effectiveInput : takeDocument(d);
            document = resolvePerformance(takeBase);
            sampleRate = t->sampleRate;
            master = document.activeMix().masterDb;
            events.assign(t->events.begin(), t->events.end());
            inputEnd = t->bodyEnd;
        }
        master = document.activeMix().masterDb;
        masterGain.reset(sampleRate, .005);
        masterGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain((float)master));
        bool invariant = d.takeProcessing || selectedTake(d);
        auto &mix = document.activeMix();
        int channels = mix.layout.id.isEmpty() ? 2 : mix.layout.size();
        block.setSize(channels, 512);
        masterRack = prepareRack(mix.masterEffects, sampleRate, invariant || !mix.layout.id.isEmpty(),
                                 channels, !mix.layout.id.isEmpty());
        if (!mix.hosted.slots.empty())
            hostedMaster = std::make_unique<HostedRackClient>(
                mix.hosted, mix.layout.id.isEmpty() ? channelLayout("stereo-v1") : mix.layout, sampleRate,
                cancelled);
        bool solo = std::any_of(document.activeMix().layers.begin(), document.activeMix().layers.end(),
                                [](auto &l) { return l.solo; });
        for (auto &l : document.activeMix().layers)
            if (!l.mute && (!solo || l.solo))
            {
                if (cancelled())
                    throw QueryCancelled();
                if (!document.activeMix().layout.id.isEmpty() && l.routing.needsSelection)
                    throw std::runtime_error("Choose supported source channels in Routing for: " +
                                             l.name.toStdString());
                voices.push_back(std::make_unique<Voice>(
                    l, sampleRate, invariant || !document.activeMix().layout.id.isEmpty(),
                    document.activeMix().layout, document.activeMix().routingVersion, cancelled));
            }
        int longest = 0;
        for (auto &voice : voices)
            longest = std::max(longest, voice->latency());
        for (auto &voice : voices)
            voice->compensate(longest - voice->latency());
        pipelineLatency = longest + (hostedMaster ? hostedMaster->latency() : 0);
    }
    void prime(int64_t first, const std::function<bool()> &cancelled)
    {
        if (primed)
            return;
        cursor = first;
        const auto end = first + pipelineLatency;
        juce::AudioBuffer<float> discard(block.getNumChannels(), 512);
        while (cursor < end)
        {
            if (cancelled())
                throw QueryCancelled();
            render(discard, 0, (int)std::min<int64_t>(512, end - cursor));
        }
        primed = true;
    }
    void render(juce::AudioBuffer<float> &out, int offset, int count)
    {
        for (int done = 0; done < count;)
        {
            while (nextEvent < events.size() && events[nextEvent].frame == cursor)
            {
                applyMixControls(takeBase.activeMix(), events[nextEvent++].controls);
                auto p = designParameters(takeBase);
                applyDesignParameters(document, p);
                for (auto &l : document.activeMix().layers)
                    for (auto &v : voices)
                        if (v->id() == l.id)
                            v->update(l);
                masterGain.setTargetValue(
                    juce::Decibels::decibelsToGain((float)document.activeMix().masterDb));
                updateRack(masterRack, document.activeMix().masterEffects);
            }
            int n = std::min(512, count - done);
            if (nextEvent < events.size())
                n = (int)std::min<int64_t>(n, events[nextEvent].frame - cursor);
            block.clear();
            if (lastVerified < 0 || cursor - lastVerified >= sampleRate)
            {
                for (auto &voice : voices)
                    voice->verify();
                lastVerified = cursor;
            }
            for (auto &voice : voices)
                voice->add(block, 0, n, cursor, inputEnd);
            processRack(masterRack, block, n);
            if (hostedMaster)
                hostedMaster->process(block, n);
            for (int i = 0; i < n; ++i)
            {
                auto gain = masterGain.getNextValue();
                for (int c = 0; c < block.getNumChannels(); ++c)
                    block.setSample(c, i, block.getSample(c, i) * gain);
            }
            for (int c = 0; c < block.getNumChannels(); ++c)
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
    : DesignGraph(d, cancelled, nullptr)
{
}
DesignGraph::DesignGraph(const DesignDocument &d, const std::function<bool()> &cancelled,
                         const DesignDocument *effectiveInput)
    : impl(std::make_unique<Impl>(d, cancelled, effectiveInput))
{
}
void DesignGraph::updateParameters(const juce::var &parameters)
{
    if (impl->inputEnd >= 0)
        return; // A frozen take only accepts its internal event timeline.
    applyDesignParameters(impl->document, parameters);
    for (auto &layer : impl->document.activeMix().layers)
        for (auto &voice : impl->voices)
            if (voice->id() == layer.id)
                voice->update(layer);
    impl->masterGain.setTargetValue(
        juce::Decibels::decibelsToGain((float)impl->document.activeMix().masterDb));
    updateRack(impl->masterRack, impl->document.activeMix().masterEffects);
    if (impl->hostedMaster)
        impl->hostedMaster->update(impl->document.activeMix().hosted);
}
DesignGraph::~DesignGraph() = default;
void DesignGraph::render(juce::AudioBuffer<float> &out, int offset, int count)
{
    impl->prime(0, [] { return false; });
    impl->render(out, offset, count);
}
void DesignGraph::seek(int64_t frame, const std::function<bool()> &cancelled, int64_t warmup)
{
    impl->prime(warmup >= 0 ? std::max<int64_t>(0, frame - warmup) : 0, cancelled);
    frame += impl->pipelineLatency;
    if (frame < impl->cursor)
        throw std::runtime_error("A seek requires fresh processing state");
    juce::AudioBuffer<float> discard(impl->block.getNumChannels(), 512);
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
    return impl->primed ? impl->cursor - impl->pipelineLatency : 0;
}
double DesignGraph::rate() const
{
    return impl->sampleRate;
}
struct DesignReader::Impl : juce::AudioSource
{
    DesignDocument document;
    DesignGraph graph;
    const bool immutableReplay;
    int64_t firstFrame, lastOverride;
    double tailSeconds;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    int64_t count = 0, total = 0, end;
    double outputRate;
    std::optional<PerformControls> pendingPerformance;
    TakeCapture *capture = nullptr;
    Impl(const DesignDocument &d, double rate, int64_t first, int64_t last, double tail,
         const std::function<bool()> &cancelled, int64_t warmup, const DesignDocument *effectiveInput)
        : document(effectiveInput    ? *effectiveInput
                   : selectedTake(d) ? takeDocument(d)
                                     : audioSourceProjection(d)),
          graph(d, cancelled, &document), immutableReplay(selectedTake(d) != nullptr), firstFrame(first),
          lastOverride(last), tailSeconds(tail),
          end((last < 0 ? mixDuration(document.activeMix(), document.sampleRate) : last) +
              (int64_t)std::llround(tail * document.sampleRate)),
          outputRate(rate)
    {
        if (first < 0 || end <= first || end > d.sampleRate * 3630 || rate < 8000 || rate > 192000)
            throw std::runtime_error("Invalid design playback/render range");
        graph.seek(first, cancelled, selectedTake(d) ? -1 : warmup);
        total = (int64_t)std::ceil((long double)(end - first) * rate / document.sampleRate);
        if (auto *t = selectedTake(d))
            total =
                (int64_t)std::ceil((long double)t->outputFrames * rate / t->captureRate +
                                   (long double)std::llround(tail * t->sampleRate) * rate / t->sampleRate) -
                (int64_t)std::ceil((long double)first * rate / t->sampleRate);
        if (rate != document.sampleRate)
        {
            resampler = std::make_unique<juce::ResamplingAudioSource>(this, false, designChannels(document));
            resampler->setResamplingRatio(document.sampleRate / rate);
            resampler->prepareToPlay(4096, rate);
        }
    }
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo &b) override
    {
        if (pendingPerformance && capture && b.numSamples > 0)
        {
            auto controls = *pendingPerformance;
            pendingPerformance.reset();
            if (!capture->append(graph.position(), controls))
                throw std::runtime_error("Take event capacity exceeded");
            applyMixControls(document.activeMix(), controls);
            graph.updateParameters(designParameters(document));
        }
        graph.render(*b.buffer, b.startSample, b.numSamples);
    }
};
DesignReader::DesignReader(const DesignDocument &d, double outputRate, int64_t first, int64_t last,
                           double tail, const std::function<bool()> &cancelled, int64_t warmup)
    : DesignReader(d, outputRate, first, last, tail, cancelled, warmup, nullptr)
{
}
DesignReader::DesignReader(const DesignDocument &d, double outputRate, int64_t first, int64_t last,
                           double tail, const std::function<bool()> &cancelled, int64_t warmup,
                           const DesignDocument *effectiveInput)
    : impl(std::make_unique<Impl>(d, outputRate, first, last, tail, cancelled, warmup, effectiveInput))
{
}
void DesignReader::updateParameters(const juce::var &parameters)
{
    if (impl->immutableReplay)
        return;
    applyDesignParameters(impl->document, parameters);
    impl->graph.updateParameters(parameters);
    impl->end = (impl->lastOverride < 0 ? mixDuration(impl->document.activeMix(), impl->document.sampleRate)
                                        : impl->lastOverride) +
                (int64_t)std::llround(impl->tailSeconds * impl->document.sampleRate);
    impl->total = std::max<int64_t>(0, (int64_t)std::ceil((long double)(impl->end - impl->firstFrame) *
                                                          impl->outputRate / impl->document.sampleRate));
}
void DesignReader::updatePerformance(const PerformControls &controls, TakeCapture &capture)
{
    // Latest full typed state wins until the next actual source production.
    // No event is emitted merely because a command arrived while FIFO is full.
    impl->pendingPerformance = controls;
    impl->capture = &capture;
}
DesignReader::~DesignReader() = default;
int DesignReader::read(juce::AudioBuffer<float> &buffer, int frames, const std::function<bool()> &cancelled)
{
    if (cancelled())
        throw QueryCancelled();
    int n = (int)std::min<int64_t>(frames, impl->total - impl->count);
    if (buffer.getNumChannels() != designChannels(impl->document) || buffer.getNumSamples() < frames)
        buffer.setSize(designChannels(impl->document), frames);
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
int64_t DesignReader::producedFrame() const
{
    return impl->graph.position();
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
    // Freeze the actual verified relocation once for metering, PCM and final
    // publication guards. Unrelated Live Design paths never enter this snapshot.
    const auto effectiveInput = selectedTake(d) ? takeDocument(d) : d;
    const bool layoutAware = !effectiveInput.activeMix().layout.id.isEmpty() || settings.layout.isNotEmpty();
    const auto bus = designLayout(d),
               delivery = layoutAware ? exportLayout(d, settings)
                                      : channelLayout(settings.channels == 1 ? "mono-v1" : "stereo-v1");
    const int channels = delivery.size();
    const auto conversion = layoutAware ? conversionMatrix(bus, delivery) : ChannelMatrix{};
    const auto order = wavOrder(delivery);
    juce::AudioBuffer<float> converted(channels, 4096);
    auto convert = [&](const juce::AudioBuffer<float> &input, int n)
    {
        converted.clear();
        if (layoutAware)
        {
            for (int c = 0; c < channels; ++c)
                for (int k = 0; k < bus.size(); ++k)
                    converted.addFrom(c, 0, input, k, 0, n, conversion[c][k]);
        }
        else if (channels == 1)
        {
            for (int i = 0; i < n; ++i)
                converted.setSample(0, i, (input.getSample(0, i) + input.getSample(1, i)) * .5f);
        }
        else
            for (int c = 0; c < 2; ++c)
                converted.copyFrom(c, 0, input, c, 0, n);
    };
    const int64_t estimated = (int64_t)std::ceil(
        (long double)((settings.last < 0 ? mixDuration(effectiveInput.activeMix(), effectiveInput.sampleRate)
                                         : settings.last) -
                      settings.first) *
            settings.sampleRate / effectiveInput.sampleRate +
        settings.tailSeconds * settings.sampleRate);
    if (estimated < 0 || (long double)estimated * channels * (settings.bits / 8) >= 4293918720.L)
        throw std::runtime_error("Output exceeds qualified RIFF WAV size (4GiB minus1MiB). Shorten the range "
                                 "or lower rate/depth; RF64 is not qualified.");
    if (!folder.createDirectory())
        throw std::runtime_error("Cannot create output folder");
    auto name = juce::File::createLegalFileName(selectedTake(d) ? selectedTake(d)->name : d.name) +
                (selectedTake(d)               ? "-take"
                 : d.activeVariation.isEmpty() ? ""
                                               : "-variation") +
                "-r" + juce::String(d.revision) + "-" + juce::Uuid().toString().substring(0, 8) + ".wav";
    auto target = folder.getNonexistentChildFile(name, {}, false);
    juce::TemporaryFile staging(target, juce::TemporaryFile::useHiddenFile);
    juce::AudioBuffer<float> block(designChannels(d), 4096);
    double normalize = 1;
    std::array<double, 16> channelPeak{};
    std::array<int64_t, 16> channelClips{};
    juce::AudioBuffer<float> ordered(channels, 4096);
    const bool hostedPass = hasHostedInserts(effectiveInput.activeMix());
    juce::File rawPass;
    juce::String rawPassHash;
    std::unique_ptr<juce::AudioFormatReader> retained;
    juce::AudioFormatManager retainedFormats;
    int64_t passFrames = 0, passRead = 0;
    if (hostedPass)
    {
        // Run arbitrary effects exactly once. Meter/normalization/encoding/splits
        // consume this durable float pass, never another graph instantiation.
        auto passFolder = folder.getChildFile("Plugin Passes").getChildFile(juce::Uuid().toString());
        if (!passFolder.createDirectory())
            throw std::runtime_error("Cannot retain plugin pass");
        rawPass = passFolder.getChildFile("Raw.wav");
        juce::TemporaryFile pending(rawPass);
        juce::WavAudioFormat wav;
        juce::StringPairArray meta;
        meta.set(juce::WavAudioFormat::bwavDescription,
                 "SoundPlus layout:" + delivery.id + "; order:" + delivery.roles.joinIntoString(","));
        std::unique_ptr<juce::AudioFormatWriter> rawWriter(
            wav.createWriterFor(pending.getFile().createOutputStream().release(), settings.sampleRate,
                                juceChannelSet(delivery), 32, meta, 0));
        if (!rawWriter)
            throw std::runtime_error("Cannot retain float plugin pass");
        DesignReader source(d, settings.sampleRate, settings.first, settings.last, settings.tailSeconds,
                            cancelled, -1, &effectiveInput);
        double peak = 0;
        while (auto n = source.read(block, 4096, cancelled))
        {
            convert(block, n);
            for (int c = 0; c < channels; ++c)
            {
                ordered.copyFrom(c, 0, converted, order[(size_t)c], 0, n);
                peak = std::max(peak, (double)converted.getMagnitude(c, 0, n));
            }
            if (!rawWriter->writeFromAudioSampleBuffer(ordered, 0, n))
                throw std::runtime_error("Plugin pass write failed");
            passFrames += n;
            progress(.4 * (double)source.consumed() / source.length());
        }
        rawWriter.reset();
        if (cancelled())
            throw QueryCancelled();
        if (!pending.overwriteTargetFileWithTemporary())
            throw std::runtime_error("Cannot finalize retained plugin pass");
        rawPassHash = processingHash(rawPass);
        auto record = juce::var(new juce::DynamicObject());
        auto *r = record.getDynamicObject();
        r->setProperty("version", 1);
        r->setProperty("path", rawPass.getFullPathName());
        r->setProperty("sha256", rawPassHash);
        r->setProperty("graph", designMixToJson(effectiveInput.activeMix()));
        r->setProperty("frames", (juce::int64)passFrames);
        r->setProperty("rate", settings.sampleRate);
        r->setProperty("layout", layoutJson(delivery));
        saveProcessingJson(passFolder.getChildFile("pass.json"), record);
        if (settings.normalize && peak > 0)
            normalize = std::pow(10., -1. / 20) / peak;
        retainedFormats.registerBasicFormats();
        retained.reset(retainedFormats.createReaderFor(rawPass));
        if (!retained || retained->lengthInSamples != passFrames)
            throw std::runtime_error("Retained plugin pass unavailable");
    }
    if (settings.normalize && !hostedPass)
    {
        DesignReader meter(d, settings.sampleRate, settings.first, settings.last, settings.tailSeconds,
                           cancelled, -1, &effectiveInput);
        double peak = 0;
        while (auto n = meter.read(block, 4096, cancelled))
        {
            convert(block, n);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < n; ++i)
                    peak = std::max(peak, (double)std::abs(converted.getSample(c, i)));
            progress(.4 * (double)meter.consumed() / meter.length());
        }
        if (peak > 0)
            normalize = std::pow(10.0, -1.0 / 20) / peak;
    }
    std::unique_ptr<DesignReader> reader;
    if (!hostedPass)
        reader.reset(new DesignReader(d, settings.sampleRate, settings.first, settings.last,
                                      settings.tailSeconds, cancelled, -1, &effectiveInput));
    auto totalFrames = hostedPass ? passFrames : reader->length();
    juce::AudioBuffer<float> retainedBlock(channels, 4096);
    auto readNext = [&]()
    {
        if (!hostedPass)
            return reader->read(block, 4096, cancelled);
        if (cancelled())
            throw QueryCancelled();
        int n = (int)std::min<int64_t>(4096, passFrames - passRead);
        if (n && !retained->read(&retainedBlock, 0, n, passRead, true, true))
            throw std::runtime_error("Retained plugin pass read failed");
        converted.clear();
        for (int c = 0; c < channels; ++c)
            converted.copyFrom(order[(size_t)c], 0, retainedBlock, c, 0, n);
        passRead += n;
        return n;
    };
    juce::WavAudioFormat format;
    auto stream = staging.getFile().createOutputStream();
    if (!stream)
        throw std::runtime_error("Cannot stage output");
    juce::StringPairArray metadata;
    if (layoutAware)
        metadata.set(juce::WavAudioFormat::bwavDescription,
                     "SoundPlus layout:" + delivery.id + "; order:" + delivery.roles.joinIntoString(","));
    std::unique_ptr<juce::AudioFormatWriter> writer(
        layoutAware ? format.createWriterFor(stream.release(), settings.sampleRate, juceChannelSet(delivery),
                                             settings.bits, metadata, 0)
                    : format.createWriterFor(stream.release(), settings.sampleRate,
                                             (unsigned)settings.channels, settings.bits, {}, 0));
    if (!writer)
        throw std::runtime_error("Cannot create WAV writer");
    DesignOutput result;
    result.path = target.getFullPathName();
    result.samples = totalFrames;
    if (hostedPass)
    {
        result.processing = "hosted-racks-v1";
        auto *p = new juce::DynamicObject();
        p->setProperty("rawPass", rawPass.getFullPathName());
        p->setProperty("rawSha256", rawPassHash);
        p->setProperty("graph", designMixToJson(effectiveInput.activeMix()));
        p->setProperty("authority", "single-float-pass-v1");
        result.sourceProvenance = juce::var(p);
    }
    result.revision = d.revision;
    result.variation = selectedTake(d) ? juce::String() : d.activeVariation;
    result.settings = settings;
    if (layoutAware)
    {
        result.settings.channels = channels;
        result.settings.layout = delivery.id;
        auto o = juce::var(new juce::DynamicObject());
        auto *p = o.getDynamicObject();
        p->setProperty("version", 1);
        p->setProperty("design", layoutJson(bus));
        p->setProperty("export", layoutJson(delivery));
        if (!effectiveInput.activeMix().layout.id.isEmpty())
            p->setProperty("spatialRoutingVersion", effectiveInput.activeMix().routingVersion);
        if (auto *take = selectedTake(d); take && take->correctedFrom.isNotEmpty())
            p->setProperty("correctionOrigin", takeToJson(*take)["correctionOrigin"]);
        p->setProperty("conversion", "role-matrix-v1; shared normalization; LFE excluded from fold-down");
        juce::Array<juce::var> layers;
        for (auto &l : effectiveInput.activeMix().layers)
        {
            auto v = juce::var(new juce::DynamicObject());
            v.getDynamicObject()->setProperty("layer", l.id);
            v.getDynamicObject()->setProperty("source", l.source.id);
            v.getDynamicObject()->setProperty("detected", l.source.detectedChannels.id.isEmpty()
                                                              ? juce::var()
                                                              : layoutJson(l.source.detectedChannels));
            if (!effectiveInput.activeMix().layout.id.isEmpty())
                v.getDynamicObject()->setProperty("routing", routingJson(l.routing));
            layers.add(v);
        }
        p->setProperty("sources", layers);
        result.channelProvenance = o;
    }
    result.created = juce::Time::getCurrentTime().toISO8601(true);
    if (auto *t = selectedTake(d))
    {
        result.kind = "take-render";
        result.takeId = t->id;
        result.recipeHash = t->recipeHash;
        result.eventHash = t->eventHash;
        result.processing = t->processing;
        result.sourceProvenance = takeToJson(*t)["sources"];
        for (auto &source : *result.sourceProvenance.getArray())
            for (auto &layer : effectiveInput.activeMix().layers)
                if (source["id"].toString() == layer.source.id)
                {
                    auto *o = source.getDynamicObject();
                    auto origins = poolOriginsForSource(*t, layer.source.id);
                    if (!origins.isVoid())
                        o->setProperty("poolOrigins", origins);
                    o->setProperty("effectivePath", layer.source.path);
                    o->setProperty("revision", (juce::int64)layer.source.revision);
                    o->setProperty("size", (juce::int64)layer.source.size);
                    o->setProperty("modified", (juce::int64)layer.source.modified);
                    o->setProperty("inode", (juce::int64)layer.source.inode);
                    break;
                }
        result.projectRate = t->sampleRate;
        result.bodyEnd = t->bodyEnd;
        result.captureRate = t->captureRate;
        result.capturedFrames = t->outputFrames;
        result.tailPolicy = "Exact consumed output duration; source end rounds up for interpolation; effect "
                            "tails once from full history";
    }
    std::mt19937 rng(170526);
    std::array<std::mt19937, 16> channelRng;
    for (int c = 0; c < 16; ++c)
        channelRng[c].seed(170526u + 7919u * c);
    double lsb = std::ldexp(1.0, 1 - settings.bits);
    while (auto n = readNext())
    {
        if (!hostedPass)
            convert(block, n);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
            {
                double value = converted.getSample(c, i) * normalize;
                if (!std::isfinite(value))
                    throw std::runtime_error("Nonfinite export channel");
                channelPeak[c] = std::max(channelPeak[c], std::abs(value));
                if (std::abs(value) > 1)
                    ++channelClips[c];
                result.peak = std::max(result.peak, std::abs(value));
                if (std::abs(value) > 1)
                    ++result.clipped;
                if (settings.dither && settings.bits < 32)
                    value += layoutAware ? ((double)channelRng[c]() / channelRng[c].max() -
                                            (double)channelRng[c]() / channelRng[c].max()) *
                                               lsb
                                         : ((double)rng() / rng.max() - (double)rng() / rng.max()) * lsb;
                // New integer contract uses symmetric nearest quantization before JUCE's
                // integer writer. Its bit-depth reduction otherwise floors negative
                // sub-LSB silence and creates a shared DC bias. Legacy bytes stay verbatim.
                if (layoutAware && settings.bits < 32)
                    value = std::round(value / lsb) * lsb;
                if (layoutAware)
                {
                    if (!std::isfinite(value))
                        throw std::runtime_error("Nonfinite quantized export channel");
                    if (std::abs(value) > 1 && std::abs(converted.getSample(c, i) * normalize) <= 1)
                    {
                        ++channelClips[c];
                        ++result.clipped;
                    }
                    channelPeak[c] = std::max(channelPeak[c], std::abs(value));
                    result.peak = std::max(result.peak, std::abs(value));
                }
                converted.setSample(c, i, (float)value);
            }
        // Adapt by channel role indices, never a preset factory's prose order.
        for (int c = 0; c < channels; ++c)
            ordered.copyFrom(c, 0, converted, order[(size_t)c], 0, n);
        if (!writer->writeFromAudioSampleBuffer(ordered, 0, n))
            throw std::runtime_error("Output write failed; no file was published");
        progress((hostedPass || settings.normalize ? .4 : 0) +
                 (hostedPass || settings.normalize ? .6 : 1) *
                     (double)(hostedPass ? passRead : reader->consumed()) / totalFrames);
    }
    writer.reset();
    if (hostedPass && processingHash(rawPass) != rawPassHash)
        throw std::runtime_error("Retained raw plugin authority changed before publication");
    if (layoutAware)
    {
        juce::Array<juce::var> peaks, clipped;
        for (int c = 0; c < channels; ++c)
        {
            peaks.add(channelPeak[c]);
            clipped.add((juce::int64)channelClips[c]);
        }
        auto *p = result.channelProvenance.getDynamicObject();
        p->setProperty("channelPeaks", peaks);
        p->setProperty("channelClips", clipped);
        p->setProperty("sharedNormalizationGain", normalize);
        p->setProperty(
            "ditherPolicy",
            "independent-channel-tpdf-v1; seed170526+7919*channel; symmetric-nearest-quantization");
    }
    bool solo = std::any_of(effectiveInput.activeMix().layers.begin(),
                            effectiveInput.activeMix().layers.end(), [](auto &l) { return l.solo; });
    for (auto &l : effectiveInput.activeMix().layers)
        if (!l.mute && (!solo || l.solo))
            sourceUnchanged(l.source);
    syncFile(staging.getFile());
    if (cancelled())
        throw QueryCancelled();
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> verify(fm.createReaderFor(staging.getFile()));
    if (!verify || verify->lengthInSamples != result.samples || verify->sampleRate != settings.sampleRate ||
        (int)verify->numChannels != channels)
        throw std::runtime_error("Finalized WAV validation failed");
    verify.reset();
    auto bytes = staging.getFile().createInputStream();
    if (!bytes)
        throw std::runtime_error("Cannot verify rendered output");
    result.sha256 = juce::SHA256(*bytes).toHexString();
    bytes.reset();
    if (cancelled())
        throw QueryCancelled();
    if (settings.splitMono)
    {
        struct OwnedStage
        {
            juce::File path;
            bool published = false;
            ~OwnedStage()
            {
                if (!published)
                    path.deleteRecursively();
            }
        };
        auto setTarget =
            folder.getNonexistentChildFile(target.getFileNameWithoutExtension() + "-split", {}, false);
        OwnedStage setStage{folder.getChildFile(".split-" + juce::Uuid().toString())};
        if (!setStage.path.createDirectory() ||
            !staging.getFile().copyFileTo(setStage.path.getChildFile("Mix.wav")))
            throw std::runtime_error("Cannot stage split output set");
        std::unique_ptr<juce::AudioFormatReader> input(fm.createReaderFor(staging.getFile()));
        juce::Array<juce::var> files;
        std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers;
        for (int c = 0; c < channels; ++c)
        {
            auto f = setStage.path.getChildFile(delivery.roles[c] + ".wav");
            auto out = f.createOutputStream();
            if (!out)
                throw std::runtime_error("Cannot stage split channel");
            auto *w =
                format.createWriterFor(out.release(), settings.sampleRate, 1, settings.bits, metadata, 0);
            if (!w)
                throw std::runtime_error("Cannot create split channel");
            writers.emplace_back(w);
        }
        juce::AudioBuffer<float> pcm(channels, 4096), mono(1, 4096);
        for (int64_t at = 0; at < result.samples; at += 4096)
        {
            if (cancelled())
                throw QueryCancelled();
            int n = (int)std::min<int64_t>(4096, result.samples - at);
            if (!input->read(&pcm, 0, n, at, true, true))
                throw std::runtime_error("Cannot verify split source");
            for (int c = 0; c < channels; ++c)
            {
                mono.copyFrom(0, 0, pcm, c, 0, n);
                if (!writers[(size_t)c]->writeFromAudioSampleBuffer(mono, 0, n))
                    throw std::runtime_error("Split write failed; set not published");
            }
        }
        writers.clear();
        input.reset();
        for (int c = 0; c < channels; ++c)
        {
            auto f = setStage.path.getChildFile(delivery.roles[c] + ".wav");
            syncFile(f);
            auto bytes = f.createInputStream();
            if (!bytes)
                throw std::runtime_error("Cannot hash split channel");
            auto v = juce::var(new juce::DynamicObject());
            v.getDynamicObject()->setProperty("role", delivery.roles[c]);
            v.getDynamicObject()->setProperty("path",
                                              setTarget.getChildFile(f.getFileName()).getFullPathName());
            v.getDynamicObject()->setProperty("sha256", juce::SHA256(*bytes).toHexString());
            files.add(v);
        }
        result.files = files;
        result.outputSetVersion = 1;
        result.path = setTarget.getChildFile("Mix.wav").getFullPathName();
        auto manifest = juce::var(new juce::DynamicObject());
        manifest.getDynamicObject()->setProperty("files", files);
        manifest.getDynamicObject()->setProperty("printMasterSha256", result.sha256);
        manifest.getDynamicObject()->setProperty("layout", layoutJson(delivery));
        auto manifestFile = setStage.path.getChildFile("output-set.json");
        if (!manifestFile.replaceWithText(juce::JSON::toString(manifest, true)))
            throw std::runtime_error("Cannot finalize output-set receipt");
        syncFile(manifestFile);
        syncFile(setStage.path.getChildFile("Mix.wav"));
        for (auto &l : effectiveInput.activeMix().layers)
            if (!l.mute && (!solo || l.solo))
                sourceUnchanged(l.source);
        if (cancelled())
            throw QueryCancelled();
        if (!setStage.path.moveFileTo(setTarget))
            throw std::runtime_error("Cannot publish split set atomically");
        setStage.published = true;
        progress(1);
        return result;
    }
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
