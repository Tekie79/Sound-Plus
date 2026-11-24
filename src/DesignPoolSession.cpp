#include "PerformanceState.h"
#include "DesignSession.h"
namespace sp
{
namespace
{
DesignLayer *findLayer(DesignDocument &d, const juce::String &id)
{
    for (auto &l : d.activeMix().layers)
        if (l.id == id)
            return &l;
    return nullptr;
}
SourcePoolEntry *findEntry(DesignLayer *l, const juce::String &id)
{
    if (l && l->pool)
        for (auto &e : l->pool->entries)
            if (e.id == id)
                return &e;
    return nullptr;
}
juce::String signature(const SourcePoolEntry &e)
{
    return juce::JSON::toString(poolEntryToJson(e), true);
}
juce::String context(const DesignDocument &d, const juce::String &layer)
{
    return d.id + ":" + d.activeVariation + ":" + layer;
}
} // namespace
void DesignSession::invalidatePoolWork()
{
    ++poolHistoryEpoch;
    for (auto &[key, counter] : poolRequests)
        if (counter)
            ++*counter;
    poolRequests.clear();
}
void DesignSession::addPoolSource(const juce::String &id, const Asset &source, int64_t first, int64_t last,
                                  bool external)
{
    addPoolEntries(id, [source, first, last, external]
                   { return std::vector<SourcePoolEntry>{inspectPoolEntry(source, first, last, external)}; });
}
void DesignSession::addPoolFiles(const juce::String &id, const juce::StringArray &files)
{
    if (files.isEmpty())
        return;
    addPoolEntries(id,
                   [files]
                   {
                       if (files.size() > (int)maxPoolEntries - 1)
                           throw std::runtime_error(
                               "A layer supports up to 64 alternatives including its initial source");
                       std::vector<SourcePoolEntry> entries;
                       for (auto &path : files)
                       {
                           auto a = inspectDesignSource(juce::File(path));
                           entries.push_back(inspectPoolEntry(a, 0, a.samples, true));
                       }
                       return entries;
                   });
}
void DesignSession::addPoolEntries(const juce::String &id, std::function<std::vector<SourcePoolEntry>()> load)
{
    auto *layer = findLayer(document, id);
    if (!layer || takeBusy() || replacing || document.activeTake.isNotEmpty())
        return;
    protectStartupEdits();
    auto base = *layer;
    auto bus = document.activeMix().layout;
    auto key = context(document, id);
    auto epoch = poolHistoryEpoch;
    auto token = stamp(Operation::import);
    working = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(
        token,
        [safe, base, bus, key, epoch, load = std::move(load)]
        {
            auto entries = load();
            std::optional<SourcePoolEntry> initial;
            if (!base.pool)
            {
                initial = poolEntry(base, bus);
                verifyPoolEntry(*initial);
            }
            return [safe, base, bus, key, epoch, entries = std::move(entries),
                    initial = std::move(initial)]() mutable
            {
                if (!safe)
                    return;
                safe->working = !safe->acceptedImports.empty();
                auto *layer = findLayer(safe->document, base.id);
                if (safe->takeBusy() || safe->document.activeTake.isNotEmpty() ||
                    safe->poolHistoryEpoch != epoch || context(safe->document, base.id) != key || !layer ||
                    safe->document.activeMix().layout != bus)
                {
                    safe->message =
                        "Alternative import belongs to an earlier context; current design retained";
                    return;
                }
                if (!layer->pool &&
                    (layer->source.id != base.source.id || layer->source.revision != base.source.revision))
                {
                    safe->message = "Source changed during alternate import; try again";
                    return;
                }
                safe->editMetadata("Add alternatives",
                                   [&](auto &d)
                                   {
                                       auto *l = findLayer(d, base.id);
                                       if (!l->pool)
                                       {
                                           l->pool.emplace();
                                           l->source.detectedChannels = initial->source.detectedChannels;
                                           l->source.layoutConfidence = initial->source.layoutConfidence;
                                           l->pool->entries.push_back(*initial);
                                           l->pool->selected = initial->id;
                                       }
                                       if (l->pool->entries.size() + entries.size() > maxPoolEntries)
                                           throw std::runtime_error(
                                               "64 entries per layer maximum; remove an alternative first");
                                       for (auto &e : entries)
                                           l->pool->entries.push_back(std::move(e));
                                   });
            };
        });
    notify();
}
void DesignSession::selectPoolEntry(const juce::String &layer, const juce::String &entry)
{
    requestPoolActivation(layer, entry, std::nullopt);
}
void DesignSession::reviewAndSelectPoolEntry(const juce::String &layer, const juce::String &entry,
                                             const LayerRouting &routing, int pair)
{
    requestPoolActivation(layer, entry, routing, pair);
}
void DesignSession::requestPoolActivation(const juce::String &id, const juce::String &entry,
                                          std::optional<LayerRouting> routing, int pair,
                                          const juce::String &remove)
{
    auto *layer = findLayer(document, id);
    auto *item = findEntry(layer, entry);
    if (!item || takeBusy() || replacing || document.activeTake.isNotEmpty())
        return;
    protectStartupEdits();
    auto candidate = *item;
    auto original = signature(*item);
    auto poolId = layer->pool->id;
    auto key = context(document, id);
    auto bus = document.activeMix().layout;
    auto epoch = poolHistoryEpoch;
    auto &counter = poolRequests[key];
    if (!counter)
        counter = std::make_shared<std::atomic<uint64_t>>(0);
    auto generation = ++*counter;
    auto serial = counter;
    auto token = stamp(Operation::import);
    working = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, id, entry, candidate, original, poolId, key, bus, epoch, generation, serial, routing, pair,
             remove]() mutable
            {
                if (serial->load() != generation)
                    return std::function<void()>(
                        [safe]
                        {
                            if (safe)
                                safe->working = !safe->acceptedImports.empty();
                        });
                if (routing)
                {
                    candidate.region.auditionPair = pair;
                    reviewPoolEntry(candidate, *routing, bus);
                }
                // Check compatibility before expensive I/O and recheck before committing.
                DesignLayer probe;
                probe.source = candidate.source;
                probe.region = candidate.region;
                probe.sync = candidate.region.startSample;
                probe.pool.emplace();
                probe.pool->entries = {candidate};
                probe.pool->selected = entry;
                activatePoolEntry(probe, entry, bus);
                try
                {
                    verifyPoolEntry(candidate, false,
                                    [serial, generation] { return serial->load() != generation; });
                }
                catch (const QueryCancelled &)
                {
                    return std::function<void()>(
                        [safe]
                        {
                            if (safe)
                                safe->working = !safe->acceptedImports.empty();
                        });
                }
                return std::function<void()>(
                    [safe, id, entry, candidate = std::move(candidate), original, poolId, key, bus, epoch,
                     generation, serial, remove]() mutable
                    {
                        if (!safe)
                            return;
                        safe->working = !safe->acceptedImports.empty();
                        auto *layer = findLayer(safe->document, id);
                        auto *current = findEntry(layer, entry);
                        if (serial->load() != generation || safe->poolHistoryEpoch != epoch ||
                            safe->takeBusy() || safe->document.activeTake.isNotEmpty() ||
                            context(safe->document, id) != key || !current || layer->pool->id != poolId ||
                            signature(*current) != original || safe->document.activeMix().layout != bus)
                        {
                            safe->message = "Obsolete source preparation discarded";
                            return;
                        }
                        // A verified unchanged review is observational, including seeded provenance.
                        if (remove.isEmpty() && layer->pool->selected == entry &&
                            signature(candidate) == original && layer->source.error.isEmpty() &&
                            layer->source.online)
                        {
                            safe->message = "Source verified; reviewed mapping unchanged";
                            return;
                        }
                        auto previousRevision = safe->document.revision;
                        safe->edit(remove.isEmpty() ? "Select alternative" : "Remove selected alternative",
                                   [&](auto &d)
                                   {
                                       auto *l = findLayer(d, id);
                                       *findEntry(l, entry) = candidate;
                                       activatePoolEntry(*l, entry, bus);
                                       l->pool->policy = "explicit-v1";
                                       l->pool->seed = l->pool->ordinal = 0;
                                       if (remove.isNotEmpty())
                                           std::erase_if(l->pool->entries,
                                                         [&](auto &e) { return e.id == remove; });
                                   });
                        if (safe->document.revision != previousRevision)
                            safe->message = "Source selected; press Play to audition the updated design";
                    });
            });
    notify();
}
void DesignSession::removePoolEntry(const juce::String &id, const juce::String &entry,
                                    const juce::String &replacement)
{
    auto *l = findLayer(document, id);
    if (!findEntry(l, entry) || takeBusy() || document.activeTake.isNotEmpty())
        return;
    if (l->pool->entries.size() < 2)
    {
        message = "Keep at least one source in this layer";
        notify();
        return;
    }
    if (l->pool->selected == entry)
    {
        if (replacement.isEmpty() || replacement == entry)
        {
            message = "Choose the replacement source explicitly before removing the active entry";
            notify();
            return;
        }
        requestPoolActivation(id, replacement, std::nullopt, -99, entry);
        return;
    }
    editMetadata("Remove alternative",
                 [&](auto &d)
                 {
                     auto *layer = findLayer(d, id);
                     std::erase_if(layer->pool->entries, [&](auto &e) { return e.id == entry; });
                 });
}
void DesignSession::reorderPoolEntry(const juce::String &id, const juce::String &entry, int delta)
{
    editMetadata("Reorder alternative",
                 [&](auto &d)
                 {
                     auto *l = findLayer(d, id);
                     if (!findEntry(l, entry))
                         return;
                     auto &entries = l->pool->entries;
                     auto i = std::find_if(entries.begin(), entries.end(),
                                           [&](auto &e) { return e.id == entry; }) -
                              entries.begin();
                     auto next = std::clamp<int>((int)i + delta, 0, (int)entries.size() - 1);
                     std::swap(entries[(size_t)i], entries[(size_t)next]);
                 });
}
void DesignSession::setPoolEntryEnabled(const juce::String &id, const juce::String &entry, bool enabled)
{
    editMetadata("Source variation eligibility",
                 [&](auto &d)
                 {
                     if (auto *e = findEntry(findLayer(d, id), entry))
                         e->enabled = enabled;
                 });
}
void DesignSession::lockPoolChoice(const juce::String &id, bool locked)
{
    editMetadata("Lock source choice",
                 [&](auto &d)
                 {
                     if (auto *l = findLayer(d, id); l && l->pool)
                         l->pool->choiceLocked = locked;
                 });
}
void DesignSession::relinkPoolEntry(const juce::String &id, const juce::String &entry, const juce::File &file)
{
    auto *l = findLayer(document, id);
    auto *e = findEntry(l, entry);
    if (!e || takeBusy() || replacing || document.activeTake.isNotEmpty())
        return;
    protectStartupEdits();
    auto original = signature(*e);
    auto candidate = *e;
    auto bus = document.activeMix().layout;
    auto key = context(document, id);
    auto epoch = poolHistoryEpoch;
    auto token = stamp(Operation::import);
    working = true;
    candidate.source.path = file.getFullPathName();
    candidate.external = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, id, entry, candidate, original, key, epoch, bus]() mutable
            {
                verifyPoolEntry(candidate, true);
                return [safe, id, entry, candidate, original, key, epoch, bus]
                {
                    if (!safe)
                        return;
                    safe->working = !safe->acceptedImports.empty();
                    auto *l = findLayer(safe->document, id);
                    auto *e = findEntry(l, entry);
                    if (safe->takeBusy() || safe->document.activeTake.isNotEmpty() ||
                        safe->poolHistoryEpoch != epoch || context(safe->document, id) != key || !e ||
                        signature(*e) != original || safe->document.activeMix().layout != bus)
                        return;
                    bool active = l->pool->selected == entry;
                    safe->edit(
                        active ? "Relocate selected alternate" : "Relocate alternate",
                        [&](auto &d)
                        {
                            auto *layer = findLayer(d, id);
                            *findEntry(layer, entry) = candidate;
                            if (active)
                                activatePoolEntry(*layer, entry, bus);
                        },
                        active ? ChangeImpact::audio : ChangeImpact::metadata);
                };
            });
    notify();
}
void DesignSession::generatePoolVariations(uint64_t seed)
{
    if (takeBusy() || replacing || document.activeTake.isNotEmpty())
        return;
    protectStartupEdits();
    auto snapshot = document;
    auto epoch = poolHistoryEpoch;
    auto token = stamp(Operation::import);
    working = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, snapshot, epoch, seed, resolve = services.resolve]() mutable
            {
                resolve(snapshot);
                auto variations = generateVariations(snapshot, seed);
                for (auto &v : variations)
                    for (auto &l : v.mix.layers)
                        if (l.pool)
                        {
                            auto *e = findEntry(&l, l.pool->selected);
                            verifyPoolEntry(*e);
                            l.source = e->source;
                            l.region = e->region;
                        }
                return [safe, snapshot, epoch, variations = std::move(variations)]() mutable
                {
                    if (!safe)
                        return;
                    safe->working = !safe->acceptedImports.empty();
                    if (safe->takeBusy() || safe->document.activeTake.isNotEmpty() ||
                        safe->poolHistoryEpoch != epoch || safe->document.id != snapshot.id ||
                        safe->document.revision != snapshot.revision)
                    {
                        safe->message = "Variation preparation belongs to an earlier design; generate again";
                        return;
                    }
                    safe->edit("Generate source variations",
                               [&](auto &d)
                               {
                                   if (d.variations.size() + variations.size() > 32)
                                       throw std::runtime_error("32 variations maximum");
                                   for (auto &v : variations)
                                       d.variations.push_back(std::move(v));
                               });
                };
            });
    notify();
}
} // namespace sp

