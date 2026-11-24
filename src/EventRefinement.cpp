#include "EventRefinement.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>

namespace sp
{
namespace
{
struct Frame
{
    int64_t first, last;
    float rms, peak, novelty;
};
struct Features
{
    std::vector<Frame> frames;
    juce::String key;
    double rate;
};
void verify(const Asset &a)
{
    FileStamp stamp;
    if (!a.online || !fileStamp(juce::File(a.path), stamp))
        throw std::runtime_error("Recording is offline");
    if (!a.deliverable() || a.size != stamp.size || a.modified != stamp.modified || a.inode != stamp.inode)
        throw std::runtime_error("Recording changed. Refresh its Library source.");
}
juce::String digest(const juce::String &s)
{
    return juce::SHA256(s.toRawUTF8(), s.getNumBytesAsUTF8()).toHexString();
}
Features extract(const EventWindow &w, const std::function<bool()> &cancel, bool fingerprintOnly)
{
    auto &a = w.source;
    verify(a);
    if (w.range.isEmpty() || w.range.getStart() < 0 || w.range.getEnd() > a.samples ||
        a.sampleRate < 8000 || a.sampleRate > 192000 || w.range.getLength() > a.sampleRate * 32 ||
        a.channels < 1 || a.channels > 16)
        throw std::runtime_error("Choose a window up to 32 seconds in a supported source");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(a.path)));
    if (!reader || reader->lengthInSamples != a.samples || reader->sampleRate != a.sampleRate ||
        (int)reader->numChannels != a.channels)
        throw std::runtime_error("Source header changed or unreadable");
    int lo = w.pair < 0 ? 0 : w.pair * 2, hi = w.pair < 0 ? a.channels : std::min(lo + 2, a.channels);
    if (lo >= hi)
        throw std::runtime_error("Selected channels are unavailable");
    const int hop = (int)std::lround(a.sampleRate * .005);
    const int size = juce::nextPowerOfTwo(hop);
    juce::dsp::FFT fft(juce::jlimit(0, 12, (int)std::log2(size)));
    std::vector<float> spectrum(size * 2), magnitudes(size / 2), prior(size / 2);
    juce::AudioBuffer<float> audio(a.channels, hop * 16);
    Features features{{}, {}, a.sampleRate};
    juce::MemoryOutputStream hashes;
    for (auto block = w.range.getStart(); block < w.range.getEnd(); block += audio.getNumSamples())
    {
        if (cancel()) throw QueryCancelled();
        int n = (int)std::min<int64_t>(audio.getNumSamples(), w.range.getEnd() - block);
        if (!reader->read(&audio, 0, n, block, true, true))
            throw std::runtime_error("Cannot decode this window");
        for (int ch = lo; ch < hi; ++ch)
        {
            auto h = juce::SHA256(audio.getReadPointer(ch), (size_t)n * sizeof(float)).getRawData();
            hashes.write(h.getData(), h.getSize());
        }
        if (fingerprintOnly) continue;
        for (int offset = 0; offset < n; offset += hop)
        {
            int count = std::min(hop, n - offset), onset = count, ending = -1;
            float energy = 0, peak = 0;
            std::fill(magnitudes.begin(), magnitudes.end(), 0.f);
            for (int ch = lo; ch < hi; ++ch)
            {
                auto p = audio.getReadPointer(ch, offset);
                float power = 0;
                std::fill(spectrum.begin(), spectrum.end(), 0.f);
                for (int i = 0; i < count; ++i)
                {
                    float v = std::isfinite(p[i]) ? p[i] : 0.f;
                    power += v * v;
                    peak = std::max(peak, std::abs(v));
                    if (std::abs(v) >= .0003f) { onset = std::min(i, onset); ending = std::max(i, ending); }
                    spectrum[i] = v * (.5f - .5f * std::cos(juce::MathConstants<float>::twoPi * i / hop));
                }
                energy = std::max(energy, power / count);
                fft.performFrequencyOnlyForwardTransform(spectrum.data(), true);
                for (int k = 0; k < size / 2; ++k)
                    magnitudes[k] = std::max(magnitudes[k], std::log1p(spectrum[k]));
            }
            float novelty = 0;
            for (int k = 0; k < size / 2; ++k)
                novelty += std::max(0.f, magnitudes[k] - prior[k]) * (k * a.sampleRate / size > 1800 ? 1.5f : 1.f);
            prior = magnitudes;
            features.frames.push_back({block + offset + onset, block + offset + ending + 1,
                                       std::sqrt(energy), peak, novelty / size});
        }
    }
    verify(a);
    features.key = digest("event-v1/hop5/max-spectrum/" + a.id + "/" + juce::String(a.revision) + "/" +
        juce::String(a.modified) + "/" + juce::String(a.inode) + "/" + juce::String(a.sampleRate) + "/" +
        juce::String(w.pair) + "/" + juce::String(w.range.getStart()) + "/" + juce::String(w.range.getEnd()) +
        "/" + juce::SHA256(hashes.getData(), hashes.getDataSize()).toHexString());
    return features;
}
struct Proposal
{
    SampleSpan span;
    float score;
    std::array<float, 16> shape{};
};
std::vector<Proposal> detect(const Features &f)
{
    const auto &frames = f.frames;
    std::vector<float> levels;
    for (auto &v : frames) levels.push_back(v.rms);
    auto q = levels.begin() + levels.size() / 4;
    std::nth_element(levels.begin(), q, levels.end());
    float floor = std::max(.0004f, *q * 2.5f);
    std::vector<Proposal> candidates;
    for (size_t i = 0; i < frames.size();)
    {
        auto &v = frames[i];
        if (v.peak < .005f || (v.rms < floor * 1.5f && v.peak < floor * 8)) { ++i; continue; }
        size_t start = i, end = i, j = i + 1;
        for (; j < frames.size() && j <= end + 3; ++j)
            if (frames[j].rms >= floor || frames[j].peak >= floor * 6) end = j;
        i = j;
        if (end > start + 600 || (end - start + 1) * 10 >= frames.size() * 9) continue;
        if (frames[end].last <= v.first) continue;
        float novelty = 0, maxRms = 0;
        Proposal p{{v.first, frames[end].last}, 0};
        for (auto k = start; k <= end; ++k)
        {
            auto bin = (k - start) * p.shape.size() / (end - start + 1);
            p.shape[bin] = std::max(p.shape[bin], frames[k].rms);
            maxRms = std::max(maxRms, frames[k].rms);
            novelty = std::max(novelty, frames[k].novelty);
        }
        for (auto &x : p.shape) x /= std::max(.0001f, maxRms);
        p.score = std::log1p(maxRms / floor) + novelty;
        candidates.push_back(p);
    }
    return candidates;
}
// Bounded dynamic time warping of amplitude envelopes, independent of the source rates.
float distance(const Proposal &a, const Proposal &b)
{
    std::array<float, 17> previous{}, next{};
    previous.fill(1000.f);
    previous[0] = 0;
    for (int i = 1; i <= 16; ++i)
    {
        next.fill(1000.f);
        for (int j = std::max(1, i - 3); j <= std::min(16, i + 3); ++j)
            next[j] = std::abs(a.shape[i-1] - b.shape[j-1]) + std::min({previous[j], previous[j-1], next[j-1]});
        previous = next;
    }
    return previous[16] / 16;
}
}
EventSuggestions refineEvents(const EventWindow &w, const std::optional<EventWindow> &ref,
                             const juce::File &directory, const std::function<bool()> &cancel)
{
    auto signature = extract(w, cancel, true).key;
    bool useRef = ref && !ref->range.isEmpty() && ref->range.getLength() <= ref->source.sampleRate * 32;
    auto referenceKey = useRef ? extract(*ref, cancel, true).key : juce::String();
    if (cancel()) throw QueryCancelled();
    auto key = digest(signature + "/" + referenceKey);
    auto file = directory.getChildFile(key + ".events");
    EventSuggestions result{w.range, {}, {}};
    auto unpack = [&](juce::String data)
    {
        auto lines = juce::StringArray::fromLines(data.trimEnd());
        if (lines.size() < 2 || lines.size() > 10 || lines[0] != digest(key + lines[1])) return false;
        auto spans = juce::StringArray::fromTokens(lines[1], ",", "");
        if (spans.size() > 8) return false;
        for (auto s : spans)
        {
            if (s == "full") continue;
            SampleSpan e{s.upToFirstOccurrenceOf("-", false, false).getLargeIntValue(),
                         s.fromFirstOccurrenceOf("-", false, false).getLargeIntValue()};
            if (e.isEmpty() || !w.range.contains(e)) return false;
            result.events.push_back(e);
        }
        return true;
    };
    if (file.existsAsFile() && file.getSize() < 2048 && unpack(file.loadFileAsString()))
        result.cacheHit = true;
    else
    {
        result.events.clear();
        auto f = extract(w, cancel, false);
        if (f.key != signature) throw std::runtime_error("Source changed during refinement");
        std::optional<Proposal> guide;
        if (useRef)
        {
            auto r = extract(*ref, cancel, false);
            if (r.key != referenceKey) throw std::runtime_error("Reference changed during refinement");
            auto guides = detect(r);
            if (guides.size() == 1) guide = guides.front();
        }
        auto events = detect(f);
        if (guide) for (auto &p : events)
            p.score = -distance(p, *guide) - .2f * std::abs(std::log(
                (p.span.getLength() / f.rate) / (guide->span.getLength() / ref->source.sampleRate)));
        std::stable_sort(events.begin(), events.end(), [](auto &a, auto &b) { return a.score > b.score; });
        juce::StringArray spans;
        for (size_t i = 0; i < std::min<size_t>(8, events.size()); ++i)
        {
            auto s = events[i].span;
            result.events.push_back(s);
            spans.add(juce::String(s.getStart()) + "-" + juce::String(s.getEnd()));
        }
        if (cancel()) throw QueryCancelled();
        if (directory.createDirectory())
        {
            auto body = spans.isEmpty() ? juce::String("full") : spans.joinIntoString(",");
            file.replaceWithText(digest(key + body) + "\n" + body);
            auto entries = directory.findChildFiles(juce::File::findFiles, false, "*.events");
            std::sort(entries.begin(), entries.end(), [](auto a, auto b) { return a.getLastModificationTime() < b.getLastModificationTime(); });
            for (int i = 0; i < entries.size() - 128; ++i) entries[i].deleteFile();
        }
    }
    verify(w.source);
    if (cancel()) throw QueryCancelled();
    result.reason = result.events.empty() ? "No distinct event. Keep the full match."
                  : result.events.size() > 1 ? "Several possible events" : "Suggested transient";
    return result;
}
}
