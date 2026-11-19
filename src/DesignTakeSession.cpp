#include "DesignSession.h"
#include "TakeCapture.h"
#include "DesignLive.h"
#include "Performance.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
namespace
{
bool matchingTakeOutput(const PerformTake &t, const DesignOutput &o)
{
    if (!t.complete || o.kind != "take-render" || o.recipeHash != t.recipeHash ||
        o.eventHash != t.eventHash || o.processing != t.processing || o.bodyEnd != t.bodyEnd ||
        o.capturedFrames != t.outputFrames || o.projectRate != t.sampleRate ||
        o.captureRate != t.captureRate || o.sha256.length() != 64)
        return false;
    if (!t.recipe->layout.id.isEmpty() &&
        (o.channelProvenance.isVoid() || layoutFromJson(o.channelProvenance["design"]) != t.recipe->layout))
        return false;
    if (!t.recipe->layout.id.isEmpty())
    {
        int policy = o.channelProvenance.hasProperty("spatialRoutingVersion")
                         ? (int)o.channelProvenance["spatialRoutingVersion"]
                         : 1;
        if (policy != t.recipe->routingVersion)
            return false;
    }
    auto *sources = o.sourceProvenance.getArray();
    if (!sources || sources->size() != (int)t.sources.size())
        return false;
    for (auto &s : t.sources)
    {
        if (std::count_if(sources->begin(), sources->end(), [&](const auto &p)
                          { return p["id"].toString() == s.id && p["sha256"].toString() == s.sha256; }) != 1)
            return false;
        auto expected = poolOriginsForSource(t, s.id);
        if (!expected.isVoid())
            for (auto &p : *sources)
                if (p["id"].toString() == s.id &&
                    juce::JSON::toString(p["poolOrigins"], true) != juce::JSON::toString(expected, true))
                    return false;
    }
    return true;
}
} // namespace
const DesignMix &DesignSession::performanceMix() const
{
    return takeBusy() || document.activeTake.isNotEmpty() ? takeDisplay : document.activeMix();
}
void DesignSession::recordTake()
{
    if (takeBusy() || replacing || working || rendering || document.activeMix().layers.empty())
        return;
    if (document.takes.size() >= maxDocumentTakes)
    {
        message = "Remove a take before recording another (16 maximum)";
        notify();
        return;
    }
    if (std::any_of(document.activeMix().layers.begin(), document.activeMix().layers.end(),
                    [](auto &l) { return l.routing.needsSelection; }))
    {
        message = "Choose supported source channels in Routing before recording";
        notify();
        return;
    }
    endGesture();
    invalidatePoolWork();
    document.activeTake.clear();
    stop();
    if (!document.activeMix().layout.id.isEmpty() && document.activeMix().routingVersion == 1)
        edit("Update Live spatial processing", [](auto &d) { upgradeLiveSpatial(d); });
    if (onBeforePlay)
        onBeforePlay();
    // Reserve while worker-side source verification is still pending, before
    // AudioClient has issued the record generation to the helper.
    services.reserveTake(true);
    protectStartupEdits();
    takeUndoBase = History{document, selectedIds, "Record take"};
    takeState = TakeState::arming;
    captureId = juce::Uuid().toString();
    takeStartedAt = services.now();
    takeReadPending = false;
    expectedGeneration = 0;
    takeCheckpoint = directory.getChildFile("designer-takes").getChildFile(captureId + ".soundplus");
    takeDisplay = document.activeMix();
    lastOutput = juce::File();
    message = "Arming take: verifying sources and fresh effect history";
    auto token = stamp(Operation::take);
    auto d = document;
    auto id = captureId;
    auto file = takeCheckpoint;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, d, id, file, resolve = services.resolve]() mutable
            {
                resolve(d);
                auto t = freezeTake(d, "Take " + juce::String(d.takes.size() + 1));
                if (!t.recipe->layout.id.isEmpty())
                {
                    TakeEvent worst;
                    worst.frame = (int64_t)(d.sampleRate * maxTakeSeconds);
                    worst.generation = UINT64_MAX;
                    worst.sequence = maxTakeEvents;
                    worst.controls = mixControls(t.recipe);
                    auto event = juce::var(new juce::DynamicObject());
                    auto *p = event.getDynamicObject();
                    p->setProperty("frame", (juce::int64)worst.frame);
                    p->setProperty("sequence", (juce::int64)worst.sequence);
                    p->setProperty("generation", (juce::int64)worst.generation);
                    p->setProperty("controls", controlsToJson(worst.controls));
                    auto existing = juce::JSON::toString(designToJson(d), true).getNumBytesAsUTF8();
                    auto eventBytes = juce::JSON::toString(event, true).getNumBytesAsUTF8() + 256;
                    if (existing + eventBytes * maxTakeEvents + 65536 > 60 * 1024 * 1024)
                        throw std::runtime_error("Recording could exceed the64MiB document budget; "
                                                 "archive/remove earlier takes first");
                }
                t.id = id;
                t.checkpoint = file.getFullPathName();
                d.takes.push_back(t);
                d.activeTake.clear();
                saveDesignAtomic(file, d);
                return [safe, d, t, file]
                {
                    if (!safe)
                        return;
                    safe->takeMutation = true;
                    safe->edit("Record take draft", [&](auto &document) { document.takes.push_back(t); });
                    safe->takeMutation = false;
                    if (!safe->undoHistory.empty() && safe->undoHistory.back().label == "Record take draft")
                        safe->undoHistory.pop_back();
                    safe->takeDisplay = takeDisplayMix(t);
                    safe->expectedGeneration =
                        safe->services.play("design:" + safe->document.id, file, 0, "record");
                    if (!safe->expectedGeneration)
                    {
                        safe->services.reserveTake(false);
                        safe->takeState = TakeState::incomplete;
                        safe->message = "Recording could not acquire audio transport";
                        return;
                    }
                    safe->active = true;
                    safe->publishedStructure = designStructure(d);
                    safe->previewDue = 0;
                };
            });
    notify();
}
void DesignSession::stopTake(bool cancel)
{
    if (!takeBusy())
        return;
    if (cancel || takeState == TakeState::arming)
    {
        services.reserveTake(false);
        stamp(Operation::take);
        ++*previewSerial;
        if (services.finishTake)
            services.finishTake(true);
        auto id = captureId;
        takeState = TakeState::idle;
        active = false;
        takeMutation = true;
        edit("Cancel take",
             [&](auto &d)
             {
                 std::erase_if(d.takes, [&](auto &t) { return t.id == id; });
                 d.activeTake.clear();
             });
        takeMutation = false;
        if (!undoHistory.empty() && undoHistory.back().label == "Cancel take")
            undoHistory.pop_back();
        takeUndoBase.reset();
        captureId.clear();
        takeGestureStart.reset();
        selectContextOutput();
        message = "Take cancelled; editable controls restored";
        notify();
        return;
    }
    if (takeState == TakeState::recording)
    {
        takeState = TakeState::finalizing;
        takeGestureStart.reset();
        if (services.finishTake)
            services.finishTake(false);
        message = "Finalizing consumed take frames";
        notify();
    }
}
void DesignSession::selectContextOutput()
{
    lastOutput = juce::File();
    auto *take = selectedTake(document);
    auto preference = document.outputSelections.find(document.activeTake.isEmpty() ? juce::String("live")
                                                                                   : document.activeTake);
    if (preference != document.outputSelections.end())
    {
        for (auto &o : document.outputs)
            if (o.path == preference->second && o.takeId == document.activeTake &&
                (!take || matchingTakeOutput(*take, o)) && !outputFiles(o).isEmpty())
                lastOutput = juce::File(o.path);
        // An explicitly selected set with a missing member is unavailable;
        // never silently deliver another finalized output in its place.
        return;
    }
    for (auto it = document.outputs.rbegin(); it != document.outputs.rend(); ++it)
        if (it->takeId == document.activeTake && (!take || matchingTakeOutput(*take, *it)) &&
            !outputFiles(*it).isEmpty())
        {
            lastOutput = juce::File(it->path);
            break;
        }
}
juce::File DesignSession::readyOutput()
{
    selectContextOutput();
    return lastOutput;
}
juce::StringArray DesignSession::readyFiles()
{
    auto file = readyOutput();
    for (auto &o : document.outputs)
        if (o.path == file.getFullPathName())
            return outputFiles(o);
    return {};
}
void DesignSession::chooseOutput(const juce::String &path)
{
    editMetadata("Ready output selection",
                 [&](auto &d) {
                     d.outputSelections[d.activeTake.isEmpty() ? juce::String("live") : d.activeTake] = path;
                 });
}
void DesignSession::selectTake(const juce::String &id)
{
    invalidatePoolWork();
    if (takeBusy())
    {
        message = "Stop recording before changing context";
        notify();
        return;
    }
    if (id.isNotEmpty() &&
        std::none_of(document.takes.begin(), document.takes.end(), [&](auto &t) { return t.id == id; }))
        return;
    stop();
    document.activeTake = id;
    document.playhead = 0;
    if (auto *t = selectedTake(document))
        takeDisplay = takeDisplayMix(*t);
    takeState = id.isEmpty()                       ? TakeState::idle
                : selectedTake(document)->complete ? TakeState::ready
                                                   : TakeState::incomplete;
    selectContextOutput();
    notify();
}
void DesignSession::nameTake(const juce::String &name, bool keep)
{
    if (takeBusy() || !selectedTake(document) || name.trim().isEmpty())
        return;
    auto id = document.activeTake;
    takeMutation = true;
    edit(
        "Name / keep take",
        [&](auto &d)
        {
            for (auto &t : d.takes)
                if (t.id == id)
                {
                    t.name = name.trim().substring(0, 120);
                    t.kept = keep;
                }
        },
        ChangeImpact::metadata);
    takeMutation = false;
}
void DesignSession::deleteTake()
{
    if (takeBusy() || !selectedTake(document))
        return;
    auto id = document.activeTake;
    stop();
    takeMutation = true;
    edit("Delete take",
         [&](auto &d)
         {
             std::erase_if(d.takes, [&](auto &t) { return t.id == id; });
             d.outputSelections.erase(id);
             d.activeTake.clear();
         });
    takeMutation = false;
    takeState = TakeState::idle;
    selectContextOutput();
}
void DesignSession::createCorrectedSpatialCopy()
{
    auto *original = selectedTake(document);
    if (!original || !original->complete || original->processing != "stock-layout-takes-v1" || takeBusy() ||
        working || replacing || rendering)
        return;
    if (document.takes.size() >= maxDocumentTakes)
    {
        message = "Remove a take before creating a corrected copy (16 maximum)";
        notify();
        return;
    }
    stop();
    working = true;
    auto token = stamp(Operation::take);
    auto snapshot = *original;
    auto documentId = document.id;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, snapshot, documentId]
            {
                auto copy = correctedSpatialCopy(snapshot);
                return [safe, copy = std::move(copy), snapshot, documentId]
                {
                    if (!safe)
                        return;
                    safe->working = false;
                    auto *current = selectedTake(safe->document);
                    if (safe->document.id != documentId || !current || current->id != snapshot.id ||
                        current->recipeHash != snapshot.recipeHash ||
                        current->eventHash != snapshot.eventHash ||
                        safe->document.takes.size() >= maxDocumentTakes)
                    {
                        safe->message = "Corrected copy cancelled: take context changed";
                        safe->notify();
                        return;
                    }
                    safe->takeMutation = true;
                    safe->edit("Create corrected spatial copy",
                               [&](auto &d)
                               {
                                   d.takes.push_back(copy);
                                   d.activeTake = copy.id;
                               });
                    safe->takeMutation = false;
                    safe->takeDisplay = takeDisplayMix(copy);
                    safe->takeState = TakeState::ready;
                    safe->selectContextOutput();
                    safe->message = "Corrected copy created; original take and outputs retained";
                    safe->notify();
                };
            });
}
void DesignSession::relinkTakeSource(const juce::String &source, const juce::File &file)
{
    if (takeBusy() || !selectedTake(document))
        return;
    auto t = *selectedTake(document);
    auto id = t.id;
    auto token = stamp(Operation::take);
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, t, source, file, id]
            {
                auto found =
                    std::find_if(t.sources.begin(), t.sources.end(), [&](auto &s) { return s.id == source; });
                auto in = file.createInputStream();
                if (found == t.sources.end() || !in || juce::SHA256(*in).toHexString() != found->sha256)
                    throw std::runtime_error("Relink rejected: source content fingerprint differs");
                auto inspected = inspectDesignSource(file);
                return [safe, id, source, file, inspected]
                {
                    if (!safe)
                        return;
                    safe->takeMutation = true;
                    safe->edit("Relink identical take source",
                               [&](auto &d)
                               {
                                   for (auto &t : d.takes)
                                       if (t.id == id)
                                           for (auto &s : t.sources)
                                               if (s.id == source)
                                               {
                                                   s.overridePath = file.getFullPathName();
                                                   s.overrideSize = inspected.size;
                                                   s.overrideModified = inspected.modified;
                                                   s.overrideInode = inspected.inode;
                                               }
                               });
                    safe->takeMutation = false;
                };
            });
}
void DesignSession::pollTake(const juce::var &state)
{
    if (takeState == TakeState::arming && expectedGeneration == 0 && (bool)state["captureReserved"] &&
        (bool)state["captureLifecycleFailed"] &&
        state["deviceReservationId"] == state["captureReservationId"])
    {
        stamp(Operation::take); // A held source-verification completion is now stale.
        services.reserveTake(false);
        takeState = TakeState::incomplete;
        takeUndoBase.reset();
        active = false;
        message = "Incomplete take before audio start: " + state["captureDeviceError"].toString();
        notify();
        return;
    }
    if (auto *t = selectedTake(document); t && !takeBusy())
    {
        takeDisplay = takeDisplayMix(*t);
        if (state["id"].toString() == "design:" + document.id && (bool)state["playing"])
        {
            auto heard = (int64_t)state["acceptedSourceEnd"];
            auto next = std::lower_bound(t->events.begin(), t->events.end(), heard,
                                         [](const auto &e, int64_t f) { return e.frame < f; });
            if (next != t->events.begin())
                applyMixControls(takeDisplay, std::prev(next)->controls);
        }
    }
    if (!takeBusy() || takeReadPending)
        return;
    bool matching = state["takeId"].toString() == captureId;
    auto phase = matching ? (int)state["takePhase"] : -1;
    if (matching && phase == TakeCapture::recording && takeState == TakeState::arming)
    {
        takeState = TakeState::recording;
        message = "Recording take";
        notify();
    }
    bool settled = expectedGeneration > 0 && !(bool)state["pendingPlayback"];
    bool failed = (settled && state["error"].toString().isNotEmpty()) ||
                  (matching && phase == TakeCapture::incomplete) ||
                  (expectedGeneration > 0 && state["error"].toString().contains("Cannot launch"));
    bool wrongGeneration = settled && (uint64_t)(int64_t)state["playGeneration"] >= expectedGeneration &&
                           ((uint64_t)(int64_t)state["playGeneration"] != expectedGeneration ||
                            state["id"].toString() != "design:" + document.id);
    failed |= wrongGeneration;
    if (!failed && (!matching || phase != TakeCapture::ready || !(bool)state["takeCheckpointReady"]))
        return;
    takeReadPending = true;
    auto token = stamp(Operation::take);
    auto path = takeCheckpoint;
    auto id = captureId;
    auto generation = expectedGeneration;
    auto fallback = document;
    auto reason =
        state["error"].toString().isNotEmpty() ? state["error"].toString() : state["takeError"].toString();
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, path, id, generation, fallback, failed, reason]() mutable
            {
                PerformTake t;
                bool found = false;
                try
                {
                    auto d = loadDesign(path);
                    for (auto &v : d.takes)
                        if (v.id == id)
                        {
                            t = v;
                            found = true;
                        }
                }
                catch (...)
                {
                }
                if (!found)
                    for (auto &v : fallback.takes)
                        if (v.id == id)
                        {
                            t = v;
                            found = true;
                        }
                if (!found)
                    throw std::runtime_error("Recoverable take draft unavailable");
                if (failed || t.generation != generation)
                {
                    t.complete = false;
                    t.reason = reason.isNotEmpty() ? reason : "Recording interrupted or generation lost";
                    hashTake(t);
                }
                return [safe, t, id]
                {
                    if (!safe)
                        return;
                    safe->active = false;
                    safe->services.reserveTake(false);
                    safe->previewDue = 0;
                    safe->takeState = t.complete ? TakeState::ready : TakeState::incomplete;
                    safe->takeReadPending = false;
                    safe->takeMutation = true;
                    safe->edit("Finalize take",
                               [&](auto &d)
                               {
                                   for (auto &v : d.takes)
                                       if (v.id == id)
                                           v = t;
                                   d.activeTake = id;
                               });
                    safe->takeMutation = false;
                    if (safe->takeUndoBase && !safe->undoHistory.empty() &&
                        safe->undoHistory.back().label == "Finalize take")
                    {
                        safe->undoHistory.back() = std::move(*safe->takeUndoBase);
                        safe->takeUndoBase.reset();
                    }
                    safe->takeDisplay = takeDisplayMix(t);
                    safe->takeGestureStart.reset();
                    safe->selectContextOutput();
                    safe->message = t.complete ? "Take ready - audition, keep/name or Render Take"
                                               : "Incomplete take: " + t.reason;
                    safe->notify();
                };
            });
}
bool recoverTakeDrafts(DesignDocument &document)
{
    bool changed = false;
    for (auto &t : document.takes)
        if (!t.complete && t.checkpoint.isNotEmpty())
            try
            {
                auto d = loadDesign(juce::File(t.checkpoint));
                for (auto &v : d.takes)
                    if (v.id == t.id)
                    {
                        auto kept = t.kept;
                        auto name = t.name;
                        t = v;
                        t.complete = false;
                        t.kept = kept;
                        t.name = name;
                        t.reason = "Recovered interrupted prefix; not a complete take";
                        hashTake(t);
                        changed = true;
                    }
            }
            catch (...)
            {
                t.reason = "Draft checkpoint unavailable; retained starting recipe";
            }
    if (changed)
        ++document.revision;
    return changed;
}
void DesignSession::restoreTakeDrafts()
{
    if (auto *t = selectedTake(document))
    {
        takeDisplay = takeDisplayMix(*t);
        takeState = t->complete ? TakeState::ready : TakeState::incomplete;
    }
}
} // namespace sp
