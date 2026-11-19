#include "WaveformPreview.h"
#include <cmath>
#include <set>
namespace sp
{
juce::String waveformSourceKey(const Asset &a)
{
    return a.id + ":" + juce::String(a.revision) + ":" + juce::String(a.size) + ":" +
           juce::String(a.modified) + ":" + juce::String(a.inode) + ":" + a.path;
}
PeakColumns compactWaveformColumns(const WaveformData &d, int64_t first, int64_t last, bool reverse,
                                   int width)
{
    if (d.peaks.empty() || d.samples < 1 || width < 1)
        return {};
    first = std::clamp<int64_t>(first, 0, d.samples - 1);
    last = std::clamp<int64_t>(last, first + 1, d.samples);
    PeakColumns columns((size_t)width, {0, 0});
    const auto bins = (double)d.peaks.size();
    for (int x = 0; x < width; ++x)
    {
        int column = reverse ? width - 1 - x : x;
        const auto a = first + (long double)(last - first) * column / width;
        const auto b = first + (long double)(last - first) * (column + 1) / width;
        const auto begin =
            std::clamp<int64_t>((int64_t)std::floor(a * bins / d.samples), 0, (int64_t)d.peaks.size() - 1);
        const auto end =
            std::clamp<int64_t>((int64_t)std::ceil(b * bins / d.samples), begin + 1, (int64_t)d.peaks.size());
        for (auto i = begin; i < end; ++i)
        {
            columns[(size_t)x].first = std::min(columns[(size_t)x].first, d.peaks[(size_t)i].first);
            columns[(size_t)x].second = std::max(columns[(size_t)x].second, d.peaks[(size_t)i].second);
        }
    }
    return columns;
}
void paintCompactWaveform(juce::Graphics &g, juce::Rectangle<float> b, const PeakColumns &columns,
                          juce::Colour colour, std::optional<double> playhead)
{
    if (columns.empty() || b.isEmpty())
        return;
    juce::Graphics::ScopedSaveState scope(g);
    g.reduceClipRegion(b.toNearestInt());
    const auto centre = b.getCentreY(), scale = b.getHeight() * .46f;
    g.setColour(colour);
    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto [lo, hi] = columns[i];
        const auto x = b.getX() + (float)i * b.getWidth() / columns.size();
        g.drawLine(x, centre - std::clamp(hi, -1.f, 1.f) * scale, x,
                   centre - std::clamp(lo, -1.f, 1.f) * scale + .7f, 1.f);
    }
    if (playhead && *playhead >= 0 && *playhead <= 1)
    {
        g.setColour(ui::playhead);
        const auto x = b.getX() + (float)*playhead * b.getWidth();
        g.drawLine(x, b.getY(), x, b.getBottom(), 1.f);
    }
}
WaveformPool::WaveformPool(const juce::File &directory, std::function<void()> beforeRead,
                           std::function<double()> clockSource)
    : decodes(std::make_shared<std::atomic<uint64_t>>(0)),
      now(clockSource ? std::move(clockSource) : [] { return juce::Time::getMillisecondCounterHiRes(); }),
      worker(directory,
             [counter = decodes, beforeRead]
             {
                 ++*counter;
                 if (beforeRead)
                     beforeRead();
             })
{
    startedMs = now();
}
void WaveformPool::demand(int owner, const std::vector<Asset> &assets)
{
    auto &wanted = owners[owner];
    wanted.clear();
    std::set<juce::String> keys;
    for (auto &a : assets)
        if (a.samples > 0 && a.id.isNotEmpty() && keys.insert(waveformSourceKey(a)).second)
        {
            wanted.push_back(a);
            if (wanted.size() == maxDemand)
                break;
        }
}
bool WaveformPool::tick()
{
    bool changed = false;
    if (auto result = worker.snapshot(); !pending.isEmpty() && result && result->generation == token)
    {
        completed[pending] = {result, ++clock, now()};
        diskHits += result->cacheHit;
        pending.clear();
        ++changedGeneration;
        if (firstReadyMs < 0)
            firstReadyMs = now() - startedMs;
        if (loadMs.size() < 512)
            loadMs.push_back(now() - submittedMs);
        changed = true;
    }
    // Only currently demanded sources are candidates. No growing job queue and
    // no concurrent calls to the single-pending WaveformCache request API.
    if (pending.isEmpty())
    {
        bool started = false;
        const Asset *validate = nullptr;
        size_t budget = 0;
        std::set<juce::String> seen;
        for (auto &[owner, assets] : owners)
        {
            juce::ignoreUnused(owner);
            for (auto &a : assets)
            {
                auto key = waveformSourceKey(a);
                if (!seen.insert(key).second)
                    continue;
                if (++budget > maxSources)
                    break;
                if (auto found = completed.find(key); found != completed.end())
                {
                    found->second.used = ++clock;
                    if (!validate && now() - found->second.validated >= 5000)
                        validate = &a;
                }
                else
                {
                    pending = key;
                    token = worker.request(a);
                    submittedMs = now();
                    ++requests;
                    started = true;
                    break;
                }
            }
            if (started || budget >= maxSources)
                break;
        }
        if (!started && validate)
        {
            pending = waveformSourceKey(*validate);
            token = worker.request(*validate);
            submittedMs = now();
            ++requests;
        }
    }
    while (completed.size() > maxSources)
    {
        auto oldest = std::min_element(completed.begin(), completed.end(),
                                       [](auto &a, auto &b) { return a.second.used < b.second.used; });
        completed.erase(oldest);
    }
    return changed;
}
std::shared_ptr<const WaveformData> WaveformPool::get(const Asset &a)
{
    if (auto it = completed.find(waveformSourceKey(a)); it != completed.end())
    {
        it->second.used = ++clock;
        ++memoryHits;
        return it->second.data;
    }
    return {};
}
std::shared_ptr<const PeakColumns> WaveformPool::columns(const Asset &a, int64_t first, int64_t last,
                                                         bool reverse, int width)
{
    width = std::clamp(width, 1, 2048);
    const auto key = waveformSourceKey(a) + ":" + juce::String(first) + ":" + juce::String(last) +
                     (reverse ? ":r:" : ":f:") + juce::String(width);
    if (auto found = geometries.find(key); found != geometries.end())
    {
        found->second.used = ++clock;
        return found->second.data;
    }
    auto source = get(a);
    if (!source || source->peaks.empty())
        return {};
    auto result = std::make_shared<PeakColumns>(compactWaveformColumns(*source, first, last, reverse, width));
    geometries[key] = {result, ++clock};
    while (geometries.size() > maxGeometries)
    {
        auto oldest = std::min_element(geometries.begin(), geometries.end(),
                                       [](auto &a, auto &b) { return a.second.used < b.second.used; });
        geometries.erase(oldest);
    }
    return result;
}
void WaveformPool::retry(const Asset &a)
{
    ++changedGeneration;
    const auto key = waveformSourceKey(a);
    completed.erase(key);
    std::erase_if(geometries, [&](auto &entry) { return entry.first.startsWith(key + ":"); });
}
juce::var WaveformPool::metrics() const
{
    auto *o = new juce::DynamicObject();
    size_t bytes = 0;
    for (auto &[key, entry] : completed)
    {
        juce::ignoreUnused(key);
        bytes += entry.data->peaks.size() * sizeof(std::pair<float, float>);
    }
    o->setProperty("requests", (juce::int64)requests);
    o->setProperty("decodes", (juce::int64)decodes->load());
    o->setProperty("diskHits", (juce::int64)diskHits);
    o->setProperty("memoryHits", (juce::int64)memoryHits);
    o->setProperty("retainedSources", (int)completed.size());
    o->setProperty("retainedGeometries", (int)geometries.size());
    o->setProperty("retainedPeakBytes", (juce::int64)bytes);
    o->setProperty("pending", pending.isNotEmpty());
    o->setProperty("firstReadyMs", firstReadyMs);
    auto times = loadMs;
    std::sort(times.begin(), times.end());
    auto *latency = new juce::DynamicObject();
    latency->setProperty("n", (int)times.size());
    if (!times.empty())
        for (auto [name, fraction] : {std::pair{"p50", .5}, {"p95", .95}, {"p99", .99}})
            latency->setProperty(name, times[(size_t)std::ceil(times.size() * fraction) - 1]);
    o->setProperty("requestToUiAdoptionMs", juce::var(latency));
    return juce::var(o);
}
} // namespace sp
