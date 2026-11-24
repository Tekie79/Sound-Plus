#include "SurroundTouch.h"
#include "AdvancedIR.h"
#include "NativeSampler.h"
#include "NativeEffectsDSP.h"
#include "AdvancedEffects.h"
#include "DesignAudio.h"
#include "TrackModel.h"
#include "MixerMeters.h"
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
#include "OrderedRack.inc"
class Voice
{
    DesignLayer layer;
    bool continuation = false, ownsTail = true;
    int64_t gateFirst = 0, gateLast = 0;
    juce::AudioBuffer<float> gated;
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
    std::unique_ptr<OrderedRack> rack;
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
        bool fixed = false, const std::function<bool()> &cancelled = [] { return false; },
        MidiTiming timing = {})
        : layer(splitPlaybackLayer(l)), continuation(usesSplitHistory(l)),
          ownsTail(!continuation || l.splitHistory->ownsTail), gateFirst(l.placement),
          gateLast(l.placement + layerDuration(l, rate)), ratio(l.source.sampleRate * l.rate / rate),
          duration(layerDuration(layer, rate)),
          rack(std::make_unique<OrderedRack>(
              l.inserts, layout.id.isEmpty() ? channelLayout("stereo-v1") : l.routing.interpretation, rate,
              invariant, !layout.id.isEmpty(), fixed, cancelled, timing)),
          layoutAware(!layout.id.isEmpty()), inputChannels(layoutAware ? l.routing.interpretation.size() : 2),
          routingVersion(version), bus(layout), designRate(rate),
          previousIn(layer.fadeInShape, layer.fadeInPoints),
          previousOut(layer.fadeOutShape, layer.fadeOutPoints), currentIn(previousIn), currentOut(previousOut)
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
        block.setSize(inputChannels, 512);
        gated.setSize(layoutAware ? layout.size() : 2, 512);
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
    void add(juce::AudioBuffer<float> &destination, int destinationOffset, int frames, int64_t frame,
             int64_t inputEnd = -1)
    {
        auto &output = continuation ? gated : destination;
        int offset = continuation ? 0 : destinationOffset;
        if (continuation)
            gated.clear();
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
        rack->process(block, frames, frame);
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
        if (continuation)
            for (int n = 0; n < frames; ++n)
            {
                auto time = frame + n - rack->latency() - alignmentFrames;
                if (time >= gateFirst && (ownsTail || time < gateLast))
                    for (int c = 0; c < destination.getNumChannels(); ++c)
                        destination.addSample(c, destinationOffset + n, gated.getSample(c, n));
            }
    }
    void automate(const CompiledAutomation &a, int64_t frame)
    {
        rack->automate(a, frame);
    }
    void appendMeters(juce::Array<juce::var> &rows) const
    {
        rack->appendMeters(rows);
    }
    int dependencies() const
    {
        return rack->dependencies();
    }
    int latency() const
    {
        return rack->latency();
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
    void update(const DesignLayer &visible)
    {
        auto updated = splitPlaybackLayer(visible);
        gateFirst = visible.placement;
        gateLast = visible.placement + layerDuration(visible, designRate);
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
        rack->update(layer.inserts);
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
    std::optional<DesignDocument> padBase;
    std::optional<PerformControls> lastPadControls;
    std::optional<SquareTouchPass> squareTouch;
    void applyControlPacket(const juce::var &p)
    {
        applyDesignParameters(document, p);
        for (auto &layer : document.activeMix().layers)
            for (auto &voice : voices)
                if (voice->id() == layer.id)
                    voice->update(layer);
        masterGain.setTargetValue(juce::Decibels::decibelsToGain((float)document.activeMix().masterDb));
        masterRack->update(document.activeMix().inserts);
        refreshTracks();
    }
    void receivePadPacket(const juce::var &p)
    {
        if (!p["padBase"].isObject())
        {
            padBase.reset();
            lastPadControls.reset();
            return;
        }
        if (!padBase)
            padBase = document;
        applyDesignParameters(*padBase, p["padBase"]);
        padBase->activeMix().performance = performanceFromJson(p["padA"]);
        padsFromJson(padBase->activeMix(), p["performPads"]);
        lastPadControls.reset();
    }
    DesignDocument takeBase;
    std::vector<TakeEvent> events;
    size_t nextEvent = 0;
    int64_t inputEnd = -1;
    juce::SmoothedValue<float> masterGain;
    std::vector<std::unique_ptr<Voice>> voices;
    std::unique_ptr<OrderedRack> masterRack;
    int pipelineLatency = 0, pipelineDependencies = 0;
    bool primed = false;
    juce::AudioBuffer<float> block{2, 512};
    struct SendEdge
    {
        int destination = -1, delay = 0, position = 0;
        juce::AudioBuffer<float> alignment;
        std::array<std::array<juce::SmoothedValue<float>, 16>, 16> pan;
        juce::SmoothedValue<float> level;
        AutomationTarget levelTarget, enableTarget;
        bool levelAutomated = false;
        bool audible = true;
    };
    struct TrackNode
    {
        DesignTrack track;
        std::unique_ptr<NativeSampler> sampler;
        std::unique_ptr<HostedRackClient> instrument;
        bool panic = false, endedMidi = false;
        std::vector<MidiPacket> midi;
        size_t nextMidi = 0;
        ChannelLayout bus, destinationBus;
        LayerRouting route;
        std::array<std::array<juce::SmoothedValue<float>, 16>, 16> matrixGains;
        std::unique_ptr<OrderedRack> rack;
        std::unique_ptr<CompiledAutomation> automation;
        std::vector<Voice *> clips;
        juce::AudioBuffer<float> pcm, alignment, legacyClips;
        MixerMeter meter;
        ChannelMatrix legacyConversion{};
        bool legacyInput = false;
        bool audible = true, mainAudible = true, volumeAutomated = false;
        std::vector<SendEdge> sends;
        int destination = -1, inputLatency = 0, outputLatency = -1, inputDependencies = 0,
            outputDependencies = 0, alignFrames = 0, alignPosition = 0;
        int stem = -1;
        juce::SmoothedValue<float> gain;
    };
    std::vector<std::unique_ptr<TrackNode>> tracks;
    std::vector<int> trackOrder;
    std::unique_ptr<CompiledAutomation> masterAutomation;
    int masterInputLatency = 0;
    MixerMeter masterMeter;
    struct StemTap
    {
        ChannelMatrix numbering{};
        juce::AudioBuffer<float> history, pcm;
        int delay = 0, index = 0;
    };
    std::vector<StemTap> stemTaps;
    void prepareStemTaps(const juce::StringArray &owners, const ChannelLayout &delivery)
    {
        size_t samples = 0;
        stemTaps.reserve((size_t)owners.size());
        for (const auto &owner : owners)
        {
            auto found = std::find_if(tracks.begin(), tracks.end(), [&](const auto &node)
            { return node->track.id == owner; });
            if (found == tracks.end())
                throw std::runtime_error("Choose an audio-processing stem owner");
            auto &node = **found;
            node.stem = (int)stemTaps.size();
            auto &tap = stemTaps.emplace_back();
            tap.numbering = conversionMatrix(node.destinationBus, delivery);
            const auto receivingTime = node.destination < 0 ? masterInputLatency
                : tracks[(size_t)node.destination]->inputLatency;
            tap.delay = pipelineLatency - receivingTime;
            samples += (size_t)delivery.size() * (tap.delay + 512);
            if (samples > 48 * 1024 * 1024)
                throw std::runtime_error("Aligned stems exceed the prepared PCM budget");
            tap.history.setSize(delivery.size(), tap.delay);
            tap.history.clear();
            tap.pcm.setSize(delivery.size(), 512);
        }
    }
    void updateMixerSolo()
    {
        const auto &mix = document.activeMix();
        if (!hasMixerRouting(mix))
            return;
        const bool solo = std::any_of(mix.tracks.begin(), mix.tracks.end(), [](const auto &t) { return t.solo; });
        for (auto &node : tracks)
        {
            node->mainAudible = !solo;
            for (auto &edge : node->sends)
                edge.audible = !solo;
        }
        std::vector<bool> outgoing(tracks.size()), incoming(tracks.size());
        auto enabled = [](const TrackNode &node, size_t index)
        {
            const auto &send = node.track.sends[index];
            auto *curve = automationCurve(node.track.automation, {send.id, "send-enable"});
            return send.enabled || (curve && curve->read && !curve->points.empty());
        };
        std::function<void(int)> forward = [&](int index)
        {
            if (index < 0 || outgoing[(size_t)index])
                return;
            outgoing[(size_t)index] = true;
            auto &node = *tracks[(size_t)index];
            node.mainAudible = true;
            forward(node.destination);
            for (size_t i = 0; i < node.sends.size(); ++i)
                if (enabled(node, i))
                {
                    node.sends[i].audible = true;
                    forward(node.sends[i].destination);
                }
        };
        std::function<void(int, bool)> backward = [&](int index, bool feedAux)
        {
            if (incoming[(size_t)index])
                return;
            incoming[(size_t)index] = true;
            for (size_t i = 0; i < tracks.size(); ++i)
            {
                auto &source = *tracks[i];
                if (source.destination == index)
                {
                    source.mainAudible = true;
                    if (feedAux)
                        forward((int)i);
                    backward((int)i, feedAux);
                }
                for (size_t s = 0; s < source.sends.size(); ++s)
                    if (source.sends[s].destination == index && enabled(source, s))
                    {
                        source.sends[s].audible = true;
                        backward((int)i, false);
                    }
            }
        };
        if (solo)
            for (size_t i = 0; i < tracks.size(); ++i)
            {
                auto &t = tracks[i]->track;
                bool selected = t.solo;
                for (auto parent = t.parent; parent.isNotEmpty();)
                {
                    auto *folder = findTrack(mix, parent);
                    selected |= folder->solo;
                    parent = folder->parent;
                }
                if (selected)
                {
                    forward((int)i);
                    backward((int)i, t.kind == TrackKind::groupBus);
                }
            }
        for (auto &node : tracks)
        {
            bool audible = !node->track.mute && (node->mainAudible || std::any_of(
                node->sends.begin(), node->sends.end(), [](const auto &edge) { return edge.audible; }));
            for (auto parent = node->track.parent; parent.isNotEmpty();)
            {
                auto *folder = findTrack(mix, parent);
                audible &= !folder->mute;
                parent = folder->parent;
            }
            if (node->audible && !audible)
            {
                node->panic = true;
                if (node->sampler)
                    node->sampler->allNotesOff(true);
            }
            node->audible = audible;
        }
    }
    void prepareTracks(bool fixed, const std::function<bool()> &cancelled)
    {
        auto &mix = document.activeMix();
        masterAutomation = std::make_unique<CompiledAutomation>(mix, "master");
        for (auto &t : mix.tracks)
        {
            if (t.kind == TrackKind::folder)
                continue;
            auto node = std::make_unique<TrackNode>();
            node->track = t;
            node->meter.prepare(sampleRate);
            if (t.kind == TrackKind::midiInstrument)
            {
                node->midi = compileMidi(mix, t.id, sampleRate);
                if (t.instrument && t.instrument->version == 1)
                {
                    if (t.instrument->format == "SoundPlus")
                        node->sampler = std::make_unique<NativeSampler>(*t.instrument, t.layout, sampleRate);
                    else if (t.instrument->format != "none")
                    {
                        auto rack = t.instrument->hostedInstrument.hosted;
                        rack.runtimeFixedState = fixed;
                        node->instrument =
                            std::make_unique<HostedRackClient>(rack, t.layout, sampleRate, cancelled);
                        node->inputLatency = node->instrument->latency();
                        node->inputDependencies = node->inputLatency;
                    }
                }
                else if (!node->midi.empty())
                    throw std::runtime_error("Choose an Instrument for MIDI Track: " + t.name.toStdString());
            }
            node->audible = trackAudible(mix, t.id);
            auto *volume = automationCurve(t.automation, {{}, "volume"});
            node->volumeAutomated = volume && volume->read;
            node->bus = t.layout;
            node->rack = std::make_unique<OrderedRack>(t.inserts, t.layout, sampleRate, true, true, fixed,
                                                       cancelled, mix.midiTiming);
            node->automation = std::make_unique<CompiledAutomation>(mix, t.id);
            node->pcm.setSize(t.layout.size(), 512);
            node->pcm.clear();
            node->legacyInput = mix.layout.id.isEmpty() && t.layout != channelLayout("stereo-v1");
            if (node->legacyInput)
            {
                node->legacyClips.setSize(2, 512);
                node->legacyConversion = conversionMatrix(channelLayout("stereo-v1"), t.layout);
            }
            node->gain.reset(sampleRate, .005);
            node->gain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain((float)t.volumeDb));
            for (auto &clip : mix.layers)
                if (clip.trackId == t.id)
                    for (auto &v : voices)
                        if (v->id() == clip.id)
                        {
                            node->clips.push_back(v.get());
                            node->inputLatency = std::max(node->inputLatency, v->latency());
                            node->inputDependencies = std::max(node->inputDependencies, v->dependencies());
                        }
            for (auto *v : node->clips)
                v->compensate(node->inputLatency - v->latency());
            tracks.push_back(std::move(node));
        }
        size_t samplerFloats = 0;
        for (auto &node : tracks)
            if (node->sampler)
                samplerFloats += node->sampler->sourceFloats();
        if (samplerFloats > 64 * 1024 * 1024)
            throw std::runtime_error("Sampler project exceeds prepared PCM budget");
        for (auto &node : tracks)
        {
            if (node->track.output.isNotEmpty())
            {
                for (size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i]->track.id == node->track.output)
                        node->destination = (int)i;
                if (node->destination < 0)
                    throw std::runtime_error("Required output Group Bus is unavailable");
            }
            node->destinationBus =
                node->destination < 0 ? designLayout(document) : tracks[(size_t)node->destination]->bus;
            node->route.interpretation = node->bus;
            for (int c = 0; c < node->bus.size(); ++c)
                node->route.inputs.push_back(c);
            auto matrix = trackMatrix(node->track, node->destinationBus, node->track.pan, node->track.azimuth,
                                      node->track.width);
            if (auto position = node->track.surroundXY)
            {
                position->x = node->automation->value(surroundX, position->x, 0);
                position->y = node->automation->value(surroundY, position->y, 0);
                position->spread = node->automation->value(surroundSpread, position->spread, 0);
                matrix = squareSurroundMatrix(node->bus, node->destinationBus, *position);
            }
            for (int c = 0; c < node->destinationBus.size(); ++c)
                for (int k = 0; k < node->bus.size(); ++k)
                {
                    node->matrixGains[(size_t)c][(size_t)k].reset(sampleRate,
                                                                  node->track.surroundXY ? .01 : .005);
                    node->matrixGains[(size_t)c][(size_t)k].setCurrentAndTargetValue(matrix[c][k]);
                }
            node->sends.reserve(node->track.sends.size());
            for (const auto &send : node->track.sends)
            {
                auto &edge = node->sends.emplace_back();
                auto bus = std::find_if(tracks.begin(), tracks.end(), [&](const auto &candidate)
                { return candidate->track.id == send.destination; });
                if (bus == tracks.end())
                    throw std::runtime_error("FX send destination is unavailable");
                edge.destination = (int)std::distance(tracks.begin(), bus);
                const auto &layout = (*bus)->bus;
                auto sendPan = sendMatrix(node->track, send, layout, node->track.pan, node->track.azimuth,
                                          node->track.width, node->track.surroundXY);
                edge.level.reset(sampleRate, .005);
                edge.level.setCurrentAndTargetValue(juce::Decibels::decibelsToGain((float)send.levelDb, -90.f));
                edge.levelTarget = {send.id, "send-level"};
                edge.enableTarget = {send.id, "send-enable"};
                const auto *curve = automationCurve(node->track.automation, edge.levelTarget);
                edge.levelAutomated = curve && curve->read;
                for (int c = 0; c < layout.size(); ++c)
                    for (int k = 0; k < node->bus.size(); ++k)
                    {
                        edge.pan[c][k].reset(sampleRate, node->track.surroundXY ? .01 : .005);
                        edge.pan[c][k].setCurrentAndTargetValue(sendPan[c][k]);
                    }
            }
        }
        std::function<int(int)> latency = [&](int index)
        {
            auto &node = *tracks[(size_t)index];
            if (node.outputLatency >= 0)
                return node.outputLatency;
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i]->destination == index || std::any_of(tracks[i]->sends.begin(),
                    tracks[i]->sends.end(), [&](const auto &edge) { return edge.destination == index; }))
                {
                    node.inputLatency = std::max(node.inputLatency, latency((int)i));
                    node.inputDependencies = std::max(node.inputDependencies, tracks[i]->outputDependencies);
                }
            node.outputDependencies = node.inputDependencies + node.rack->dependencies();
            node.outputLatency = node.inputLatency + node.rack->latency();
            trackOrder.push_back(index);
            return node.outputLatency;
        };
        for (size_t i = 0; i < tracks.size(); ++i)
            masterInputLatency = std::max(masterInputLatency, latency((int)i));
        // Only root outputs determine Master alignment; nested intermediate buses are not summed again.
        masterInputLatency = 0;
        for (auto &node : tracks)
            if (node->destination < 0)
                masterInputLatency = std::max(masterInputLatency, node->outputLatency);
        int64_t allocated = 0;
        for (auto &node : tracks)
        {
            int destinationLatency =
                node->destination < 0 ? masterInputLatency : tracks[(size_t)node->destination]->inputLatency;
            node->alignFrames = destinationLatency - node->outputLatency;
            allocated += (int64_t)node->alignFrames * node->destinationBus.size();
            if (allocated > 48 * 1024 * 1024 || destinationLatency > sampleRate * 8)
                throw std::runtime_error("Multitrack compensation exceeds bounded processing budget");
            if (node->alignFrames)
            {
                node->alignment.setSize(node->destinationBus.size(), node->alignFrames);
                node->alignment.clear();
            }
            for (auto &edge : node->sends)
            {
                const auto &destination = *tracks[(size_t)edge.destination];
                edge.delay = destination.inputLatency - node->outputLatency;
                allocated += (int64_t)edge.delay * destination.bus.size();
                if (allocated > 48 * 1024 * 1024 || destination.inputLatency > sampleRate * 8)
                    throw std::runtime_error("FX send compensation exceeds bounded processing budget");
                if (edge.delay)
                {
                    edge.alignment.setSize(destination.bus.size(), edge.delay);
                    edge.alignment.clear();
                }
            }
        }
        updateMixerSolo();
        pipelineLatency = masterInputLatency + masterRack->latency();
        pipelineDependencies = 0;
        for (auto &node : tracks)
            if (node->destination < 0)
                pipelineDependencies = std::max(pipelineDependencies, node->outputDependencies);
        pipelineDependencies += masterRack->dependencies();
    }
    void refreshTracks()
    {
        if (!masterAutomation)
            return;
        auto &mix = document.activeMix();
        masterAutomation = std::make_unique<CompiledAutomation>(mix, "master");
        for (auto &n : tracks)
        {
            n->track = *findTrack(mix, n->track.id);
            n->automation = std::make_unique<CompiledAutomation>(mix, n->track.id);
            auto audible = hasMixerRouting(mix) ? n->audible : trackAudible(mix, n->track.id);
            if (n->audible && !audible)
            {
                n->panic = true;
                if (n->sampler)
                    n->sampler->allNotesOff(true);
            }
            n->audible = audible;
            auto *c = automationCurve(n->track.automation, {{}, "volume"});
            n->volumeAutomated = c && c->read;
            n->gain.setTargetValue(juce::Decibels::decibelsToGain((float)n->track.volumeDb));
            for (size_t i = 0; i < n->sends.size(); ++i)
            {
                auto &edge = n->sends[i];
                edge.level.setTargetValue(juce::Decibels::decibelsToGain((float)n->track.sends[i].levelDb, -90.f));
                const auto *curve = automationCurve(n->track.automation, edge.levelTarget);
                edge.levelAutomated = curve && curve->read;
            }
            n->rack->update(n->track.inserts);
            if (n->sampler && n->track.instrument)
                n->sampler->update(*n->track.instrument);
            if (n->instrument && n->track.instrument)
                n->instrument->update(n->track.instrument->hostedInstrument.hosted);
        }
        updateMixerSolo();
    }
    void renderTracks(int frames, int offset)
    {
        for (auto &node : tracks)
            node->pcm.clear();
        for (int index : trackOrder)
        {
            auto &node = *tracks[(size_t)index];
            if (node.legacyInput)
                node.legacyClips.clear();
            if (node.sampler)
                node.sampler->render(node.pcm, frames, cursor, node.midi, node.nextMidi,
                                     node.automation.get(), inputEnd);
            if (node.instrument)
            {
                bool ending = inputEnd >= 0 && cursor + frames > inputEnd && !node.endedMidi;
                node.instrument->process(
                    node.pcm, frames, &node.midi, cursor, document.activeMix().midiTiming,
                    node.audible && !node.endedMidi, node.panic, inputEnd,
                    node.automation->hostedEvents(node.track.instrument->hostedInstrument.hosted, cursor,
                                                  frames, 0, sampleRate));
                node.panic = false;
                if (ending)
                    node.endedMidi = true;
            }
            if (!node.audible && node.sampler)
                node.sampler->allNotesOff(true);
            for (auto *clip : node.clips)
            {
                clip->automate(*node.automation, cursor);
                clip->add(node.legacyInput ? node.legacyClips : node.pcm, 0, frames, cursor, inputEnd);
            }
            if (node.legacyInput)
                for (int c = 0; c < node.bus.size(); ++c)
                    for (int k = 0; k < 2; ++k)
                        if (node.legacyConversion[c][k])
                            node.pcm.addFrom(c, 0, node.legacyClips, k, 0, frames,
                                             node.legacyConversion[c][k]);
            node.rack->automate(*node.automation, cursor - node.inputLatency);
            node.rack->process(node.pcm, frames, cursor - node.inputLatency);
            auto *motion = squareTouch && squareTouch->owner == node.track.id ? &*squareTouch : nullptr;
            if (motion)
                motion->apply(std::max<int64_t>(0, cursor - node.outputLatency), sampleRate);
            ChannelMatrix matrix{};
            int matrixEnd = 0;
            for (int n = 0; n < frames; ++n)
            {
                const auto frame = std::max<int64_t>(0, cursor + n - node.outputLatency);
                if (n >= matrixEnd)
                {
                    auto time = node.automation->segmentFrame(frame);
                    node.route.spatial = node.track.spatial;
                    node.route.azimuth = node.automation->value({{}, "azimuth"}, node.track.azimuth, time);
                    node.route.width = node.automation->value({{}, "width"}, node.track.width, time);
                    auto pan = node.automation->value({{}, "pan"}, node.track.pan, time);
                    auto position = node.track.surroundXY;
                    if (position)
                    {
                        position->x = node.automation->value(surroundX, position->x, time);
                        position->y = node.automation->value(surroundY, position->y, time);
                        position->spread = node.automation->value(surroundSpread, position->spread, time);
                        if (motion && !motion->points.empty() &&
                            (motion->release < 0 || frame <= motion->returnEnd))
                        {
                            auto applied = motion->value(std::max(time, motion->points.back().frame));
                            position->x = applied.x;
                            position->y = applied.y;
                        }
                        matrix = squareSurroundMatrix(node.bus, node.destinationBus, *position);
                    }
                    else
                        matrix = trackMatrix(node.track, node.destinationBus, pan, node.route.azimuth,
                                             node.route.width);
                    for (int c = 0; c < node.destinationBus.size(); ++c)
                        for (int k = 0; k < node.bus.size(); ++k)
                            node.matrixGains[(size_t)c][(size_t)k].setTargetValue(matrix[c][k]);
                    for (size_t i = 0; i < node.sends.size(); ++i)
                    {
                        auto &edge = node.sends[i];
                        const auto &send = node.track.sends[i];
                        if (!send.roles.empty() || (!send.postFader && !send.followPan))
                            continue;
                        const auto &bus = tracks[(size_t)edge.destination]->bus;
                        auto p = sendMatrix(node.track, send, bus, pan, node.route.azimuth, node.route.width, position);
                        for (int c = 0; c < bus.size(); ++c)
                            for (int k = 0; k < node.bus.size(); ++k)
                                edge.pan[c][k].setTargetValue(p[c][k]);
                    }
                    matrixEnd = n + node.automation->untilBoundary(frame, frames - n);
                    if (motion && motion->release >= 0 && frame < motion->returnEnd)
                        matrixEnd = std::min(matrixEnd, n + (int)(motion->returnEnd - frame));
                }
                float gain = node.volumeAutomated
                                 ? juce::Decibels::decibelsToGain((float)node.automation->value(
                                       {{}, "volume"}, node.track.volumeDb, frame))
                                 : node.gain.getNextValue();
                const float fader = gain;
                if (!node.audible || !node.mainAudible)
                    gain = 0;
                std::array<float, 2> inputGain{};
                std::array<std::array<float, 2>, 6> smoothed;
                std::array<float, 16> stemInputs{};
                if (node.track.surroundXY)
                {
                    for (int k = 0; k < node.bus.size(); ++k)
                    {
                        float power = 0;
                        for (int c = 0; c < node.destinationBus.size(); ++c)
                        {
                            auto value = node.matrixGains[(size_t)c][(size_t)k].getNextValue();
                            smoothed[c][k] = value;
                            power += value * value;
                        }
                        inputGain[k] = node.pcm.getSample(k, n) * gain / std::sqrt(power * node.bus.size());
                    }
                }
                for (int c = 0; c < node.destinationBus.size(); ++c)
                {
                    float value = 0;
                    for (int k = 0; k < node.bus.size(); ++k)
                        if (node.track.surroundXY)
                            value += smoothed[c][k] * inputGain[k];
                        else
                            value += node.matrixGains[(size_t)c][(size_t)k].getNextValue() *
                                     node.pcm.getSample(k, n) * gain;
                    if (node.alignFrames)
                    {
                        auto delayed = node.alignment.getSample(c, node.alignPosition);
                        node.alignment.setSample(c, node.alignPosition, value);
                        value = delayed;
                    }
                    node.meter.measure(c, value);
                    if (node.stem >= 0)
                        stemInputs[c] = value;
                    auto &destination = node.destination < 0 ? block : tracks[(size_t)node.destination]->pcm;
                    destination.addSample(c, n, value);
                }
                if (node.alignFrames)
                    node.alignPosition = (node.alignPosition + 1) % node.alignFrames;
                node.meter.advance(node.destinationBus.size());
                if (node.stem >= 0)
                {
                    auto &tap = stemTaps[(size_t)node.stem];
                    for (int c = 0; c < tap.pcm.getNumChannels(); ++c)
                    {
                        float value = 0;
                        for (int k = 0; k < node.destinationBus.size(); ++k)
                            value += tap.numbering[c][k] * stemInputs[k];
                        if (tap.delay)
                        {
                            const auto delayed = tap.history.getSample(c, tap.index);
                            tap.history.setSample(c, tap.index, value);
                            value = delayed;
                        }
                        tap.pcm.setSample(c, offset + n, value);
                    }
                    if (tap.delay)
                        tap.index = (tap.index + 1) % tap.delay;
                }
                for (size_t i = 0; i < node.sends.size(); ++i)
                {
                    auto &edge = node.sends[i];
                    const auto &send = node.track.sends[i];
                    auto &target = *tracks[(size_t)edge.destination];
                    const bool enabled = node.automation->value(edge.enableTarget, send.enabled ? 1. : 0., frame) >= .5;
                    auto level = edge.level.getNextValue();
                    if (edge.levelAutomated)
                        level = juce::Decibels::decibelsToGain((float)node.automation->value(
                                    edge.levelTarget, send.levelDb, frame), -90.f);
                    level = enabled && node.audible && edge.audible ? level * (send.postFader ? fader : 1.f) : 0.f;
                    const bool square = node.track.surroundXY && (send.postFader || send.followPan) &&
                                        send.roles.empty() && squareSurroundLayout(target.bus);
                    std::array<std::array<float, 2>, 16> pans{};
                    std::array<float, 2> scales{};
                    if (square)
                        for (int k = 0; k < node.bus.size(); ++k)
                        {
                            float power = 0;
                            for (int c = 0; c < target.bus.size(); ++c)
                            {
                                pans[c][k] = edge.pan[c][k].getNextValue();
                                power += pans[c][k] * pans[c][k];
                            }
                            scales[k] = power > 0 ? level / std::sqrt(power * node.bus.size()) : 0.f;
                        }
                    for (int c = 0; c < target.bus.size(); ++c)
                    {
                        float value = 0;
                        for (int k = 0; k < node.bus.size(); ++k)
                            value += node.pcm.getSample(k, n) * (square ? pans[c][k] * scales[k]
                                                                         : edge.pan[c][k].getNextValue() * level);
                        if (edge.delay > 0)
                        {
                            auto delayed = edge.alignment.getSample(c, edge.position);
                            edge.alignment.setSample(c, edge.position, value);
                            value = delayed;
                        }
                        target.pcm.addSample(c, n, value);
                    }
                    if (edge.delay > 0)
                        edge.position = (edge.position + 1) % edge.delay;
                }
            }
        }
    }
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
        if (hasMultiPad(selectedTake(d) ? takeBase.activeMix() : d.activeMix()))
            padBase = selectedTake(d) ? takeBase : audioSourceProjection(d);
        master = document.activeMix().masterDb;
        masterGain.reset(sampleRate, .005);
        masterGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain((float)master));
        bool invariant = d.takeProcessing || selectedTake(d);
        auto &mix = document.activeMix();
        int channels = mix.layout.id.isEmpty() ? 2 : mix.layout.size();
        block.setSize(channels, 512);
        masterMeter.prepare(sampleRate);
        masterRack = std::make_unique<OrderedRack>(
            mix.inserts, mix.layout.id.isEmpty() ? channelLayout("stereo-v1") : mix.layout, sampleRate,
            invariant || mix.trackSchema || !mix.layout.id.isEmpty(), !mix.layout.id.isEmpty(), invariant,
            cancelled, mix.midiTiming);
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
                auto layout = document.activeMix().layout;
                if (document.activeMix().trackSchema && layout.id.isNotEmpty())
                    layout = findTrack(document.activeMix(), l.trackId)->layout;
                voices.push_back(std::make_unique<Voice>(
                    l, sampleRate, invariant || mix.trackSchema || !layout.id.isEmpty(), layout,
                    document.activeMix().routingVersion, invariant, cancelled, mix.midiTiming));
            }
        int longest = 0;
        for (auto &voice : voices)
            longest = std::max(longest, voice->latency());
        for (auto &voice : voices)
            voice->compensate(longest - voice->latency());
        pipelineLatency = longest + masterRack->latency();
        for (auto &voice : voices)
            pipelineDependencies = std::max(pipelineDependencies, voice->dependencies());
        pipelineDependencies += masterRack->dependencies();
        if (document.activeMix().trackSchema)
            prepareTracks(invariant, cancelled);
        // v3 records the producer's audible/source clock after PDC priming.
        // Reapply controls at the same internal graph position, including the
        // frozen pipeline's lookahead. Historical zero-latency stock recipes
        // retain their exact event interpretation.
        // Recipe7 retains the bounded source dependencies used by compensated
        // lookahead/causalized IR conversion during the consumed body. Output
        // duration stays bodyEnd; historical recipes keep their original gate.
        if (auto *t = selectedTake(d);
            t &&
            (t->processing == "advanced-effects-takes-v1" || t->processing == "hosted-automation-takes-v1" ||
             t->processing == "multi-pad-perform-takes-v1" || t->processing == "square-surround-takes-v1" ||
             t->processing == "track-mixer-takes-v1"))
            inputEnd += pipelineDependencies;
        if (auto *t = selectedTake(d);
            t && (t->pluginAware || t->trackRecipe || hasNativeEffects((const DesignMix &)t->recipe)))
            for (auto &event : events)
                event.frame += pipelineLatency;
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
                if (padBase)
                {
                    padBase = takeBase;
                    lastPadControls.reset();
                }
                else
                    applyControlPacket(designParameters(takeBase));
            }
            int n = std::min(512, count - done);
            if (padBase)
            {
                const auto frame = std::max<int64_t>(0, cursor - pipelineLatency);
                auto scheduled = frame - frame % 32;
                for (auto &curve : padBase->activeMix().masterAutomation.curves)
                    if (curve.read && curve.target.instance.startsWith("perform:"))
                        for (auto &point : curve.points)
                            if (point.frame <= frame && point.frame > scheduled)
                                scheduled = point.frame;
                auto evaluated = evaluatePadAutomation(*padBase, scheduled);
                auto controls = mixControls(evaluated.activeMix());
                if (!lastPadControls || *lastPadControls != controls)
                {
                    applyControlPacket(designParameters(evaluated));
                    lastPadControls = controls;
                }
                n = std::min(n, 32 - (int)(frame % 32));
                for (auto &curve : padBase->activeMix().masterAutomation.curves)
                    if (curve.read && curve.target.instance.startsWith("perform:"))
                        for (auto &point : curve.points)
                            if (point.frame > frame)
                                n = (int)std::min<int64_t>(n, point.frame - frame);
            }
            if (nextEvent < events.size())
                n = (int)std::min<int64_t>(n, events[nextEvent].frame - cursor);
            block.clear();
            if (lastVerified < 0 || cursor - lastVerified >= sampleRate)
            {
                for (auto &voice : voices)
                    voice->verify();
                for (auto &node : tracks)
                    if (node->sampler)
                        node->sampler->verify();
                lastVerified = cursor;
            }
            if (masterAutomation)
                renderTracks(n, offset + done);
            else
                for (auto &voice : voices)
                    voice->add(block, 0, n, cursor, inputEnd);
            if (masterAutomation)
                masterRack->automate(*masterAutomation, cursor - masterInputLatency);
            masterRack->process(block, n, cursor - masterInputLatency);
            for (int i = 0; i < n; ++i)
            {
                auto gain = masterGain.getNextValue();
                if (masterAutomation)
                {
                    auto *curve = automationCurve(document.activeMix().masterAutomation, {{}, "volume"});
                    if (curve && curve->read)
                        gain = juce::Decibels::decibelsToGain((float)masterAutomation->value(
                            {{}, "volume"}, document.activeMix().masterDb,
                            std::max<int64_t>(0, cursor + i - pipelineLatency)));
                }
                for (int c = 0; c < block.getNumChannels(); ++c)
                {
                    auto sample = block.getSample(c, i) * gain;
                    block.setSample(c, i, sample);
                    masterMeter.measure(c, sample);
                }
                masterMeter.advance(block.getNumChannels());
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
juce::var DesignGraph::nativeMeters() const
{
    juce::Array<juce::var> rows;
    impl->masterRack->appendMeters(rows);
    for (auto &voice : impl->voices)
        voice->appendMeters(rows);
    for (auto &track : impl->tracks)
        track->rack->appendMeters(rows);
    return rows;
}
juce::var DesignGraph::mixerMeters() const
{
    juce::Array<juce::var> rows;
    rows.add(impl->masterMeter.snapshot("master", designLayout(impl->document)));
    for (const auto &track : impl->tracks)
        rows.add(track->meter.snapshot(track->track.id, track->destinationBus));
    return rows;
}
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
        return;
    impl->receivePadPacket(parameters);
    impl->applyControlPacket(parameters);
}

