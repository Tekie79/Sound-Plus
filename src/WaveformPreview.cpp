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
                           std::function<double()> clockSource, WaveformCache::Checkpoint checkpoint)
    : decodes(std::make_shared<std::atomic<uint64_t>>(0)),
      now(clockSource ? std::move(clockSource) : [] { return juce::Time::getMillisecondCounterHiRes(); }),
      worker(
          directory,
          [counter = decodes, beforeRead]
          {
              ++*counter;
              if (beforeRead)
                  beforeRead();
          },
          std::move(checkpoint))
{
    startedMs = now();
}
void WaveformPool::demand(int owner, const std::vector<Asset> &assets)
{
    if (owner < 0 || owner > 3)
        return;
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
void WaveformPool::adoptDisplay(const juce::String &key, Entry entry)
{
    auto summary = std::make_shared<WaveformData>(*entry.data);
    if (summary->peaks.size() > maxSummaryPairs)
        summary->peaks = compactWaveformColumns(*summary, 0, summary->samples, false, (int)maxSummaryPairs);
    entry.data = std::move(summary);
    displays[key] = std::move(entry);
    while (displays.size() > maxDisplaySources)
    {
        auto oldest = std::min_element(displays.begin(), displays.end(),
                                       [](auto &a, auto &b) { return a.second.used < b.second.used; });
        displays.erase(oldest);
    }
}
bool WaveformPool::tick()
{
    struct Wanted
    {
        Asset asset;
        juce::String key;
        int owner;
    };
    std::vector<Wanted> wanted;
    std::set<juce::String> keys, hot;
    for (auto &[owner, assets] : owners)
        for (auto &a : assets)
        {
            auto key = waveformSourceKey(a);
            if (keys.insert(key).second)
                wanted.push_back({a, key, owner});
            // One selected source and four cards normally occupy five reservations.
            if (owner <= 1 && hot.size() < 8)
                hot.insert(key);
        }
    std::erase_if(serviced, [&](auto &entry) { return !keys.contains(entry.first); });
    std::erase_if(displays, [&](auto &entry) { return !keys.contains(entry.first); });
    bool changed = false;
    auto missingSource = [&](const Wanted &w)
    { return !completed.contains(w.key) && (!displays.contains(w.key) || hot.contains(w.key)); };
    auto foreground =
        std::find_if(wanted.begin(), wanted.end(), [&](auto &w) { return w.owner == 0 && missingSource(w); });
    if (pending.isNotEmpty())
    {
        bool abandoned = !keys.contains(pending);
        bool priority = foreground != wanted.end() && foreground->key != pending && pendingOwner > 0 &&
                        !protectedBackground;
        if (abandoned || priority)
        {
            worker.cancel();
            pending.clear();
            ++cancellations;
            if (priority)
                ++preemptions;
        }
    }
    if (auto result = worker.snapshot(); pending.isNotEmpty() && result && result->generation == token)
    {
        auto entry = Entry{result, ++clock, now()};
        if (pendingValidation && displays.contains(pending) && !completed.contains(pending))
            adoptDisplay(pending, entry);
        else
            completed[pending] = entry;
        // Validation can change availability without changing the catalog key.
        std::erase_if(geometries, [&](auto &g) { return g.first.startsWith(pending + ":"); });
        diskHits += result->cacheHit;
        pending.clear();
        ++changedGeneration;
        changed = true;
        if (firstReadyMs < 0)
            firstReadyMs = now() - startedMs;
        if (loadMs.size() < 512)
            loadMs.push_back(now() - submittedMs);
    }
    if (pending.isEmpty())
    {
        std::vector<const Wanted *> missing;
        const Wanted *validate = nullptr;
        for (auto &w : wanted)
        {
            auto found = completed.find(w.key);
            Entry *entry = found != completed.end()   ? &found->second
                           : displays.contains(w.key) ? &displays.at(w.key)
                                                      : nullptr;
            if (missingSource(w))
                missing.push_back(&w);
            if (entry && now() - entry->validated >= 5000 &&
                (!validate ||
                 entry->validated < (completed.contains(validate->key) ? completed.at(validate->key)
                                                                       : displays.at(validate->key))
                                        .validated))
                validate = &w;
        }
        const Wanted *next = nullptr;
        bool reserved = false;
        auto older = [&](auto *a, auto *b) { return serviced[a->key] < serviced[b->key]; };
        // At most three ordinary admissions between overdue validations. A
        // validation is protected from priority preemption, but not abandonment.
        pendingValidation = validate && (missing.empty() || sinceValidation >= 3);
        if (pendingValidation)
        {
            next = validate;
            reserved = true;
            sinceValidation = 0;
            ++validationAdmissions;
        }
        else if (!missing.empty())
        {
            auto background = missing;
            std::erase_if(background, [](auto *w) { return w->owner == 0; });
            // After three foreground admissions reserve one useful background job.
            // It cannot be priority-cancelled, but abandonment always cancels it.
            reserved = foregroundStreak >= 3 && !background.empty();
            // Every fourth ordinary admission crosses surface-priority boundaries.
            if (reserved || (++admissions % 4 == 0 && !background.empty()))
            {
                next = *std::min_element(background.begin(), background.end(), older);
                ++fairAdmissions;
            }
            else
            {
                int priority = missing.front()->owner;
                std::erase_if(missing, [&](auto *w) { return w->owner != priority; });
                next = *std::min_element(missing.begin(), missing.end(), older);
            }
        }
        if (next)
        {
            pending = next->key;
            pendingOwner = next->owner;
            protectedBackground = reserved;
            if (pendingOwner == 0)
                ++foregroundStreak;
            else
                foregroundStreak = 0;
            serviced[pending] = ++clock;
            std::shared_ptr<const WaveformData> validation;
            if (pendingValidation)
                validation =
                    completed.contains(pending) ? completed.at(pending).data : displays.at(pending).data;
            else
                ++sinceValidation;
            token = worker.request(next->asset, validation);
            submittedMs = now();
            ++requests;
        }
    }
    while (completed.size() > maxSources)
    {
        auto oldest = completed.end();
        for (auto it = completed.begin(); it != completed.end(); ++it)
            if (!hot.contains(it->first) &&
                (oldest == completed.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == completed.end())
            oldest = completed.begin();
        if (keys.contains(oldest->first))
        {
            adoptDisplay(oldest->first, oldest->second);
        }
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
    if (auto it = displays.find(waveformSourceKey(a)); it != displays.end())
    {
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
    displays.erase(key);
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
    size_t displayBytes = 0, maxPairs = 0;
    for (auto &[key, e] : displays)
    {
        displayBytes += e.data->peaks.size() * sizeof(std::pair<float, float>);
        maxPairs = std::max(maxPairs, e.data->peaks.size());
    }
    o->setProperty("maxDisplayPairs", (int)maxPairs);
    o->setProperty("displaySources", (int)displays.size());
    o->setProperty("displayPeakBytes", (juce::int64)displayBytes);
    o->setProperty("validations", (juce::int64)validationAdmissions);
    o->setProperty("cancellations", (juce::int64)cancellations);
    o->setProperty("priorityPreemptions", (juce::int64)preemptions);
    o->setProperty("fairAdmissions", (juce::int64)fairAdmissions);
    o->setProperty("activeDemands", (int)serviced.size());
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
