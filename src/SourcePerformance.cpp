#include "SourcePerformance.h"
#include "PerformanceState.h"
#include "SourcePools.h"
#include <set>

namespace sp
{
namespace
{
float inGain(int64_t elapsed, int length)
{
    if (length <= 0 || elapsed >= length)
        return 1;
    return std::sin(float(elapsed) / length * juce::MathConstants<float>::halfPi);
}
float outGain(int64_t elapsed, int length)
{
    if (length <= 0 || elapsed >= length)
        return 0;
    return std::cos(float(elapsed) / length * juce::MathConstants<float>::halfPi);
}
} // namespace
bool hasSourcePerformance(const DesignDocument &d)
{
    auto uses = [](auto &m)
    {
        return std::any_of(m.layers.begin(), m.layers.end(),
                           [](auto &l) { return l.pool && (l.pool->live || l.pool->triggerMidi); });
    };
    return uses(d.original) ||
           std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return uses(v.mix); }) ||
           std::any_of(d.takes.begin(), d.takes.end(),
                       [](auto &t) { return !t.sourcePerformance.events.empty(); });
}
std::vector<SourceVoiceBinding> liveSourceBindings(const DesignMix &m)
{
    std::vector<SourceVoiceBinding> result;
    for (int p = 0; p <= int(m.performancePads.size()); ++p)
        for (int c = 0; c < 4; ++c)
        {
            const auto &id = padState(m, p).assignments[size_t(c)].layer;
            auto l =
                std::find_if(m.layers.begin(), m.layers.end(), [&](auto &layer) { return layer.id == id; });
            if (l == m.layers.end() || !l->pool || !l->pool->live)
                continue;
            for (auto &e : l->pool->entries)
                if (e.reviewed && e.enabled)
                {
                    if (result.size() >= maxPreparedPoolEntries)
                        throw std::runtime_error("Prepare at most 64 live pool entries");
                    result.push_back({padIdentity(m, p), id, l->pool->id, c, e});
                }
        }
    return result;
}
juce::var sourceBindingsJson(const std::vector<SourceVoiceBinding> &bindings)
{
    juce::Array<juce::var> a;
    for (auto &b : bindings)
    {
        auto *o = new juce::DynamicObject;
        juce::var row(o);
        o->setProperty("pad", b.pad);
        o->setProperty("corner", b.corner);
        o->setProperty("layer", b.layer);
        o->setProperty("pool", b.pool);
        o->setProperty("entry", poolEntryToJson(b.entry));
        a.add(row);
    }
    return a;
}
juce::var sourceEventsJson(const std::vector<SourceVoiceEvent> &events)
{
    juce::Array<juce::var> a;
    for (auto &e : events)
    {
        auto *o = new juce::DynamicObject;
        juce::var row(o);
        o->setProperty("id", e.id);
        o->setProperty("binding", e.binding);
        o->setProperty("command", e.action == SourceVoiceAction::trigger ? "trigger" : "switch");
        o->setProperty("frame", (juce::int64)e.frame);
        o->setProperty("sequence", (juce::int64)e.sequence);
        o->setProperty("overlap", e.overlap);
        o->setProperty("levelDb", e.levelDb);
        a.add(row);
    }
    return a;
}
SourcePerformance readSourcePerformance(const juce::var &bindings, const juce::var &events)
{
    auto *b = bindings.getArray();
    auto *e = events.getArray();
    if (!b || !e || b->isEmpty() || e->isEmpty() || b->size() > maxPreparedPoolEntries ||
        e->size() > int(maxTakeEvents))
        throw std::runtime_error("Invalid live source binding or event count");
    SourcePerformance result;
    for (auto &row : *b)
        result.bindings.push_back({row["pad"].toString(), row["layer"].toString(), row["pool"].toString(),
                                   (int)row["corner"], poolEntryFromJson(row["entry"])});
    for (auto &row : *e)
    {
        auto action = row["command"].toString();
        if (action != "trigger" && action != "switch")
            throw std::runtime_error("Unsupported source voice command");
        result.events.push_back(
            {row["id"].toString(), (int)row["binding"],
             action == "trigger" ? SourceVoiceAction::trigger : SourceVoiceAction::switchSource,
             (int64_t)row["frame"], (uint64_t)(int64_t)row["sequence"], (int)row["overlap"],
             (double)row["levelDb"]});
    }
    return result;
}
void validateSourcePerformance(const PerformTake &t)
{
    auto &s = t.sourcePerformance;
    if (s.events.empty() && s.bindings.empty())
        return;
    if (s.bindings.empty() || s.bindings.size() > maxPreparedPoolEntries || s.events.empty() ||
        s.events.size() + t.events.size() > maxTakeEvents)
        throw std::runtime_error("Invalid source performance capacity");
    auto &m = static_cast<const DesignMix &>(t.recipe);
    std::set<std::pair<juce::String, juce::String>> keys;
    for (auto &b : s.bindings)
    {
        auto &e = b.entry;
        if (b.corner < 0 || b.corner >= 4 || b.layer.isEmpty() || b.pool.isEmpty() || !e.enabled ||
            !e.reviewed || e.fingerprint.length() != 64 || !e.fingerprint.containsOnly("0123456789abcdef") ||
            !keys.emplace(b.layer, e.id).second)
            throw std::runtime_error("Invalid frozen source identity or digest");
        bool found = false;
        for (int p = 0; p <= int(m.performancePads.size()); ++p)
            if (padIdentity(m, p) == b.pad && padState(m, p).assignments[size_t(b.corner)].layer == b.layer)
                found = true;
        if (!found)
            throw std::runtime_error("Source voice no longer belongs to its frozen corner");
    }
    std::set<juce::String> ids;
    std::set<int> used;
    int64_t last = -1;
    uint64_t seq = 0;
    for (auto &e : s.events)
    {
        if (e.id.length() != 32 || !ids.insert(e.id).second || e.binding < 0 ||
            size_t(e.binding) >= s.bindings.size() || e.frame < 0 || e.frame < last || e.frame >= t.bodyEnd ||
            e.sequence != ++seq || !std::isfinite(e.levelDb) || e.levelDb < -60 || e.levelDb > 6 ||
            e.overlap < 0 || e.overlap > int(t.sampleRate * 2) ||
            (e.action == SourceVoiceAction::trigger && e.overlap != 0))
            throw std::runtime_error("Invalid frozen source event or ordering");
        last = e.frame;
        used.insert(e.binding);
    }
    if (used.size() != s.bindings.size())
        throw std::runtime_error("Unreferenced frozen source binding");
}