bool DesignGraph::squareTouch(const juce::var &command)
{
    if (impl->inputEnd >= 0)
        return false;
    auto phase = command["phase"].toString();
    auto pass = command["pass"].toString();
    auto owner = command["owner"].toString();
    auto point = juce::Point<double>{(float)command["x"], (float)command["y"]};
    if (pass.length() != 32 || owner.isEmpty() || !std::isfinite(point.x) || !std::isfinite(point.y) ||
        std::abs(point.x) > 1 || std::abs(point.y) > 1)
        return false;
    if (phase == "begin")
    {
        if (impl->squareTouch && impl->squareTouch->release < 0)
            return false;
        auto *t = findTrack(impl->document.activeMix(), owner);
        if (!t || !t->surroundXY)
            return false;
        auto &motion = impl->squareTouch.emplace();
        motion.id = pass;
        motion.owner = owner;
        motion.generation = command["playGeneration"];
        motion.graphEpoch = command["graphEpoch"];
        motion.underlying = *t;
        motion.points.reserve(squareTouchPointLimit);
        motion.pending = point;
        return true;
    }
    if (!impl->squareTouch || impl->squareTouch->id != pass || impl->squareTouch->owner != owner ||
        impl->squareTouch->release >= 0 || impl->squareTouch->ending)
        return false;
    if (phase == "move")
        impl->squareTouch->pending = point;
    else if (phase == "end")
        impl->squareTouch->ending = true;
    else
        return false;
    return true;
}
juce::var DesignGraph::squareTouchReceipt(bool close)
{
    if (!impl->squareTouch)
        return {};
    auto &motion = *impl->squareTouch;
    if (close && motion.release < 0)
        for (auto &node : impl->tracks)
            if (node->track.id == motion.owner)
                motion.finish(std::max<int64_t>(0, impl->cursor - node->outputLatency), impl->sampleRate,
                              true);
    return motion.result();
}

