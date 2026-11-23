#include "AdvancedIR.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <CommonCrypto/CommonDigest.h>
#include <numeric>
#include <set>
#include <cmath>
namespace sp
{
namespace
{
void cancel(const IRCancel &f)
{
    if (f())
        throw std::runtime_error("Impulse preparation cancelled");
}
bool same(const FileStamp &a, const FileStamp &b)
{
    return a.size == b.size && a.modified == b.modified && a.inode == b.inode;
}
juce::String digest(const juce::File &file, const IRCancel &cancelled)
{
    auto in = file.createInputStream();
    if (!in)
        throw std::runtime_error("IR offline: select or relink " + file.getFileName().toStdString());
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::array<char, 65536> bytes;
    while (!in->isExhausted())
    {
        cancel(cancelled);
        auto n = in->read(bytes.data(), (int)bytes.size());
        if (n <= 0)
            throw std::runtime_error("Cannot read complete IR");
        CC_SHA256_Update(&context, bytes.data(), (CC_LONG)n);
    }
    unsigned char result[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(result, &context);
    return juce::String::toHexString(result, sizeof(result), 0);
}
void unchanged(const ImpulseReference &r)
{
    FileStamp current;
    auto &s = r.source;
    if (!fileStamp(juce::File(s.path), current))
        throw std::runtime_error("IR offline: " + s.name.toStdString() + ". Relink an identical copy.");
    if (current.size != s.size || current.modified != s.modified || current.inode != s.inode)
        throw std::runtime_error("IR changed: " + s.name.toStdString() +
                                 ". Choose IR deliberately, or relink identical content.");
}
double sinc(double x)
{
    return std::abs(x) < 1.e-12
               ? 1
               : std::sin(juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
}
} // namespace
void validateImpulseBus(const ImpulseReference &r, const ChannelLayout &bus)
{
    validateImpulse(r);
    validateLayout(bus);
    std::set<juce::String> covered;
    for (auto &route : r.routes)
    {
        if (!bus.roles.contains(route.input) || !bus.roles.contains(route.output))
            throw std::runtime_error(
                "IR role map does not match this bus; choose an explicit compatible IR map");
        covered.insert(route.output);
    }
    if (covered.size() != (size_t)bus.size())
        throw std::runtime_error("IR map must cover every declared output role");
    if ((r.mapping == "stereo-diagonal" || r.mapping == "true-stereo") && bus != channelLayout("stereo-v1"))
        throw std::runtime_error("Stereo/true-stereo IR requires exact L/R Stereo bus");
}
void verifyImpulseFile(const ImpulseReference &r, const IRCancel &cancelled)
{
    validateImpulse(r);
    cancel(cancelled);
    unchanged(r);
    if (digest(juce::File(r.source.path), cancelled) != r.sha256)
        throw std::runtime_error("IR content hash changed: " + r.source.name.toStdString());
    unchanged(r);
    cancel(cancelled);
}
void verifyMixImpulses(const DesignMix &mix, const IRCancel &cancelled)
{
    for (auto *effect : advancedInserts(mix))
        if (effect->type == "convolution-reverb")
        {
            if (!effect->impulse)
                throw std::runtime_error("Convolution Reverb is unresolved: choose an impulse response");
            verifyImpulseFile(*effect->impulse, cancelled);
        }
}
ImpulseReference inspectImpulse(const juce::File &file, const ChannelLayout &bus, const juce::String &mapping,
                                const IRCancel &cancelled)
{
    cancel(cancelled);
    FileStamp before, after;
    if (!fileStamp(file, before))
        throw std::runtime_error("IR offline: choose an existing local audio file");
    ImpulseReference r;
    r.source = inspectDesignSource(file);
    r.source.id = "ir-" + juce::Uuid().toString();
    r.mapping = mapping;
    if (mapping == "mono-diagonal" || mapping == "role-diagonal")
    {
        auto physical = wavOrder(bus);
        if (mapping == "role-diagonal" &&
            (r.source.layoutConfidence == "container-mask" ||
             r.source.layoutConfidence == "producer-declaration") &&
            r.source.detectedChannels != bus)
            throw std::runtime_error(
                "IR speaker mask disagrees with this named bus (Side/Rear are distinct)");
        for (int i = 0; i < bus.size(); ++i)
        {
            auto channel = mapping == "mono-diagonal"
                               ? 0
                               : (int)(std::find(physical.begin(), physical.end(), i) - physical.begin());
            r.routes.push_back({bus.roles[i], bus.roles[i], channel});
        }
    }
    else if (mapping == "stereo-diagonal")
        r.routes = {{"L", "L", 0}, {"R", "R", 1}};
    else if (mapping == "true-stereo")
        r.routes = {{"L", "L", 0}, {"L", "R", 1}, {"R", "L", 2}, {"R", "R", 3}};
    else
        throw std::runtime_error("Choose an explicit supported IR map");
    // Metadata bounds are checked before hashing or decoding the whole file.
    r.sha256 = juce::String::repeatedString("0", 64);
    validateImpulseBus(r, bus);
    r.sha256 = digest(file, cancelled);
    if (!fileStamp(file, after) || !same(before, after))
        throw std::runtime_error("IR changed while being inspected");
    verifyImpulseFile(r, cancelled);
    return r;
}
ImpulseReference relinkImpulse(const ImpulseReference &original, const juce::File &file,
                               const ChannelLayout &bus, const IRCancel &cancelled)
{
    auto replacement = inspectImpulse(file, bus, original.mapping, cancelled);
    if (replacement.sha256 != original.sha256 ||
        replacement.source.sampleRate != original.source.sampleRate ||
        replacement.source.samples != original.source.samples ||
        replacement.source.channels != original.source.channels)
        throw std::runtime_error(
            "Relink requires identical IR content; use Choose IR for a deliberate replacement");
    replacement.source.id = original.source.id;
    replacement.routes = original.routes;
    validateImpulseBus(replacement, bus);
    return replacement;
}
PreparedImpulse prepareImpulse(const ImpulseReference &r, const ChannelLayout &bus, double rate,
                               const IRCancel &cancelled)
{
    if (!std::isfinite(rate) || rate < 8000 || rate > 192000 || rate != std::round(rate))
        throw std::runtime_error("Unsupported IR target rate");
    validateImpulseBus(r, bus);
    verifyImpulseFile(r, cancelled);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(r.source.path)));
    if (!reader || reader->sampleRate != r.source.sampleRate || reader->lengthInSamples != r.source.samples ||
        (int)reader->numChannels != r.source.channels)
        throw std::runtime_error("IR header changed");
    juce::AudioBuffer<float> original(r.source.channels, (int)r.source.samples);
    if (!reader->read(&original, 0, original.getNumSamples(), 0, true, true))
        throw std::runtime_error("Incomplete IR decode");
    for (int c = 0; c < original.getNumChannels(); ++c)
        for (int n = 0; n < original.getNumSamples(); ++n)
            if (!std::isfinite(original.getSample(c, n)))
                throw std::runtime_error("IR contains nonfinite samples");
    PreparedImpulse result;
    if (rate == r.source.sampleRate)
        result.pcm = std::move(original);
    else
    {
        if (!std::isfinite(rate) || rate < 8000 || rate > 192000 || rate != std::round(rate))
            throw std::runtime_error("Unsupported IR target rate");
        int sourceRate = (int)r.source.sampleRate, targetRate = (int)rate,
            gcd = std::gcd(sourceRate, targetRate), phases = targetRate / gcd;
        result.resampleLatency = (int)std::ceil(32 * rate / r.source.sampleRate);
        int frames =
            result.resampleLatency + (int)std::ceil((r.source.samples + 32) * rate / r.source.sampleRate);
        result.pcm.setSize(r.source.channels, frames);
        double cutoff = std::min(1., rate / r.source.sampleRate), scale = r.source.sampleRate / rate;
        auto coefficients = [&](double fraction)
        {
            std::array<double, 64> weights{};
            double sum = 0;
            for (int j = 0; j < 64; ++j)
            {
                double delta = fraction - (j - 31);
                weights[(size_t)j] =
                    std::abs(delta) < 32 ? cutoff * sinc(cutoff * delta) * sinc(delta / 32) : 0;
                sum += weights[(size_t)j];
            }
            for (auto &w : weights)
                w *= scale / sum;
            return weights;
        };
        std::vector<std::array<double, 64>> table;
        if (phases <= 4096)
        {
            table.reserve((size_t)phases);
            for (int i = 0; i < phases; ++i)
                table.push_back(coefficients((double)i / phases));
        }
        for (int n = 0; n < frames; ++n)
        {
            if (n % 1024 == 0)
                cancel(cancelled);
            int64_t numerator = (int64_t)(n - result.resampleLatency) * sourceRate;
            int64_t whole = numerator / targetRate, remainder = numerator % targetRate;
            if (remainder < 0)
            {
                remainder += targetRate;
                --whole;
            }
            auto weights = table.empty() ? coefficients((double)remainder / targetRate)
                                         : table[(size_t)(remainder / gcd)];
            for (int c = 0; c < r.source.channels; ++c)
            {
                double value = 0;
                for (int j = 0; j < 64; ++j)
                {
                    auto at = whole + j - 31;
                    if (at >= 0 && at < r.source.samples)
                        value += original.getSample(c, (int)at) * weights[(size_t)j];
                }
                result.pcm.setSample(c, n, (float)value);
            }
        }
    }
    verifyImpulseFile(r, cancelled);
    return result;
}
} // namespace sp
