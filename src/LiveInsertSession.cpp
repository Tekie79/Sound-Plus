#include "LiveInsertSession.h"
#include "PluginProcessing.h"
#include <juce_events/juce_events.h>
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
struct LiveInsertSession::Entry
{
    HostedRack rack;
    ChannelLayout layout;
    double rate = 0;
    Capture binding;
    std::unique_ptr<HostedRackClient> client;
    juce::String lastCapture, pendingSettings;
    bool failed = false, editorVisible = false;
    int64_t lastNative = 0;
};
LiveInsertSession::LiveInsertSession(juce::File directory, Receiver callback,
                                     std::function<void(juce::String)> error)
    : base(directory.getChildFile("live-inserts").getChildFile(juce::Uuid().toString())),
      receive(std::move(callback)), failure(std::move(error)), worker([this] { run(); })
{
}
LiveInsertSession::~LiveInsertSession()
{
    quitting = true;
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}
juce::File LiveInsertSession::rootFor(const DesignDocument &d, uint64_t incarnation) const
{
    auto key = d.id + ":" + juce::String(incarnation) + ":" + d.activeVariation;
    return base.getChildFile(juce::SHA256(key.toRawUTF8(), key.getNumBytesAsUTF8()).toHexString());
}
void LiveInsertSession::sync(const DesignDocument &d, uint64_t incarnation)
{
    std::lock_guard lock(mutex);
    latest = Snapshot{d, incarnation};
    wake.notify_one();
}
void LiveInsertSession::editor(const juce::String &slot, const juce::File &root, const juce::String &owner,
                               bool show)
{
    std::lock_guard lock(mutex);
    open[slot] =
        EditorRequest{root, owner, generations.contains(slot) ? generations[slot] : juce::String(), show};
    wake.notify_one();
}
void LiveInsertSession::flush(std::function<void(bool)> callback)
{
    std::lock_guard lock(mutex);
    flushes.push_back(std::move(callback));
    wake.notify_one();
}
bool LiveInsertSession::accepts(const Capture &capture)
{
    std::lock_guard lock(mutex);
    auto found = generations.find(capture.slot);
    return found != generations.end() && found->second == capture.generation;
}
juce::String LiveInsertSession::problem(const juce::String &slot)
{
    std::lock_guard lock(mutex);
    auto at = problems.find(slot);
    return at == problems.end() ? juce::String() : at->second;
}
void LiveInsertSession::trace(const juce::var &state)
{
    auto event = juce::var(new juce::DynamicObject());
    for (auto key : {"opaqueObservation",  "generation",         "binding",
                     "modelSequence",      "nativeSequence",     "gesture",
                     "gestureActive",      "eventTicks",         "appliedTicks",
                     "dspBlockStartTicks", "dspBlockEndTicks",   "firstDspControlAppliedTicks",
                     "firstDspEventTicks", "firstDspStartTicks", "firstDspEndTicks",
                     "capturedTicks",      "uiReceivedTicks",    "uiAppliedTicks",
                     "deferredByGesture",  "processedFrames",    "device"})
        event.getDynamicObject()->setProperty(key, state[key]);
    event.getDynamicObject()->setProperty("tickFrequency",
                                          (juce::int64)juce::Time::getHighResolutionTicksPerSecond());
    auto settings = state["slots"][0]["settings"];
    event.getDynamicObject()->setProperty("plugin", settings["plugin"]);
    event.getDynamicObject()->setProperty("parameters", settings["parameters"]);
    std::lock_guard lock(mutex);
    if (traces.size() < 128)
        traces.push_back(event);
}
void LiveInsertSession::run()
{
    std::map<juce::String, Entry> entries;
    juce::String context;
    std::map<juce::String, EditorRequest> editors;
    std::vector<std::function<void(bool)>> waiting;
    uint64_t captureTicket = 0;
    double deadline = 0;
    bool flushOkay = true;
    while (!quitting)
    {
        std::optional<Snapshot> next;
        bool requestFlush = false;
        std::vector<juce::var> events;
        {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(25));
            events = std::move(traces);
            traces.clear();
            // Deterministic QA barrier: coalesce Main snapshots while the real
            // retained service continues running/native publication can be held.
            const auto hold =
                juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_LIVE_TEST_HOLD_SNAPSHOT", {});
            if (hold.isEmpty() || !juce::File(hold).existsAsFile())
            {
                next = std::move(latest);
                latest.reset();
            }
            for (auto &[id, show] : open)
                editors[id] = show;
            open.clear();
            if (!flushes.empty())
            {
                for (auto &callback : flushes)
                    waiting.push_back(std::move(callback));
                flushes.clear();
                requestFlush = true;
            }
        }
        if (!events.empty())
        {
            auto file = base.getChildFile("events.jsonl");
            base.createDirectory();
            if (file.getSize() < 32 * 1024 * 1024)
                for (auto &event : events)
                    file.appendText(juce::JSON::toString(event, true) + "\n", false, false, "\n");
        }
        if (next)
        {
            auto root = rootFor(next->document, next->incarnation);
            if (context != root.getFullPathName())
            {
                entries.clear();
                {
                    std::lock_guard lock(mutex);
                    generations.clear();
                    problems.clear();
                }
                context = root.getFullPathName();
            }
            std::set<juce::String> wanted;
            auto reconcile = [&](const HostedRack &rack, const ChannelLayout &layout,
                                 const juce::String &owner, const juce::String &label)
            {
                for (auto &slot : rack.slots)
                {
                    wanted.insert(slot.id);
                    auto found = entries.find(slot.id);
                    auto editorRequest = editors.find(slot.id);
                    if (found != entries.end() && found->second.failed && editorRequest != editors.end() &&
                        editorRequest->second.root == root && editorRequest->second.owner == owner &&
                        editorRequest->second.show)
                    {
                        {
                            std::lock_guard lock(mutex);
                            generations.erase(slot.id);
                        }
                        entries.erase(found);
                        found = entries.end();
                        editorRequest->second.generation
                            .clear(); // Explicit reconnect, from the current model seed.
                    }

                    if (found != entries.end() &&
                        (found->second.binding.owner != owner || found->second.binding.expected.isEmpty() ||
                         found->second.layout != layout || found->second.rate != next->document.sampleRate))
                    {
                        {
                            std::lock_guard lock(mutex);
                            generations.erase(slot.id);
                        }
                        entries.erase(found);
                    }
                    auto &e = entries[slot.id];
                    if (e.binding.slot.isEmpty())
                    {
                        {
                            std::lock_guard lock(mutex);
                            problems.erase(slot.id);
                        }
                        e.rack.id = slot.id;
                        e.rack.slots = {slot};
                        e.rack.runtimeControlTicks = next->document.liveControlTicks;
                        e.rack.bypass = rack.bypass;
                        e.layout = layout;
                        e.rate = next->document.sampleRate;
                        e.binding.document = next->document.id;
                        e.binding.variation = next->document.activeVariation;
                        e.binding.incarnation = next->incarnation;
                        e.binding.owner = owner;
                        e.binding.slot = slot.id;
                        e.binding.expected = slot.settings;
                        auto binding = juce::var(new juce::DynamicObject());
                        auto *b = binding.getDynamicObject();
                        b->setProperty("document", e.binding.document);
                        b->setProperty("incarnation", (juce::int64)e.binding.incarnation);
                        b->setProperty("variation", e.binding.variation);
                        b->setProperty("owner", owner);
                        b->setProperty("slot", slot.id);
                        b->setProperty("rack", rack.id);
                        b->setProperty("label", label);
                        try
                        {
                            e.client = std::make_unique<HostedRackClient>(
                                e.rack, layout, e.rate, [&] { return quitting.load(); },
                                root.getChildFile(slot.id).getChildFile(juce::Uuid().toString()), binding);
                            e.binding.generation = e.client->instanceGeneration();
                            {
                                std::lock_guard lock(mutex);
                                generations[slot.id] = e.binding.generation;
                            }
                        }
                        catch (const std::exception &error)
                        {
                            e.failed = true;
                            {
                                std::lock_guard lock(mutex);
                                problems[e.binding.slot] = error.what();
                            }
                            juce::MessageManager::callAsync(
                                [callback = failure, text = juce::String(error.what())] { callback(text); });
                        }
                    }
                    else if (e.client &&
                             (e.binding.expected != slot.settings ||
                              newerHostedIntents(e.rack.slots[0], slot) || e.rack.bypass != rack.bypass ||
                              e.rack.slots[0].bypass != slot.bypass))
                    {
                        auto before = e.rack;
                        e.rack.bypass = rack.bypass;
                        e.rack.slots = {slot};
                        e.rack.runtimeControlTicks = next->document.liveControlTicks;
                        if (!newerHostedIntents(before.slots[0], slot) &&
                            slot.settings == e.pendingSettings && before.bypass == e.rack.bypass &&
                            before.slots[0].bypass == slot.bypass)
                            e.client->acceptCapture(e.rack);
                        else
                            try
                            {
                                e.client->update(e.rack);
                            }
                            catch (const std::exception &error)
                            {
                                e.failed = true;
                                {
                                    std::lock_guard lock(mutex);
                                    problems[e.binding.slot] = error.what();
                                }
                                juce::MessageManager::callAsync(
                                    [callback = failure, text = juce::String(error.what())]
                                    { callback(text); });
                            }
                        e.binding.expected = slot.settings;
                        e.pendingSettings.clear();
                    }
                }
            };
            if (next->document.activeTake.isEmpty())
            {
                auto &mix = next->document.activeMix();
                reconcile(mix.inserts.hosted,
                          mix.layout.id.isEmpty() ? channelLayout("stereo-v1") : mix.layout, "master",
                          "Master");
                for (auto &layer : mix.layers)
                    reconcile(layer.inserts.hosted,
                              mix.layout.id.isEmpty() ? channelLayout("stereo-v1")
                                                      : layer.routing.interpretation,
                              layer.id, layer.name);
            }
            std::erase_if(entries, [&](auto &pair) { return !wanted.contains(pair.first); });
            {
                std::lock_guard lock(mutex);
                std::erase_if(generations, [&](auto &p) { return !wanted.contains(p.first); });
                std::erase_if(problems, [&](auto &p) { return !wanted.contains(p.first); });
            }
        }
        bool allCaptured = true;
        if (requestFlush)
        {
            ++captureTicket;
            deadline = juce::Time::getMillisecondCounterHiRes() + 6000;
            flushOkay = true;
        }
        for (auto &[id, e] : entries)
        {
            if (!e.client || e.failed)
            {
                flushOkay = false;
                continue;
            }
            try
            {
                e.client->checkHealth();
                if (editors.contains(id))
                {
                    const auto &request = editors[id];
                    if (request.root.getFullPathName() == context && request.owner == e.binding.owner &&
                        (request.generation.isEmpty() || request.generation == e.binding.generation))
                        e.client->editor(id, request.show && !editorsSuspended.load());
                    editors.erase(id);
                }
                if (requestFlush)
                    e.client->capture(captureTicket);
                if (editorsSuspended.load() && e.editorVisible)
                    e.client->editor(id, false);
                auto state = e.client->liveState();
                if (state["generation"].toString() != e.binding.generation ||
                    (uint64_t)(juce::int64)state["modelSequence"] != e.client->modelSequence())
                {
                    allCaptured = false;
                    continue;
                }
                if ((uint64_t)(juce::int64)state["captureSequence"] < captureTicket)
                    allCaptured = false;
                auto serial = state["nativeSequence"].toString() + ":" + state["modelSequence"].toString() +
                              ":" + state["captureSequence"].toString();
                const bool visible = (bool)state["editorVisible"],
                           visibilityChanged = visible != e.editorVisible;
                e.editorVisible = visible;
                if (e.lastCapture == serial && !visibilityChanged)
                    continue;
                e.lastCapture = serial;
                const bool nativeChanged = (juce::int64)state["nativeSequence"] > e.lastNative;
                e.lastNative = (juce::int64)state["nativeSequence"];
                auto settings = juce::JSON::toString(state["slots"][0]["settings"], true);
                if (settings == e.binding.expected && !visibilityChanged)
                    continue;
                auto capture = e.binding;
                capture.settings = settings;
                capture.native = nativeChanged;
                capture.gesture = e.binding.generation + ":" + state["gesture"].toString();
                capture.evidence = state;
                e.pendingSettings = settings;
                juce::MessageManager::callAsync([callback = receive, capture] { callback(capture); });
            }
            catch (const std::exception &error)
            {
                e.failed = true;
                {
                    std::lock_guard lock(mutex);
                    problems[e.binding.slot] = error.what();
                }
                flushOkay = false;
                juce::MessageManager::callAsync([callback = failure, text = juce::String(error.what())]
                                                { callback(text); });
                e.client.reset();
            }
        }
        if (!waiting.empty() &&
            (allCaptured || !flushOkay || juce::Time::getMillisecondCounterHiRes() > deadline))
        {
            auto success = allCaptured && flushOkay;
            for (auto &callback : waiting)
                juce::MessageManager::callAsync([callback, success] { callback(success); });
            waiting.clear();
        }
    }
}
} // namespace sp