DesignGraph::~DesignGraph() = default;
void DesignGraph::render(juce::AudioBuffer<float> &out, int offset, int count)
{
    impl->prime(0, [] { return false; });
    impl->render(out, offset, count);
}
void DesignGraph::seek(int64_t frame, const std::function<bool()> &cancelled, int64_t warmup)
{
    // A short effect warmup cannot recreate held MIDI voices, sampler positions or sustain.
    // Prepare this score's complete history on the cancellable graph worker.
    if (hasMidi(impl->document))
        warmup = -1;
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
int64_t DesignGraph::injectMidi(const juce::String &owner, MidiPacket packet)
{
    packet.frame =
        std::clamp<int64_t>(packet.frame, impl->cursor, impl->cursor + (int64_t)(impl->sampleRate * 5));
    packet.flags |= 128;
    for (auto &node : impl->tracks)
        if (node->track.id == owner)
        {
            if (!node->sampler && !node->instrument)
                throw std::runtime_error("Choose an Instrument before MIDI monitoring");
            if (node->midi.size() >= 196608)
                throw std::runtime_error("Live MIDI capacity exceeded");
            auto at = std::upper_bound(node->midi.begin() + node->nextMidi, node->midi.end(), packet.frame,
                                       [](int64_t frame, const auto &e) { return frame < e.frame; });
            node->midi.insert(at, packet);
            return packet.frame;
        }
    throw std::runtime_error("MIDI monitor track missing");
}
void DesignGraph::midiPanic(const juce::String &owner)
{
    for (auto &node : impl->tracks)
        if (owner.isEmpty() || node->track.id == owner)
        {
            if (node->sampler)
                node->sampler->allNotesOff(true);
            node->panic = true;
            auto next = node->midi.begin() + node->nextMidi;
            node->midi.erase(
                std::remove_if(next, node->midi.end(), [](auto &e) { return (e.flags & 128) != 0; }),
                node->midi.end());
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
juce::var DesignReader::nativeMeters() const
{
    return impl->graph.nativeMeters();
}
juce::var DesignReader::mixerMeters() const
{
    return impl->graph.mixerMeters();
}
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
bool DesignReader::squareTouch(const juce::var &command)
{
    return !impl->immutableReplay && impl->graph.squareTouch(command);
}
juce::var DesignReader::squareTouchReceipt(bool close)
{
    return impl->graph.squareTouchReceipt(close);
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
int64_t DesignReader::injectMidi(const juce::String &owner, MidiPacket packet)
{
    return impl->graph.injectMidi(owner, packet);
}
void DesignReader::midiPanic(const juce::String &owner)
{
    impl->graph.midiPanic(owner);
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
    verifyMixImpulses(effectiveInput.activeMix(), cancelled);
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
    if (effectiveInput.activeMix().trackSchema)
        result.processing = "multitrack-automation-v1";
    if (hasMidi(effectiveInput.activeMix()))
    {
        result.processing = "midi-instruments-v1";
        auto p = juce::var(new juce::DynamicObject);
        p.getDynamicObject()->setProperty("midi", midiMixJson(effectiveInput.activeMix()));
        juce::Array<juce::var> instruments;
        for (auto &t : effectiveInput.activeMix().tracks)
            if (t.instrument)
            {
                auto v = juce::var(new juce::DynamicObject);
                v.getDynamicObject()->setProperty("track", t.id);
                v.getDynamicObject()->setProperty("instrument", instrumentJson(*t.instrument));
                instruments.add(v);
            }
        p.getDynamicObject()->setProperty("instruments", instruments);
        result.sourceProvenance = p;
    }
    if (hasNativeEffects(effectiveInput.activeMix()))
        result.processing =
            hasAdvancedEffects(effectiveInput.activeMix()) ? "advanced-effects-v1" : "native-effects-v1";
    if (hasHostedAutomation(effectiveInput.activeMix()))
        result.processing = "hosted-automation-v1";
    if (hasMultiPad(effectiveInput.activeMix()))
        result.processing = "multi-pad-perform-v1";
    if (hasSquareSurround(effectiveInput.activeMix()))
        result.processing = "square-surround-v1";
    if (hostedPass)
    {
        if (result.processing.isEmpty())
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
        if (t->pluginAware)
        {
            auto p = juce::var(new juce::DynamicObject());
            p.getDynamicObject()->setProperty("frozen", takePluginProvenance(*t));
            p.getDynamicObject()->setProperty("authority", "single-float-pass-v1");
            p.getDynamicObject()->setProperty("rawPass", rawPass.getFullPathName());
            p.getDynamicObject()->setProperty("rawSha256", rawPassHash);
            result.pluginProvenance = p;
        }
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
    if (auto *t = selectedTake(d); t && t->pluginAware)
        verifyTakePlugins(*t);
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
        verifyMixImpulses(effectiveInput.activeMix(), cancelled);
        if (!setStage.path.moveFileTo(setTarget))
            throw std::runtime_error("Cannot publish split set atomically");
        setStage.published = true;
        progress(1);
        return result;
    }
    verifyMixImpulses(effectiveInput.activeMix(), cancelled);
    if (!staging.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot publish durable WAV");
    progress(1);
    return result;
}
std::vector<DesignOutput> renderMixerStems(const DesignDocument &d, const juce::File &folder,
    const juce::StringArray &owners, const std::function<bool()> &cancelled,
    const std::function<void(double)> &progress)
{
    validateDesign(d);
    auto input = d;
    auto &mix = input.activeMix();
    if (!mix.trackSchema || selectedTake(d) || owners.isEmpty() || owners.size() > maxDesignTracks)
        throw std::runtime_error("Choose live Audio, MIDI, Group or FX Return stems");
    std::set<juce::String> unique;
    bool masterBoundary = true;
    for (const auto &owner : owners)
    {
        auto *track = findTrack(mix, owner);
        if (!track || track->kind == TrackKind::folder || !unique.insert(owner).second)
            throw std::runtime_error("A stem needs a unique audio owner");
        masterBoundary &= track->output.isEmpty();
        for (auto parent = track->output; parent.isNotEmpty();)
        {
            if (owners.contains(parent))
                throw std::runtime_error("Choose a Group or its children for one stem batch");
            parent = findTrack(mix, parent)->output;
        }
    }
    // Keep the complete upstream graph and Perform controls. Master-only inserts
    // and gain are outside the tap and cannot change stems or delay their origin.
    mix.inserts = {};
    mix.masterDb = 0;
    auto settings = input.renderSettings;
    settings.normalize = settings.splitMono = false;
    auto delivery = exportLayout(input, settings);
    settings.channels = delivery.size();
    settings.layout = delivery.id;
    const auto first = settings.first;
    const auto end = settings.last < 0 ? mixDuration(mix, d.sampleRate) : settings.last;
    const auto projectFrames = end - first + (int64_t)std::llround(settings.tailSeconds * d.sampleRate);
    const auto frames = (int64_t)std::ceil((long double)projectFrames * settings.sampleRate / d.sampleRate);
    if (first < 0 || end <= first || frames < 1 || (long double)frames * settings.channels * (settings.bits / 8) >=
            4293918720.L)
        throw std::runtime_error("Invalid or oversized stem range");
    verifyMixImpulses(mix, cancelled);
    DesignGraph graph(input, cancelled);
    graph.impl->prepareStemTaps(owners, delivery);
    graph.impl->inputEnd = end + graph.impl->pipelineDependencies;
    graph.seek(first, cancelled);

    const auto batch = juce::Uuid().toString();
    if (!folder.createDirectory())
        throw std::runtime_error("Cannot create permanent stem folder");
    auto target = folder.getChildFile("Stems-" + batch);
    struct Stage
    {
        juce::File path;
        bool published = false;
        ~Stage() { if (!published) path.deleteRecursively(); }
    } stage{folder.getChildFile(".stem-stage-" + batch)};
    if (!stage.path.createDirectory())
        throw std::runtime_error("Cannot create stem staging folder");
    const auto order = wavOrder(delivery);
    juce::WavAudioFormat format;
    std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers;
    std::vector<DesignOutput> outputs;
    std::vector<juce::File> files;
    auto stemName = [&](int index)
    {
        const auto &track = *findTrack(mix, owners[index]);
        auto name = juce::File::createLegalFileName(track.name).trim();
        if (name.endsWithIgnoreCase(".wav"))
            name = name.dropLastCharacters(4).trimEnd();
        return juce::String(index + 1).paddedLeft('0', 2) + " " +
               (name.isEmpty() ? juce::String("Stem") : name.substring(0, 80)) + ".wav";
    };
    for (int i = 0; i < owners.size(); ++i)
    {
        const auto filename = stemName(i);
        auto file = stage.path.getChildFile(filename);
        juce::StringPairArray description;
        description.set(juce::WavAudioFormat::bwavDescription, "SoundPlus pre-Master | " + delivery.id);
        auto *writer = format.createWriterFor(file.createOutputStream().release(), settings.sampleRate,
                                              juceChannelSet(delivery), settings.bits, description, 0);
        if (!writer)
            throw std::runtime_error("Cannot open stem WAV writer");
        writers.emplace_back(writer);
        files.push_back(file);
        auto &output = outputs.emplace_back();
        output.kind = "track-stem-pre-master";
        output.processing = "track-mixer-stems-v1";
        output.path = target.getChildFile(filename).getFullPathName();
        output.samples = frames;
        output.revision = d.revision;
        output.variation = d.activeVariation;
        output.created = juce::Time::getCurrentTime().toISO8601(true);
        output.settings = settings;
        auto *channels = new juce::DynamicObject();
        channels->setProperty("version", 1);
        channels->setProperty("design", layoutJson(designLayout(d)));
        channels->setProperty("export", layoutJson(delivery));
        output.channelProvenance = juce::var(channels);
        auto *receipt = new juce::DynamicObject();
        receipt->setProperty("batch", batch);
        receipt->setProperty("owner", owners[i]);
        receipt->setProperty("receipt", target.getChildFile("stems.json").getFullPathName());
        output.sourceProvenance = juce::var(receipt);
    }

    struct Source final : juce::AudioSource
    {
        std::function<void(const juce::AudioSourceChannelInfo &)> read;
        void prepareToPlay(int, double) override {}
        void releaseResources() override {}
        void getNextAudioBlock(const juce::AudioSourceChannelInfo &info) override { read(info); }
    } source;
    juce::AudioBuffer<float> discard(designLayout(input).size(), 512);
    int64_t consumed = 0;
    source.read = [&](const juce::AudioSourceChannelInfo &info)
    {
        for (int offset = 0; offset < info.numSamples;)
        {
            if (cancelled())
                throw QueryCancelled();
            const int count = std::min(512, info.numSamples - offset);
            const auto valid = (int)juce::jlimit<int64_t>(0, count, projectFrames - consumed);
            if (valid)
                graph.render(discard, 0, valid);
            for (size_t i = 0; i < graph.impl->stemTaps.size(); ++i)
                for (int channel = 0; channel < settings.channels; ++channel)
                {
                    const int output = (int)i * settings.channels + channel;
                    const int at = info.startSample + offset;
                    if (valid)
                        info.buffer->copyFrom(output, at, graph.impl->stemTaps[i].pcm,
                                              order[(size_t)channel], 0, valid);
                    info.buffer->clear(output, at + valid, count - valid);
                }
            consumed += valid;
            offset += count;
        }
    };
    const int packedChannels = owners.size() * settings.channels;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    if (settings.sampleRate != d.sampleRate)
    {
        resampler = std::make_unique<juce::ResamplingAudioSource>(&source, false, packedChannels);
        resampler->setResamplingRatio(d.sampleRate / settings.sampleRate);
        resampler->prepareToPlay(512, settings.sampleRate);
    }
    juce::AudioBuffer<float> packed(packedChannels, 512), stem(settings.channels, 512);
    const auto lsb = std::ldexp(1., 1 - settings.bits);
    std::mt19937 dither(170526);
    for (int64_t written = 0; written < frames;)
    {
        const auto count = (int)std::min<int64_t>(512, frames - written);
        juce::AudioSourceChannelInfo request(&packed, 0, count);
        if (resampler)
            resampler->getNextAudioBlock(request);
        else
            source.getNextAudioBlock(request);
        for (size_t i = 0; i < writers.size(); ++i)
        {
            auto &output = outputs[i];
            for (int c = 0; c < settings.channels; ++c)
                for (int n = 0; n < count; ++n)
                {
                    double sample = packed.getSample((int)i * settings.channels + c, n);
                    if (!std::isfinite(sample))
                        throw std::runtime_error("Nonfinite stem audio");
                    output.peak = std::max(output.peak, std::abs(sample));
                    output.clipped += std::abs(sample) > 1;
                    if (settings.bits < 32)
                    {
                        if (settings.dither)
                            sample += ((double)dither() / dither.max() - (double)dither() / dither.max()) * lsb;
                        sample = std::round(sample / lsb) * lsb;
                    }
                    stem.setSample(c, n, (float)juce::jlimit(-1., 1., sample));
                }
            if (!writers[i]->writeFromAudioSampleBuffer(stem, 0, count))
                throw std::runtime_error("Stem write failed");
        }
        written += count;
        progress(.95 * (double)written / (double)frames);
    }
    writers.clear();
    juce::Array<juce::var> members;
    juce::AudioFormatManager readers;
    readers.registerBasicFormats();
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (cancelled())
            throw QueryCancelled();
        syncFile(files[i]);
        std::unique_ptr<juce::AudioFormatReader> reader(readers.createReaderFor(files[i]));
        if (!reader || reader->lengthInSamples != frames || reader->sampleRate != settings.sampleRate ||
            reader->numChannels != (unsigned)settings.channels)
            throw std::runtime_error("Incomplete or inconsistent stem file");
        outputs[i].sha256 = processingHash(files[i]);
        auto *item = new juce::DynamicObject();
        item->setProperty("file", files[i].getFileName());
        item->setProperty("owner", owners[(int)i]);
        item->setProperty("sha256", outputs[i].sha256);
        members.add(juce::var(item));
    }
    auto *receipt = new juce::DynamicObject();
    receipt->setProperty("version", 1);
    receipt->setProperty("tap", masterBoundary ? "pre-master-edges-v1" : "selected-pre-master-taps-v1");
    receipt->setProperty("additivity", masterBoundary && !settings.dither
        ? "Master input before encoding and clipping"
        : "Individual outputs; overlapping sends, downstream processing or encoding can affect sums");
    receipt->setProperty("origin", (juce::int64)first);
    receipt->setProperty("frames", (juce::int64)frames);
    receipt->setProperty("rate", settings.sampleRate);
    receipt->setProperty("bits", settings.bits);
    receipt->setProperty("layout", layoutJson(delivery));
    receipt->setProperty("files", members);
    auto receiptFile = stage.path.getChildFile("stems.json");
    if (!receiptFile.replaceWithText(juce::JSON::toString(juce::var(receipt), true)))
        throw std::runtime_error("Cannot write complete stem receipt");
    syncFile(receiptFile);
    verifyMixImpulses(mix, cancelled);
    for (const auto &layer : mix.layers)
        if (!layer.mute)
            sourceUnchanged(layer.source);
    if (cancelled())
        throw QueryCancelled();
    if (!stage.path.moveFileTo(target))
        throw std::runtime_error("Cannot publish aligned stem batch");
    stage.published = true;
    progress(1);
    return outputs;
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