namespace sp
{
void DesignSession::learnSourceTrigger(const juce::String &layer, const juce::String &entry,
                                       const juce::String &device)
{
    if (ownsPlayback() || takeBusy() || document.activeTake.isNotEmpty() || device.isEmpty())
        return;
    learningSource = LearningSource{juce::String((juce::int64)lifetime) + "/" + document.id + "/" +
                                        document.activeVariation,
                                    layer, entry, device};
    nextSourceScan = 0;
    message = "Play a MIDI note or move a controller";
    notify();
}
void DesignSession::forgetSourceTrigger(const juce::String &id)
{
    learningSource.reset();
    nextSourceScan = 0;
    editMetadata("Forget source trigger",
                 [&](auto &d)
                 {
                     for (auto &l : d.activeMix().layers)
                         if (l.id == id && l.pool)
                             l.pool->triggerMidi.reset();
                 });
}
void DesignSession::pollSourceInputs()
{
    auto context = juce::String((juce::int64)lifetime) + "/" + document.id + "/" + document.activeVariation;
    if (sourceInputContext != context || document.activeTake.isNotEmpty())
    {
        sourceInputs.clear();
        heldSourceKeys.clear();
        sourceInputContext = context;
        nextSourceScan = 0;
        if (learningSource && learningSource->context != context)
            learningSource.reset();
        if (document.activeTake.isNotEmpty())
        {
            learningSource.reset();
            return;
        }
    }
    auto &mix = document.activeMix();
    if (sourceInputRevision != document.revision)
    {
        sourceInputRevision = document.revision;
        nextSourceScan = 0;
    }
    if (services.now() >= nextSourceScan)
    {
        nextSourceScan = services.now() + 1500;
        std::set<juce::String> wanted;
        if (learningSource)
            wanted.insert(learningSource->device);
        for (auto &l : mix.layers)
            if (l.pool && l.pool->live && l.pool->triggerMidi)
                wanted.insert(l.pool->triggerMidi->device);
        if (wanted.empty())
        {
            sourceInputs.clear();
            heldSourceKeys.clear();
            return;
        }
        auto devices = juce::MidiInput::getAvailableDevices();
        std::erase_if(sourceInputs,
                      [&](auto &v)
                      {
                          bool gone = !wanted.contains(v.first) ||
                                      std::none_of(devices.begin(), devices.end(),
                                                   [&](auto &d) { return d.identifier == v.first; });
                          if (gone)
                              std::erase_if(heldSourceKeys, [&](auto &key) { return key.first == v.first; });
                          return gone;
                      });
        for (auto &d : devices)
            if (wanted.contains(d.identifier) && !sourceInputs.contains(d.identifier))
            {
                auto p = std::make_unique<AutomationMidiInput>();
                if (p->open(d.identifier, true))
                    sourceInputs.emplace(d.identifier, std::move(p));
            }
    }
    for (auto &[device, input] : sourceInputs)
    {
        if (input->overflowed())
        {
            input->drain([](auto) {});
            heldSourceKeys.clear();
            message = "MIDI trigger queue full";
            continue;
        }
        input->drain(
            [&](auto e)
            {
                auto key = std::make_pair(device, (e.channel * 2 + int(e.note)) * 128 + e.number);
                if (e.value < (e.note ? 1 : 64))
                {
                    heldSourceKeys.erase(key);
                    return;
                }
                if (!heldSourceKeys.insert(key).second)
                    return;
                if (learningSource && learningSource->device == device)
                {
                    auto learn = *learningSource;
                    learningSource.reset();
                    editMetadata("Learn source trigger",
                                 [&](auto &d)
                                 {
                                     for (auto &l : d.activeMix().layers)
                                         if (l.id == learn.layer && l.pool && l.pool->live)
                                             l.pool->triggerMidi = SourceTriggerMidi{
                                                 device, learn.entry, e.channel, e.number, e.note};
                                 });
                    message = "MIDI trigger learned";
                    notify();
                    return;
                }
                if (!canTriggerSource())
                    return;
                for (int pad = 0; pad <= int(mix.performancePads.size()); ++pad)
                    for (int c = 0; c < 4; ++c)
                    {
                        auto &id = padState(mix, pad).assignments[(size_t)c].layer;
                        for (auto &l : mix.layers)
                            if (l.id == id && l.pool && l.pool->live && l.pool->triggerMidi)
                            {
                                auto &m = *l.pool->triggerMidi;
                                if (m.device == device && m.channel == e.channel && m.note == e.note &&
                                    m.key == e.number)
                                {
                                    auto db =
                                        e.note ? juce::Decibels::gainToDecibels(e.value / 127., -60.) : 0.;
                                    triggerSource(pad, c, m.entry, false, 0, db);
                                    return;
                                }
                            }
                    }
            });
    }
}
} // namespace sp
