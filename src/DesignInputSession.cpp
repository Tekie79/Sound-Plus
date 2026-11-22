#include "DesignSession.h"
#include "TrackModel.h"
#include "DesignLive.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
namespace
{
void require(bool b, const char *s)
{
    if (!b)
        throw std::runtime_error(s);
}
InputTrackPlan trackPlan(const DesignTrack &t)
{
    require(t.kind == TrackKind::audio && t.input.has_value(), "Choose an Audio Track input first");
    return {t.id, t.name, t.input->channels, t.input->layout};
}
InputDeviceInfo deviceInfo(const juce::var &v)
{
    InputDeviceInfo d;
    d.uid = v["uid"].toString();
    d.name = v["name"].toString();
    d.rate = (double)v["rate"];
    d.channels = (int)v["channels"];
    d.latency = (int)v["latency"];
    d.safety = (int)v["safety"];
    if (v["layout"].isObject())
        d.reportedLayout = layoutFromJson(v["layout"]);
    return d;
}
} // namespace
bool DesignSession::inputReady() const
{
    auto state = inputStatus();
    if (inputConfigPending || armedInputs.empty() || !(bool)state["open"] ||
        state["error"].toString().isNotEmpty())
        return false;
    for (auto &id : armedInputs)
    {
        auto *t = findTrack(document.activeMix(), id);
        if (!t || !t->input || t->input->deviceUID != state["device"]["uid"].toString())
            return false;
    }
    return true;
}
float DesignSession::inputPeak(const juce::String &id) const
{
    auto *t = findTrack(document.activeMix(), id);
    auto s = inputStatus();
    float peak = 0;
    if (!t || !t->input || s["device"]["uid"].toString() != t->input->deviceUID || !(bool)s["open"])
        return 0;
    for (int c : t->input->channels)
        if (c < s["peaks"].size())
            peak = std::max(peak, (float)s["peaks"][c]);
    return peak;
}
void DesignSession::configureTrackInput(const juce::String &owner, TrackInput input)
{
    if (inputBusy())
        return;
    armInputTrack(owner, false);
    editMetadata("Audio Track input",
                 [&](auto &d)
                 {
                     auto *t = findTrack(d.activeMix(), owner);
                     require(t && t->kind == TrackKind::audio, "Select an Audio Track");
                     t->input = std::move(input);
                 });
}
void DesignSession::armInputTrack(const juce::String &id, bool arm)
{
    if (midiBusy() || inputBusy() || takeBusy() || transitionPending() || document.activeTake.isNotEmpty())
    {
        message = "Stop recording and choose Live Design before changing input arms";
        notify();
        return;
    }
    if (!arm)
    {
        pendingInputArms.erase(id);
        armedInputs.erase(id);
        monitoredInputs.erase(id);
        updateInputDevice();
        notify();
        return;
    }
    auto *t = findTrack(document.activeMix(), id);
    if (!t || t->kind != TrackKind::audio)
        return;
    if (!t->input)
    {
        auto devices = inputStatus()["availableDevices"];
        auto selected = devices[0];
        if (auto *a = devices.getArray())
            for (auto &d : *a)
                if ((bool)d["default"])
                    selected = d;
        if (!selected.isObject())
        {
            message = "No compatible audio inputs found";
            notify();
            return;
        }
        configureTrackInput(
            id, {selected["uid"].toString(), selected["name"].toString(), {0}, channelLayout("mono-v1")});
        t = findTrack(document.activeMix(), id);
    }
    auto uid = t->input->deviceUID;
    auto incarnation = lifetime;
    auto doc = document.id;
    auto variation = document.activeVariation;
    pendingInputArms.insert(id);
    juce::Component::SafePointer<DesignSession> safe(this);
    services.inputPermission(
        [safe, id, uid, incarnation, doc, variation](bool granted)
        {
            if (!safe || safe->lifetime != incarnation || safe->document.id != doc ||
                safe->document.activeVariation != variation || !safe->pendingInputArms.erase(id) ||
                safe->inputBusy())
                return;
            if (!granted)
            {
                safe->message = "Microphone permission denied. Allow SoundPlus in macOS Privacy & Security / "
                                "Microphone, then arm again.";
                safe->notify();
                return;
            }
            auto *t = findTrack(safe->document.activeMix(), id);
            if (!t || !t->input || t->input->deviceUID != uid)
                return;
            for (auto &other : safe->armedInputs)
                if (auto *armed = findTrack(safe->document.activeMix(), other);
                    armed && armed->input && armed->input->deviceUID != uid)
                {
                    safe->message = "Simultaneous tracks must use separate channels of one input device";
                    safe->notify();
                    return;
                }
            if (safe->armedInputs.size() >= 8)
            {
                safe->message = "At most eight armed Audio Tracks";
                safe->notify();
                return;
            }
            safe->inputArmContext = juce::String((juce::int64)safe->lifetime) + ":" + doc + ":" + variation;
            safe->armedInputs.insert(id);
            safe->updateInputDevice();
            safe->notify();
        });
}
void DesignSession::updateInputDevice()
{
    if (inputBusy())
        return;
    try
    {
        std::erase_if(armedInputs,
                      [&](auto &id)
                      {
                          auto *t = findTrack(document.activeMix(), id);
                          return !t || t->kind != TrackKind::audio || !t->input;
                      });
        std::erase_if(monitoredInputs, [&](auto &id) { return !armedInputs.contains(id); });
        auto state = inputStatus();
        juce::String uid;
        if (!armedInputs.empty())
            uid = findTrack(document.activeMix(), *armedInputs.begin())->input->deviceUID;
        if ((uid.isEmpty() && ((bool)state["open"] || inputConfigPending)) ||
            (!uid.isEmpty() && (!(bool)state["open"] || state["device"]["uid"].toString() != uid ||
                                state["error"].toString().isNotEmpty())))
        {
            auto command = sp::message("input_device");
            command.getDynamicObject()->setProperty("uid", uid);
            command.getDynamicObject()->setProperty("serial", (juce::int64)++inputControlSerial);
            inputConfigPending = true;
            services.inputCommand(command);
        }
        auto command = sp::message("input_monitor");
        command.getDynamicObject()->setProperty("monitorSerial", (juce::int64)++inputMonitorControlSerial);
        if (!monitoredInputs.empty() && (bool)state["open"] && state["device"]["uid"].toString() == uid)
        {
            InputRecordingPlan p;
            p.id = juce::Uuid().toString();
            p.document = document.id;
            p.deviceUID = uid;
            p.root = inputRecordingRoot();
            for (auto &id : monitoredInputs)
                p.tracks.push_back(trackPlan(*findTrack(document.activeMix(), id)));
            command.getDynamicObject()->setProperty("plan", inputPlanJson(p));
        }
        services.inputCommand(command);
    }
    catch (const std::exception &e)
    {
        message = e.what();
        armedInputs.clear();
        monitoredInputs.clear();
    }
}
void DesignSession::monitorInputTrack(const juce::String &id, bool enabled)
{
    if (enabled && !inputReady())
    {
        message = "Wait for the armed input device before enabling monitoring";
        notify();
        return;
    }
    if (!armedInputs.contains(id))
    {
        message = "Arm the Audio Track before enabling raw input monitoring";
        notify();
        return;
    }
    if (enabled)
        monitoredInputs.insert(id);
    else
        monitoredInputs.erase(id);
    if (inputBusy())
    {
        // Monitoring is a transient device path and never changes the captured PCM.
        try
        {
            auto command = sp::message("input_monitor");
            command.getDynamicObject()->setProperty("monitorSerial",
                                                    (juce::int64)++inputMonitorControlSerial);
            if (!monitoredInputs.empty())
            {
                InputRecordingPlan p;
                p.id = juce::Uuid().toString();
                p.document = document.id;
                p.root = inputRecordingRoot();
                p.deviceUID = findTrack(document.activeMix(), id)->input->deviceUID;
                for (auto &owner : monitoredInputs)
                    p.tracks.push_back(trackPlan(*findTrack(document.activeMix(), owner)));
                command.getDynamicObject()->setProperty("plan", inputPlanJson(p));
            }
            services.inputCommand(command);
        }
        catch (const std::exception &e)
        {
            message = e.what();
            monitoredInputs.clear();
        }
    }
    else
        updateInputDevice();
    notify();
}
void DesignSession::startInputRecording()
{
    if (midiBusy() || inputBusy() || takeBusy() || replacing || rendering || working ||
        document.activeTake.isNotEmpty())
        return;
    if (!inputReady())
    {
        message = "Arm an Audio Track and wait for its input device";
        notify();
        return;
    }
    if (flushLive(
            [safe = juce::Component::SafePointer<DesignSession>(this)]
            {
                if (safe)
                    safe->startInputRecording();
            }))
        return;
    stop();
    endGesture();
    InputRecordingPlan p;
    p.id = juce::Uuid().toString();
    p.document = document.id;
    p.variation = document.activeVariation;
    p.root = inputRecordingRoot();
    p.projectRate = document.sampleRate;
    p.start = document.playhead;
    p.deviceUID = inputStatus()["device"]["uid"].toString();
    for (auto &id : armedInputs)
        p.tracks.push_back(trackPlan(*findTrack(document.activeMix(), id)));
    try
    {
        validateInputPlan(p, deviceInfo(inputStatus()["device"]));
        require(document.activeMix().layers.size() + p.tracks.size() <= 64,
                "Recording would exceed the64-clip limit");
    }
    catch (const std::exception &e)
    {
        message = e.what();
        notify();
        return;
    }
    inputOperation = p.id;
    inputDocument = document.id;
    inputVariation = document.activeVariation;
    inputIncarnation = lifetime;
    inputProcessGeneration = (uint64_t)(int64_t)services.status()["processGeneration"];
    inputStopRequested = inputCancelRequested = inputReadPending = false;
    inputLastPhase = "preparing";
    message = "Preparing audio input";
    active = previewWaiting = true;
    previewDue = 0;
    auto snapshot = document;
    snapshot.activeMix().duration = (int64_t)std::llround(document.sampleRate * 3600);
    auditionContext = designPlaybackContext(document);
    auto file = directory.getChildFile("designer-preview").getChildFile("input-" + p.id + ".soundplus");
    auto token = stamp(Operation::input);
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, snapshot, p, file, resolve = services.resolve]() mutable
            {
                resolve(snapshot);
                saveDesignAtomic(file, snapshot);
                return [safe, p, file, snapshot]() mutable
                {
                    if (!safe || safe->inputOperation != p.id)
                        return;
                    if (safe->inputStopRequested)
                    {
                        safe->message = "Input recording cancelled before it started";
                        safe->finishInputContext();
                        return;
                    }
                    try
                    {
                        p.generation =
                            safe->services.play("design:" + p.document, file, p.start, "input-record");
                        require(p.generation != 0, "Audio transport did not accept input recording");
                        safe->expectedGeneration = p.generation;
                        safe->publishedStructure = designStructure(snapshot);
                        safe->previewWaiting = false;
                        auto command = sp::message("input_record");
                        command.getDynamicObject()->setProperty("plan", inputPlanJson(p));
                        safe->services.inputCommand(command);
                        safe->message = "Preparing audio input";
                    }
                    catch (const std::exception &e)
                    {
                        safe->message = e.what();
                        safe->finishInputContext();
                    }
                };
            });
    notify();
}
void DesignSession::stopInputRecording(bool cancel)
{
    if (!inputBusy())
        return;
    inputStopRequested = true;
    inputCancelRequested = cancel;
    active = false;
    if (!previewWaiting)
        try
        {
            auto command = sp::message("input_finish");
            command.getDynamicObject()->setProperty("id", inputOperation);
            command.getDynamicObject()->setProperty("cancel", cancel);
            services.inputCommand(command);
        }
        catch (const std::exception &e)
        {
            message = e.what();
            notify();
            return;
        }
    message = cancel ? "Cancelling audio recording; retained PCM will remain recoverable"
                     : "Finalizing recorded audio";
    notify();
}
void DesignSession::finishInputContext()
{
    inputOperation.clear();
    inputReadPending = false;
    inputStopRequested = inputCancelRequested = false;
    active = previewWaiting = false;
    previewDue = 0;
    stamp(Operation::input);
    services.stop();
    if (inputClose)
    {
        auto close = std::move(inputClose);
        inputClose = {};
        closeWhenSafe(std::move(close));
    }
}
void DesignSession::pollInput(const juce::var &state)
{
    auto context = juce::String((juce::int64)lifetime) + ":" + document.id + ":" + document.activeVariation;
    if (!inputBusy() && !armedInputs.empty() && inputArmContext != context)
    {
        armedInputs.clear();
        monitoredInputs.clear();
        pendingInputArms.clear();
        updateInputDevice();
    }
    auto input = state["inputRecording"];
    if (inputConfigPending && (uint64_t)(int64_t)input["configurationSerial"] == inputControlSerial)
        inputConfigPending = false;
    if (!monitoredInputs.empty() &&
        (!(bool)input["open"] ||
         ((uint64_t)(int64_t)input["monitorSerial"] == inputMonitorControlSerial &&
          (!(bool)input["monitoring"] || input["monitorError"].toString().isNotEmpty()))))
    {
        message = input["monitorError"].toString().isNotEmpty() ? input["monitorError"].toString()
                                                                : "Raw input monitoring turned off";
        monitoredInputs.clear();
        notify();
    }
    if (!inputBusy())
    {
        if (!inputConfigPending && input["error"].toString().isNotEmpty() && !armedInputs.empty())
        {
            message = input["error"].toString();
            armedInputs.clear();
            monitoredInputs.clear();
            notify();
        }
        return;
    }
    if (inputReadPending)
        return;
    bool lost = (!state["processGeneration"].isVoid() &&
                 (uint64_t)(int64_t)state["processGeneration"] != inputProcessGeneration);
    if (lost || (input.isVoid() && state["error"].toString().isNotEmpty()))
    {
        message = "Input helper interrupted; source PCM is retained for recovery";
        armedInputs.clear();
        monitoredInputs.clear();
        finishInputContext();
        notify();
        return;
    }
    if (!(bool)input["busy"] && input["request"].toString() == inputOperation &&
        input["error"].toString().isNotEmpty())
    {
        message = input["error"].toString();
        finishInputContext();
        notify();
        return;
    }
    if (input["id"].toString() != inputOperation)
        return;
    auto phase = input["state"].toString();
    if (phase != inputLastPhase)
    {
        inputLastPhase = phase;
        if (phase == "recording" && !inputStopRequested)
        {
            message = "Recording audio";
            notify();
        }
        if (phase == "finalizing")
        {
            message = "Finalizing recorded audio";
            notify();
        }
    }
    if (phase == "complete")
        applyInputReceipt(input);
    else if (phase == "incomplete" || phase == "cancelled")
    {
        message = phase == "cancelled" ? "Input recording cancelled; source prefix retained"
                                       : input["reason"].toString();
        finishInputContext();
        notify();
    }
}
void DesignSession::applyInputReceipt(juce::var receipt)
{
    inputReadPending = true;
    auto id = inputOperation;
    auto ownerDocument = inputDocument;
    auto variation = inputVariation;
    auto incarnation = inputIncarnation;
    auto root = inputRecordingRoot();
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(stamp(Operation::input),
            [safe, receipt, id, ownerDocument, variation, incarnation, root]() mutable
            {
                auto file = juce::File(receipt["receipt"].toString());
                require(file == root.getChildFile(id).getChildFile("receipt.json") && file.existsAsFile(),
                        "Recording receipt missing or outside durable source storage");
                auto retained = juce::JSON::parse(file);
                require(((int)retained["version"] == 1 || (int)retained["version"] == 2) &&
                            (retained["state"].toString() == "complete" ||
                             retained["state"].toString() == "recovered") &&
                            retained["plan"]["id"].toString() == id,
                        "Recording is not finalized");
                auto plan = inputPlanFromJson(retained["plan"]);
                require(plan.document == ownerDocument && plan.variation == variation,
                        "Recording belongs to another document/variation; its files are retained");
                auto *a = retained["outputs"].getArray();
                require(a && a->size() == (int)plan.tracks.size(), "Recording set is incomplete");
                std::vector<DesignLayer> layers;
                for (auto &v : *a)
                {
                    auto source = juce::File(v["path"].toString());
                    require(source.isAChildOf(root.getChildFile(id)) && source.existsAsFile(),
                            "Recorded source missing");
                    require(juce::SHA256(source).toHexString() == v["sha256"].toString(),
                            "Recorded source checksum mismatch");
                    auto asset = inspectDesignSource(source);
                    require(asset.samples == (int64_t)v["frames"] && asset.sampleRate == (double)v["rate"] &&
                                asset.channels == (int)v["layout"]["roles"].size(),
                            "Recorded source header mismatch");
                    auto layout = layoutFromJson(v["layout"]);
                    asset.detectedChannels = layout;
                    asset.layoutConfidence = "recorded-explicit";
                    auto layer = makeDesignLayer(asset, 0, -1, true);
                    layer.id = "record-" + id + "-" + v["owner"].toString();
                    layer.name = ((bool)retained["recovered"]
                                      ? juce::String::fromUTF8(retained["prefixIntegrity"].toString() ==
                                                                       "legacy-unverified"
                                                                   ? "Recovered (unverified) · "
                                                                   : "Recovered · ")
                                      : juce::String()) +
                                 v["name"].toString();
                    layer.trackId = v["owner"].toString();
                    layer.placement = (int64_t)retained["placement"];
                    layer.recordingOrigin = juce::var(new juce::DynamicObject);
                    auto *o = layer.recordingOrigin.getDynamicObject();
                    o->setProperty("version", 1);
                    o->setProperty("id", id);
                    o->setProperty("receipt", file.getFullPathName());
                    o->setProperty("recovered", retained["recovered"]);
                    layers.push_back(std::move(layer));
                }
                return [safe, layers = std::move(layers), id, ownerDocument, variation, incarnation,
                        recovered = (bool)retained["recovered"]]() mutable
                {
                    if (!safe || safe->inputOperation != id)
                        return;
                    if (safe->lifetime != incarnation || safe->document.id != ownerDocument ||
                        safe->document.activeVariation != variation)
                    {
                        safe->message = "Recording retained; its original design context changed";
                        safe->finishInputContext();
                        return;
                    }
                    bool inserted = false;
                    {
                        juce::ScopedValueSetter<bool> guard(safe->inputMutation, true);
                        safe->edit("Record Audio Tracks",
                                   [&](auto &d)
                                   {
                                       require(d.activeMix().layers.size() + layers.size() <= 64,
                                               "Recording sources retained; timeline clip limit reached");
                                       for (auto &l : layers)
                                       {
                                           auto *t = findTrack(d.activeMix(), l.trackId);
                                           require(t && t->kind == TrackKind::audio,
                                                   "Recording owner unavailable; source files retained");
                                           if (findClip(d.activeMix(), l.id))
                                               continue;
                                           initialiseRouting(l, d.activeMix().layout.id.isEmpty()
                                                                    ? channelLayout("stereo-v1")
                                                                    : t->layout);
                                           d.selectedLayer = l.id;
                                           d.selectedTrack = t->id;
                                           d.activeMix().layers.push_back(l);
                                       }
                                       inserted = true;
                                   });
                    }
                    if (inserted)
                        safe->message = recovered ? "Recovered partial audio added; source files retained"
                                                  : "Recorded audio added; Undo keeps the source files";
                    safe->finishInputContext();
                    safe->notify();
                };
            });
}
void DesignSession::findInputRecoveries(std::function<void(juce::Array<juce::var>)> done)
{
    std::set<juce::String> used;
    for (auto &l : document.activeMix().layers)
        if (!l.recordingOrigin.isVoid())
            used.insert(l.recordingOrigin["id"].toString());
    auto root = inputRecordingRoot();
    auto doc = document.id;
    auto variation = document.activeVariation;
    enqueue(stamp(Operation::inputRecoveryList),
            [root, used, doc, variation, done = std::move(done)]
            {
                auto results = InputRecordingStore::recoverable(root);
                for (int i = results.size(); --i >= 0;)
                    if (used.contains(results[i]["plan"]["id"].toString()) ||
                        results[i]["plan"]["document"].toString() != doc ||
                        results[i]["plan"]["variation"].toString() != variation)
                        results.remove(i);
                return [done, results] { done(results); };
            });
}
void DesignSession::recoverInputRecording(const juce::File &partial, bool allowUnverifiedLegacy)
{
    if (midiBusy() || inputBusy() || takeBusy() || replacing || document.activeTake.isNotEmpty())
        return;
    if (partial.getParentDirectory() != inputRecordingRoot())
    {
        message = "Recording recovery folder is outside this catalog's retained source storage";
        notify();
        return;
    }
    auto leaf = partial.getFileName();
    inputOperation =
        leaf.startsWithChar('.') ? leaf.substring(1).upToLastOccurrenceOf(".partial", false, false) : leaf;
    inputDocument = document.id;
    inputVariation = document.activeVariation;
    inputIncarnation = lifetime;
    inputProcessGeneration = (uint64_t)(int64_t)services.status()["processGeneration"];
    inputReadPending = true;
    auto id = inputOperation;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(stamp(Operation::input),
            [safe, partial, id, allowUnverifiedLegacy]
            {
                auto receipt = InputRecordingStore::recover(partial, allowUnverifiedLegacy);
                return [safe, receipt, id]
                {
                    if (safe && safe->inputOperation == id)
                    {
                        safe->inputReadPending = false;
                        safe->applyInputReceipt(receipt);
                    }
                };
            });
}
} // namespace sp
