#pragma once
#include "UIComponents.h"
#include <deque>
#include <mutex>
// Included by the isolated service after restore/describe helpers are defined.
namespace
{
template <class Slot> class RackLiveUI : private AudioProcessorListener
{
    const juce::var job;
    std::vector<Slot> &slots;
    sp::HostedRackExchange &exchange;
    File directory;
    String generation;
    sp::HostedRack requested;
    int64_t lastEditor = 0, lastModel = 0, lastBaseline = 0, captureSequence = 0, nativeSequence = 0;
    double nextPoll = 0, nextCapture = 0, nextCheckpoint = 0;
    std::vector<String> publishedHashes;
    int64_t publishedModel = 0, publishedNative = 0;
    bool hasPublished = false;
    std::atomic<int64_t> processedFrames{0}, processBegin{0}, processEnd{0}, nativeEventTicks{0};
    std::atomic<int64_t> modelAppliedTicks{0}, modelSourceTicks{0}, firstActor{0}, firstSource{0},
        firstDspBegin{0}, firstDspEnd{0};
    std::mutex traceMutex;
    std::map<String, std::vector<float>> capturedSeeds;
    std::deque<String> capturedOrder;
    std::vector<int64_t> restoredSequence;
    int64_t appliedFrames = -1, appliedTicks = 0, eventTicks = 0;
    std::atomic<bool> writing{false}, nativeDirty{false};
    std::atomic<int64_t> gestureSequence{0};
    std::atomic<int> activeGestures{0};
    bool pendingCapture = false;
    std::vector<std::vector<float>> lastValues;
    void audioProcessorParameterChanged(AudioProcessor *, int, float) override
    {
        if (!writing.load())
        {
            nativeEventTicks = Time::getHighResolutionTicks();
            nativeDirty = true;
        }
    }
    void audioProcessorChanged(AudioProcessor *, const AudioProcessorListener::ChangeDetails &) override
    {
        if (!writing.load())
        {
            nativeEventTicks = Time::getHighResolutionTicks();
            nativeDirty = true;
        }
    }
    void audioProcessorParameterChangeGestureBegin(AudioProcessor *, int) override
    {
        if (!writing.load())
        {
            ++activeGestures;
            ++gestureSequence;
            {
                nativeEventTicks = Time::getHighResolutionTicks();
                nativeDirty = true;
            }
        }
    }
    void audioProcessorParameterChangeGestureEnd(AudioProcessor *, int) override
    {
        if (!writing.load())
        {
            activeGestures = std::max(0, activeGestures.load() - 1);
            {
                nativeEventTicks = Time::getHighResolutionTicks();
                nativeDirty = true;
            }
        }
    }
    void rememberValues()
    {
        lastValues.clear();
        for (auto &slot : slots)
        {
            std::vector<float> values;
            for (auto *p : slot.effect->getParameters())
                values.push_back(p->getValue());
            lastValues.push_back(std::move(values));
        }
    }
    void settleStopped()
    {
        // A zero-length block consumes queued SDK controls without advancing tails.
        // This is control/service work, never the device callback.
        if (exchange.lease.load() == 0)
            onPluginMessageThread(
                [&]
                {
                    for (auto &slot : slots)
                    {
                        AudioBuffer<float> zero(slot.effect->getTotalNumInputChannels(), 0);
                        MidiBuffer midi;
                        slot.effect->processBlock(zero, midi);
                    }
                });
    }
    struct Window final : DocumentWindow
    {
        Window(const String &name) : DocumentWindow(name, sp::ui::surface, DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar(true);
            setAlwaysOnTop(true);
        }
        void closeButtonPressed() override
        {
            setVisible(false);
        }
    };
    std::map<String, std::unique_ptr<Window>> windows;
    bool publish(bool checkpoint)
    {
        bool coherent = true, opaqueObservation = false;
        std::vector<String> hashes;
        var record = obj();
        Array<var> captures;
        onPluginMessageThread(
            [&]
            {
                if (exchange.lease.load() != 0 &&
                    processBegin.load() <= std::max(modelAppliedTicks.load(), nativeEventTicks.load()))
                {
                    coherent = false;
                    return;
                }
                for (size_t i = 0; i < slots.size(); ++i)
                {
                    auto settings = JSON::parse(requested.slots[i].settings);
                    const ScopedLock lock(slots[i].effect->getCallbackLock());
                    auto state = describe(*slots[i].effect);
                    for (auto key :
                         {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
                        put(settings, key, state[key]);
                    std::vector<float> seed;
                    for (auto &p : *state["parameters"].getArray())
                        seed.push_back((float)p["value"]);
                    auto hash = state["stateHash"].toString();
                    hashes.push_back(hash);
                    if (hasPublished && i < publishedHashes.size() && hash != publishedHashes[i] &&
                        lastModel == publishedModel && nativeSequence == publishedNative)
                    {
                        ++nativeSequence;
                        ++gestureSequence;
                        opaqueObservation = true;
                    }
                    if (!capturedSeeds.contains(hash))
                        capturedOrder.push_back(hash);
                    capturedSeeds[hash] = std::move(seed);
                    while (capturedOrder.size() > 256)
                    {
                        capturedSeeds.erase(capturedOrder.front());
                        capturedOrder.pop_front();
                    }
                    auto row = obj();
                    put(row, "slot", requested.slots[i].id);
                    put(row, "settings", settings);
                    put(row, "instance", generation);
                    captures.add(row);
                    {
                        std::lock_guard metrics(traceMutex);
                        put(record, "gesture", (int64)gestureSequence.load());
                        put(record, "gestureActive", activeGestures.load() > 0);
                        put(record, "firstDspControlAppliedTicks", (int64)firstActor.load());
                        put(record, "firstDspEventTicks", (int64)firstSource.load());
                        put(record, "firstDspStartTicks", (int64)firstDspBegin.load());
                        put(record, "firstDspEndTicks", (int64)firstDspEnd.load());
                        put(record, "processedFrames", (int64)processedFrames.load());
                        put(record, "capturedTicks", (int64)Time::getHighResolutionTicks());
                    }
                }
                exchange.heartbeatTicks = Time::getHighResolutionTicks();
            });
        if (!coherent)
            return false;
        put(record, "generation", generation);
        put(record, "binding", job["binding"]);
        put(record, "modelSequence", (int64)lastModel);
        put(record, "captureSequence", (int64)captureSequence);
        put(record, "nativeSequence", (int64)nativeSequence);
        put(record, "eventTicks", (int64)eventTicks);
        put(record, "appliedTicks", (int64)appliedTicks);
        put(record, "dspBlockStartTicks", (int64)processBegin.load());
        put(record, "dspBlockEndTicks", (int64)processEnd.load());
        put(record, "slots", captures);
        put(record, "opaqueObservation", opaqueObservation);
        hasPublished = true;
        publishedHashes = std::move(hashes);
        publishedModel = lastModel;
        publishedNative = nativeSequence;
        sp::saveProcessingJson(directory.getChildFile("state.json"), record);
        if (checkpoint)
            sp::saveProcessingJson(directory.getChildFile("checkpoint.json"), record);
        return true;
    }

  public:
    RackLiveUI(const var &request, std::vector<Slot> &prepared, sp::HostedRackExchange &e)
        : job(request), slots(prepared), exchange(e), directory(request["liveDirectory"].toString()),
          generation(request["generation"].toString()), requested(sp::hostedRackFromJson(request["rack"]))
    {
        restoredSequence.resize(slots.size());
        var endpoint = obj();
        put(endpoint, "generation", generation);
        put(endpoint, "binding", job["binding"]);
        put(endpoint, "rack", requested.id);
        put(endpoint, "pid", (int)::getpid());
        put(endpoint, "rate", job["rate"]);
        put(endpoint, "layout", job["layout"]);
        put(endpoint, "exchange", job["exchange"]);
        Array<var> identities;
        for (auto &slot : requested.slots)
        {
            auto row = obj();
            put(row, "slot", slot.id);
            put(row, "plugin", JSON::parse(slot.settings)["plugin"]);
            identities.add(row);
        }
        put(endpoint, "slots", identities);
        sp::saveProcessingJson(File(job["endpoint"].toString()), endpoint);
        onPluginMessageThread(
            [&]
            {
                for (auto &slot : slots)
                    slot.effect->addListener(this);
                rememberValues();
            });
        publish(true);
    }
    ~RackLiveUI()
    {
        onPluginMessageThread(
            [&]
            {
                windows.clear();
                for (auto &slot : slots)
                    slot.effect->removeListener(this);
            });
    }
    void processed(int frames, int64_t began)
    {
        std::lock_guard metrics(traceMutex);
        processedFrames += frames;
        const auto end = Time::getHighResolutionTicks();
        processEnd = end;
        const auto model = modelAppliedTicks.load(), native = nativeEventTicks.load(),
                   actor = std::max(model, native);
        if (actor > 0 && began > actor && firstActor.load() != actor)
        {
            firstSource = native > model ? native : modelSourceTicks.load();
            firstDspBegin = began;
            firstDspEnd = end;
            firstActor = actor;
        }
        processBegin = began;
    }
    void poll()
    {
        auto reset = exchange.resetRequested.load(std::memory_order_acquire);
        if (reset != exchange.resetAcknowledged.load())
        {
            onPluginMessageThread(
                [&]
                {
                    for (auto &slot : slots)
                    {
                        slot.effect->reset();
                        slot.dry.clear();
                        slot.write = 0;
                    }
                });
            exchange.resetAcknowledged.store(reset, std::memory_order_release);
        }
        auto now = Time::getMillisecondCounterHiRes();
        if (now < nextPoll)
            return;
        nextPoll = now + 25;
        onPluginMessageThread([&] { exchange.heartbeatTicks = Time::getHighResolutionTicks(); });
        auto read = [&](const char *name) -> var
        {
            auto file = directory.getChildFile(name);
            if (!file.existsAsFile())
                return {};
            auto v = sp::loadProcessingJson(file);
            return v["generation"].toString() == generation ? v : var();
        };
        if (editorCloseRequested.exchange(false))
            onPluginMessageThread(
                [&]
                {
                    for (auto &[id, window] : windows)
                        window->setVisible(false);
                });
        auto command = read("editor.json");
        if (command.isObject() && (int64_t)command["sequence"] > lastEditor)
        {
            lastEditor = (int64_t)command["sequence"];
            auto id = command["slot"].toString();
            onPluginMessageThread(
                [&]
                {
                    for (size_t i = 0; i < slots.size(); ++i)
                        if (requested.slots[i].id == id)
                        {
                            if (!(bool)command["open"])
                            {
                                if (windows.contains(id))
                                    windows[id]->setVisible(false);
                                return;
                            }
                            if (!windows.contains(id))
                            {
                                auto &effect = *slots[i].effect;
                                auto *editor = effect.hasEditor() ? effect.createEditorIfNeeded() : nullptr;
                                if (!editor)
                                    editor = new GenericAudioProcessorEditor(effect);
                                auto window = std::make_unique<Window>(effect.getName() + " · " +
                                                                       job["binding"]["label"].toString());
                                window->setContentOwned(editor, true);
                                window->setResizable(editor->isResizable(), false);
                                window->centreWithSize(editor->getWidth(), editor->getHeight());
                                windows[id] = std::move(window);
                            }
                            windows[id]->setVisible(true);
                            Process::makeForegroundProcess();
                            windows[id]->toFront(true);
                        }
                });
        }
        command = read("baseline.json");
        if (command.isObject() && (int64_t)command["sequence"] > lastBaseline)
        {
            auto accepted = sp::hostedRackFromJson(command["rack"]);
            check(accepted.id == requested.id && accepted.slots.size() == slots.size(),
                  "Capture acknowledgement owner changed");
            for (size_t i = 0; i < slots.size(); ++i)
            {
                auto settings = JSON::parse(accepted.slots[i].settings);
                auto seed = capturedSeeds.find(settings["stateHash"].toString());
                check(seed != capturedSeeds.end(), "Capture acknowledgement expired; retry coherent capture");
                slots[i].opaqueValues = seed->second;
                slots[i].baselineRequests = seed->second;
            }
            lastBaseline = (int64_t)command["sequence"];
        }
        command = read("model.json");
        bool capture = false;
        if (command.isObject() && (int64_t)command["sequence"] > lastModel)
        {
            auto next = sp::hostedRackFromJson(command["rack"]);
            check(next.id == requested.id && next.slots.size() == requested.slots.size(),
                  "Live slot topology changed");
            writing = true;
            onPluginMessageThread(
                [&]
                {
                    for (size_t i = 0; i < slots.size(); ++i)
                    {
                        check(next.slots[i].id == requested.slots[i].id, "Live slot identity changed");
                        auto before = JSON::parse(requested.slots[i].settings),
                             after = JSON::parse(next.slots[i].settings);
                        for (auto key : {"id", "format", "path", "fingerprint"})
                            check(before["plugin"][key] == after["plugin"][key],
                                  "Live processor identity changed");
                        auto &slot = slots[i];
                        const ScopedLock lock(slot.effect->getCallbackLock());
                        validateRequestedIdentity(*slot.effect, after);
                        if ((bool)command["changes"][(int)i]["fullState"] &&
                            restoredSequence[i] != (int64_t)command["changes"][(int)i]["restoreSequence"])
                        {
                            restoredSequence[i] = (int64_t)command["changes"][(int)i]["restoreSequence"];
                            restore(*slot.effect, after, &slot.opaqueValues);
                            validateRequestedIdentity(*slot.effect, after);
                            std::fill(slot.baselineRequests.begin(), slot.baselineRequests.end(),
                                      std::numeric_limits<float>::quiet_NaN());
                            if (auto *baseline = after["parameterBaseline"].getArray())
                                for (auto &p : *baseline)
                                    slot.baselineRequests.at((size_t)(int)p["index"]) = (float)p["value"];
                        }
                        else
                        {
                            auto *parameters = after["parameters"].getArray();
                            auto old = before["parameters"];
                            check(parameters && sp::pluginParameterSchemaSignature(old) ==
                                                    sp::pluginParameterSchemaSignature(after["parameters"]),
                                  "Live parameter schema changed");
                            for (int p = 0; p < parameters->size(); ++p)
                            {
                                auto value = (*parameters)[p];
                                int index = (int)value["index"];
                                if (command["changes"][(int)i]["indices"].isArray() &&
                                    command["changes"][(int)i]["indices"].getArray()->contains(
                                        value["index"]))
                                {
                                    float x = (float)value["value"];
                                    float effective = x == slot.baselineRequests[(size_t)index]
                                                          ? slot.opaqueValues[(size_t)index]
                                                          : x;
                                    slot.effect->getParameters()[index]->setValueNotifyingHost(effective);
                                }
                            }
                        }
                        *slot.bypass = next.bypass || next.slots[i].bypass;
                        modelSourceTicks = (int64_t)command["eventTicks"];
                        modelAppliedTicks = Time::getHighResolutionTicks();
                    }
                });
            settleStopped();
            onPluginMessageThread([&] { rememberValues(); });
            writing = false;
            appliedFrames = processedFrames.load();
            appliedTicks = Time::getHighResolutionTicks();
            eventTicks = (int64_t)command["eventTicks"];
            pendingCapture = true;
            requested = std::move(next);
            lastModel = (int64_t)command["sequence"];
        }
        command = read("capture.json");
        if (command.isObject() && (int64_t)command["sequence"] > captureSequence)
        {
            captureSequence = (int64_t)command["sequence"];
            settleStopped();
            pendingCapture = true;
        }
        bool changed = false;
        onPluginMessageThread(
            [&]
            {
                for (size_t i = 0; i < slots.size(); ++i)
                {
                    auto &effect = *slots[i].effect;
                    check(effect.getParameters().size() == (int)lastValues[i].size(),
                          "Live schema changed; reprepare required");
                    for (int p = 0; p < effect.getParameters().size(); ++p)
                        check(effect.getHostedParameter(p)->getParameterID() == slots[i].parameterIds[p],
                              "Live parameter identity changed; reprepare required");
                    for (int p = 0; p < effect.getParameters().size(); ++p)
                        if (effect.getParameters()[p]->getValue() != lastValues[i][(size_t)p])
                            changed = true;
                }
            });
        const bool dirty = nativeDirty.exchange(false);
        if (dirty || (changed && !pendingCapture))
        {
            ++nativeSequence;
            auto stamp = nativeEventTicks.load();
            eventTicks = stamp > 0 ? stamp : Time::getHighResolutionTicks();
            appliedTicks = eventTicks;
            settleStopped();
            pendingCapture = true;
        }
        capture = pendingCapture && (exchange.lease.load() == 0 || processBegin.load() > appliedTicks);
        if (((capture && now >= nextCapture) || now >= nextCheckpoint) && publish(true))
        {
            onPluginMessageThread([&] { rememberValues(); });
            pendingCapture = false;
            nextCapture = now + 75;
            nextCheckpoint = now + 2000;
        }
    }
};
} // namespace