bool PoolVoices::schedule(size_t sound, const SourceVoiceEvent &e)
{
    if (sound >= sounds.size() || count == queue.size() ||
        (count && e.frame < queue[(read + count - 1) % queue.size()].frame))
        return false;
    queue[(read + count++) % queue.size()] = {(int)sound, e.action, e.frame, e.overlap,
                                              juce::Decibels::decibelsToGain(float(e.levelDb))};
    return true;
}
float PoolVoices::envelope(const Playing &p, int64_t frame)
{
    if (p.release >= 0)
        return p.releaseGain * outGain(frame - p.release, p.decay);
    return inGain(frame - p.first, p.attack);
}
void PoolVoices::start(const Event &e)
{
    const bool sustain = e.action == SourceVoiceAction::switchSource;
    if (sustain)
    {
        if (baseRelease < 0)
        {
            baseRelease = e.frame;
            baseFade = e.overlap;
        }
        for (auto &p : playing)
            if (p.sound >= 0 && p.sustained && p.release < 0)
            {
                p.releaseGain = envelope(p, e.frame);
                p.release = e.frame;
                p.decay = e.overlap;
            }
    }
    auto at = std::find_if(playing.begin(), playing.end(), [](auto &p) { return p.sound < 0; });
    if (at == playing.end())
    {
        at = std::min_element(playing.begin(), playing.end(),
                              [](auto &a, auto &b)
                              {
                                  return std::pair(a.sustained && a.release < 0, a.order) <
                                         std::pair(b.sustained && b.release < 0, b.order);
                              });
        ++stolen;
    }
    *at = {e.sound, e.frame, -1, sustain ? e.overlap : 0, 0, e.level, 1, sustain, ++order};
}
void PoolVoices::mix(juce::AudioBuffer<float> &base, int frames, int64_t frame, float gain, int64_t sourceEnd)
{
    if (count == 0 && order == 0)
        return;
    for (int n = 0; n < frames; ++n)
    {
        const auto t = frame + n;
        while (count && queue[read].frame <= t)
        {
            start(queue[read]);
            read = (read + 1) % queue.size();
            --count;
        }
        if (baseRelease >= 0)
        {
            float g = outGain(t - baseRelease, baseFade);
            for (int c = 0; c < base.getNumChannels(); ++c)
                base.setSample(c, n, base.getSample(c, n) * g);
        }
        if (sourceEnd >= 0 && t >= sourceEnd)
            continue;
        for (auto &p : playing)
            if (p.sound >= 0)
            {
                auto &pcm = sounds[size_t(p.sound)].pcm;
                const auto local = t - p.first;
                if (local < 0)
                    continue;
                if (local >= pcm.getNumSamples() || (p.release >= 0 && t - p.release >= p.decay))
                {
                    p.sound = -1;
                    continue;
                }
                const float g = envelope(p, t) * gain * p.level;
                for (int c = 0; c < base.getNumChannels(); ++c)
                    base.addSample(c, n, pcm.getSample(c, (int)local) * g);
            }
    }
}
} // namespace sp
