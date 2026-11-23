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
    std::mutex traceMutex, dspOperationMutex;
    std::map<String, std::vector<float>> capturedSeeds;
    std::deque<String> capturedOrder;
    std::vector<int64_t> restoredSequence;
    int64_t appliedFrames = -1, appliedTicks = 0, eventTicks = 0;
    std::atomic<bool> writing{false}, nativeDirty{false};
    std::atomic<int64_t> gestureSequence{0};
    std::atomic<int> activeGestures{0};
    bool pendingCapture = false;
    struct WriteScope
    {
        std::atomic<bool> &flag;
        bool prior;
        explicit WriteScope(std::atomic<bool> &f) : flag(f), prior(f.exchange(true)) {}
        ~WriteScope()
        {
            flag = prior;
        }
    };
    std::vector<std::vector<float>> lastValues;
    std::vector<int> observedPrograms;
    std::atomic<bool> opaqueBoundary{false};
    std::vector<std::unique_ptr<std::atomic<int64_t>[]>> nativeParameterTicks;
    std::vector<std::unique_ptr<std::atomic<float>[]>> observedControlValues;
    std::atomic<int64_t> nativeStateTicks{0}, nativeAnyTicks{0};
    Array<var> skippedControls;
    void noteNativeParameter(AudioProcessor *processor, int index, float value)
    {
        if (writing.load())
            return;
        const auto tick = Time::getHighResolutionTicks();
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].effect.get() == processor && isPositiveAndBelow(index, slots[i].parameterIds.size()))
            {
                if (slots[i].overlayActive[(size_t)index] &&
                    value == slots[i].overlayValues[(size_t)index].load() && activeGestures.load() == 0)
                    return;
                slots[i].manualValues[(size_t)index] = value;
                auto before = observedControlValues[i][index].exchange(value);
                if (before == value && activeGestures.load() == 0)
                    return;
                nativeParameterTicks[i][index] = tick;
                nativeAnyTicks = tick;
            }
        nativeEventTicks = tick;
        nativeDirty = true;
    }
    void noteNativeGesture(AudioProcessor *processor, int index)
    {
        auto tick = Time::getHighResolutionTicks();
        nativeAnyTicks = tick;
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].effect.get() == processor && isPositiveAndBelow(index, slots[i].parameterIds.size()))
                nativeParameterTicks[i][index] = tick;
    }
    void audioProcessorParameterChanged(AudioProcessor *processor, int index, float value) override
    {
        noteNativeParameter(processor, index, value);
    }
    void audioProcessorChanged(AudioProcessor *,
                               const AudioProcessorListener::ChangeDetails &details) override
    {
        if (!writing.load())
        {
            if (details.nonParameterStateChanged)
            {
                opaqueBoundary = true;
                nativeStateTicks = Time::getHighResolutionTicks();
                nativeAnyTicks = nativeStateTicks.load();
            }
            nativeEventTicks = Time::getHighResolutionTicks();
            nativeDirty = true;
        }
    }
    void audioProcessorParameterChangeGestureBegin(AudioProcessor *processor, int index) override
    {
        if (!writing.load())
        {
            noteNativeGesture(processor, index);
            ++activeGestures;
            ++gestureSequence;
            {
                nativeEventTicks = Time::getHighResolutionTicks();
                nativeDirty = true;
            }
        }
    }
    void audioProcessorParameterChangeGestureEnd(AudioProcessor *processor, int index) override
    {
        if (!writing.load())
        {
            noteNativeGesture(processor, index);
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
        observedPrograms.clear();
        for (auto &slot : slots)
        {
            std::vector<float> values;
            const auto i = lastValues.size();
            int index = 0;
            for (auto *p : slot.effect->getParameters())
            {
                auto value = slot.overlayActive[(size_t)index] ? slot.manualValues[(size_t)index].load()
                                                               : p->getValue();
                values.push_back(value);
                observedControlValues[i][index++] = value;
            }
            lastValues.push_back(std::move(values));
            observedPrograms.push_back(slot.effect->getCurrentProgram());
        }
    }
    bool idle() const
    {
        const auto requested = exchange.lastRequestTicks.load(std::memory_order_acquire);
        return exchange.lease.load() == 0 || requested == 0 ||
               Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks() - requested) > .1;
    }
    void settleStopped()
    {
        // A graph may retain its exclusive lease at natural EOF. A zero-length
        // flush serializes with resumed production but never advances history.
        if (idle())
        {
            onPluginMessageThread(
                [&]
                {
                    WriteScope own(writing);
                    for (auto &slot : slots)
                    {
                        // AU setters are synchronous. AudioUnitRender with zero frames
                        // can cache this timestamp and suppress the next real render.
                        if (!slot.needsParameterFlush)
                            continue;
                        const ScopedLock lock(slot.effect->getCallbackLock());
                        juce::AudioBuffer<float> zero(slot.effect->getTotalNumInputChannels(), 0);
                        MidiBuffer midi;
                        slot.effect->processBlock(zero, midi);
                    }
                });
        }
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
    struct BaseCapture
    {
        Slot &slot;
        bool active = false;
        explicit BaseCapture(Slot &s) : slot(s)
        {
            for (int p = 0; p < slot.parameterIds.size(); ++p)
                if (slot.overlayActive[(size_t)p])
                {
                    active = true;
                    slot.effect->getParameters()[p]->setValueNotifyingHost(slot.manualValues[(size_t)p]);
                }
            flush();
        }
        void flush()
        {
            if (!active || !slot.needsParameterFlush)
                return;
            juce::AudioBuffer<float> zero(slot.effect->getTotalNumOutputChannels(), 0);
            MidiBuffer midi;
            slot.effect->processBlock(zero, midi);
        }
        ~BaseCapture()
        {
            for (int p = 0; p < slot.parameterIds.size(); ++p)
                if (slot.overlayActive[(size_t)p])
                    slot.effect->getParameters()[p]->setValueNotifyingHost(slot.overlayValues[(size_t)p]);
            flush();
        }
    };
    std::map<String, std::unique_ptr<Window>> windows;
    bool publish(bool checkpoint)
    {
        // Explicit test-only barrier: hold publication while continuing native
        // and Main control adoption, so ordering/command races are repeatable.
        auto hold = SystemStats::getEnvironmentVariable("SOUNDPLUS_LIVE_TEST_HOLD_CAPTURE", {});
        if (hold.isNotEmpty() && File(hold).existsAsFile())
            return false;
        settleStopped();
        bool coherent = true, opaqueObservation = false;
        std::vector<String> hashes;
        var record = obj();
        Array<var> captures;
        onPluginMessageThread(
            [&]
            {
                if (!idle() &&
                    processBegin.load() <= std::max(modelAppliedTicks.load(), nativeEventTicks.load()))
                {
                    coherent = false;
                    return;
                }
                for (size_t i = 0; i < slots.size(); ++i)
                {
                    auto settings = JSON::parse(requested.slots[i].settings);
                    const ScopedLock lock(slots[i].effect->getCallbackLock());
                    check(slots[i].effect->getParameters().size() == slots[i].parameterIds.size(),
                          "Live parameter schema changed; reprepare required");
                    for (int p = 0; p < slots[i].parameterIds.size(); ++p)
                        check(slots[i].effect->getHostedParameter(p)->getParameterID() ==
                                  slots[i].parameterIds[p],
                              "Live parameter identity changed; reprepare required");
                    WriteScope host(writing);
                    BaseCapture base(slots[i]);
                    auto state = describe(*slots[i].effect);
                    for (auto key :
                         {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
                        put(settings, key, state[key]);
                    // Descriptions expose a bounded subset, not a dense native array.
                    // Keep the complete native-indexed seed, including omitted aliases.
                    std::vector<float> seed;
                    for (auto *parameter : slots[i].effect->getParameters())
                        seed.push_back(parameter->getValue());
                    for (auto &p : *state["parameters"].getArray())
                    {
                        const int index = (int)p["index"];
                        check(isPositiveAndBelow(index, seed.size()) &&
                                  slots[i].effect->getHostedParameter(index)->getParameterID() ==
                                      p["stableId"].toString(),
                              "Captured native parameter identity changed");
                        seed[(size_t)index] = (float)p["value"];
                    }
                    auto hash = state["stateHash"].toString();
                    hashes.push_back(hash);
                    if (hasPublished && i < publishedHashes.size() && hash != publishedHashes[i] &&
                        lastModel == publishedModel && nativeSequence == publishedNative)
                    {
                        ++nativeSequence;
                        opaqueObservation = true; // A snapshot is not a new user gesture.
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
                bool visible = false;
                for (auto &[id, window] : windows)
                    visible |= window->isVisible();
                put(record, "editorVisible", visible);
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
        put(record, "skippedControls", skippedControls);
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
        for (auto &slot : slots)
        {
            auto count = slot.parameterIds.size();
            nativeParameterTicks.push_back(std::make_unique<std::atomic<int64_t>[]>((size_t)count));
            observedControlValues.push_back(std::make_unique<std::atomic<float>[]>((size_t)count));
        }
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
                rememberValues();
                for (auto &slot : slots)
                    slot.effect->addListener(this);
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
    template <class F> void automationWrite(F &&apply)
    {
        WriteScope own(writing);
        apply();
    }
    std::mutex &operationMutex()
    {
        return dspOperationMutex;
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
                    // Serialize the entire DSP/dry-ring/request completion, not
                    // merely the plugin's callback. A new borrower drains first.
                    std::lock_guard operation(dspOperationMutex);
                    for (auto &slot : slots)
                    {
                        if (requested.instrument)
                        {
                            MidiBuffer panic;
                            for (int ch = 1; ch <= 16; ++ch)
                            {
                                panic.addEvent(MidiMessage::controllerEvent(ch, 64, 0), 0);
                                panic.addEvent(MidiMessage::allSoundOff(ch), 0);
                                panic.addEvent(MidiMessage::allNotesOff(ch), 0);
                            }
                            juce::AudioBuffer<float> silence(slot.effect->getTotalNumOutputChannels(), 1);
                            silence.clear();
                            slot.effect->processBlock(silence, panic);
                        }
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
            pendingCapture = true;
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
            skippedControls.clear();
            const auto origin = (int64_t)command["eventTicks"];
            onPluginMessageThread(
                [&]
                {
                    WriteScope own(writing);
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
                        bool restored = false, applied = false;
                        if ((bool)command["changes"][(int)i]["fullState"] &&
                            restoredSequence[i] != (int64_t)command["changes"][(int)i]["restoreSequence"])
                        {
                            restoredSequence[i] = (int64_t)command["changes"][(int)i]["restoreSequence"];
                            const auto restoreOrigin =
                                command["changes"][(int)i].hasProperty("restoreTicks")
                                    ? (int64_t)command["changes"][(int)i]["restoreTicks"]
                                    : origin;
                            if (nativeAnyTicks.load() > restoreOrigin)
                            {
                                skippedControls.add(-1);
                            }
                            else
                            {
                                restore(*slot.effect, after, &slot.opaqueValues);
                                for (int p = 0; p < slot.parameterIds.size(); ++p)
                                {
                                    slot.manualValues[(size_t)p] =
                                        slot.effect->getParameters()[p]->getValue();
                                    if (slot.overlayActive[(size_t)p])
                                        slot.effect->getParameters()[p]->setValueNotifyingHost(
                                            slot.overlayValues[(size_t)p]);
                                }
                                restored = true;
                                applied = true;
                                validateRequestedIdentity(*slot.effect, after);
                                std::fill(slot.baselineRequests.begin(), slot.baselineRequests.end(),
                                          std::numeric_limits<float>::quiet_NaN());
                                if (auto *baseline = after["parameterBaseline"].getArray())
                                    for (auto &p : *baseline)
                                        slot.baselineRequests.at((size_t)(int)p["index"]) = (float)p["value"];
                            }
                        }
                        if (!restored)
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
                                    const auto origins = command["changes"][(int)i]["origins"];
                                    const auto parameterOrigin =
                                        origins.hasProperty(String(index))
                                            ? (int64_t)origins[Identifier(String(index))]
                                            : origin;
                                    if (nativeStateTicks.load() > parameterOrigin ||
                                        nativeParameterTicks[i][index].load() > parameterOrigin)
                                    {
                                        skippedControls.add(index);
                                        continue;
                                    }
                                    float x = (float)value["value"];
                                    float effective = x == slot.baselineRequests[(size_t)index]
                                                          ? slot.opaqueValues[(size_t)index]
                                                          : x;
                                    slot.manualValues[(size_t)index] = effective;
                                    slot.effect->getParameters()[index]->setValueNotifyingHost(
                                        slot.overlayActive[(size_t)index]
                                            ? slot.overlayValues[(size_t)index].load()
                                            : effective);
                                    observedControlValues[i][index] =
                                        slot.effect->getParameters()[index]->getValue();
                                    applied = true;
                                }
                            }
                        }
                        const bool bypassChanged = *slot.bypass != (next.bypass || next.slots[i].bypass);
                        *slot.bypass = next.bypass || next.slots[i].bypass;
                        if (applied || bypassChanged)
                        {
                            modelSourceTicks = origin;
                            modelAppliedTicks = Time::getHighResolutionTicks();
                        }
                    }
                });
            settleStopped();
            onPluginMessageThread([&] { rememberValues(); });
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
        bool changed = false, programChanged = false;
        onPluginMessageThread(
            [&]
            {
                for (size_t i = 0; i < slots.size(); ++i)
                {
                    auto &effect = *slots[i].effect;
                    const auto program = effect.getCurrentProgram();
                    if (program != observedPrograms[i])
                    {
                        programChanged = true;
                        observedPrograms[i] = program;
                    }
                    check(effect.getParameters().size() == (int)lastValues[i].size() &&
                              effect.getParameters().size() == slots[i].parameterIds.size(),
                          "Live schema changed; reprepare required");
                    for (int p = 0; p < effect.getParameters().size(); ++p)
                        check(effect.getHostedParameter(p)->getParameterID() == slots[i].parameterIds[p],
                              "Live parameter identity changed; reprepare required");
                    for (int p = 0; p < effect.getParameters().size(); ++p)
                        if ((slots[i].overlayActive[(size_t)p]
                                 ? slots[i].manualValues[(size_t)p].load()
                                 : effect.getParameters()[p]->getValue()) != lastValues[i][(size_t)p])
                            changed = true;
                }
            });
        const bool opaqueChanged = opaqueBoundary.exchange(false);
        if (programChanged || opaqueChanged)
        {
            ++gestureSequence;
            if (!nativeDirty.load())
                nativeEventTicks = Time::getHighResolutionTicks();
            if (programChanged)
            {
                nativeStateTicks = nativeEventTicks.load();
                nativeAnyTicks = nativeEventTicks.load();
            }
            nativeDirty = true;
        }
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
        capture = pendingCapture && (idle() || processBegin.load() > appliedTicks);
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
