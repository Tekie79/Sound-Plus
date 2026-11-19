#include "DesignSession.h"
#include "PerformanceState.h"
#include "DesignLive.h"
#include "Performance.h"
#include "SourcePools.h"
#include <cmath>

namespace sp
{
namespace
{
DesignLayer *layerById(DesignDocument &document, const juce::String &id)
{
    for (auto &layer : document.activeMix().layers)
        if (layer.id == id)
            return &layer;
    return nullptr;
}
bool sameTakes(const DesignDocument &a, const DesignDocument &b)
{
    if (a.takes.size() != b.takes.size() || a.activeTake != b.activeTake)
        return false;
    for (size_t i = 0; i < a.takes.size(); ++i)
    {
        auto &x = a.takes[i];
        auto &y = b.takes[i];
        if (x.recipe.identity() != y.recipe.identity() || x.events.identity() != y.events.identity())
            return false;
        // Metadata is small; immutable recipe/events need not be serialized for
        // every live control edit or copied into every Undo snapshot.
        auto header = [](const PerformTake &t)
        {
            auto o = juce::var(new juce::DynamicObject());
            auto *v = o.getDynamicObject();
            v->setProperty("id", t.id);
            v->setProperty("name", t.name);
            v->setProperty("session", t.session);
            v->setProperty("reason", t.reason);
            v->setProperty("recipeHash", t.recipeHash);
            v->setProperty("eventHash", t.eventHash);
            v->setProperty("checkpoint", t.checkpoint);
            v->setProperty("kept", t.kept);
            v->setProperty("complete", t.complete);
            v->setProperty("bodyEnd", (juce::int64)t.bodyEnd);
            v->setProperty("outputFrames", (juce::int64)t.outputFrames);
            v->setProperty("generation", (juce::int64)t.generation);
            v->setProperty("rate", t.sampleRate);
            v->setProperty("captureRate", t.captureRate);
            v->setProperty("processing", t.processing);
            v->setProperty("correctedFrom", t.correctedFrom);
            v->setProperty("originalRecipeHash", t.originalRecipeHash);
            v->setProperty("originalEventHash", t.originalEventHash);
            v->setProperty("poolOrigins", t.poolOrigins);
            juce::Array<juce::var> s;
            for (auto &source : t.sources)
            {
                s.add(source.id);
                s.add(source.sha256);
                s.add(source.overridePath);
                s.add((juce::int64)source.overrideSize);
                s.add((juce::int64)source.overrideModified);
                s.add((juce::int64)source.overrideInode);
            }
            v->setProperty("sources", s);
            return juce::JSON::toString(o, true);
        };
        if (header(x) != header(y))
            return false;
    }
    return true;
}
DesignSessionServices nativeServices(AudioClient &audio)
{
    DesignSessionServices services;
    services.status = [&audio] { return audio.status(); };
    services.play = [&audio](const auto &id, const auto &file, int64_t frame, const auto &mode)
    { return audio.playDesign(id, file, frame, {}, mode); };
    services.parameters = [&audio](const auto &document, uint64_t generation, double event)
    { audio.updateDesign(document, generation, event); };
    services.stop = [&audio] { audio.stop(); };
    services.finishTake = [&audio](bool cancel) { audio.finishTake(cancel); };
    services.reserveTake = [&audio](bool reserve) { audio.reserveTake(reserve); };
    return services;
}
} // namespace

DesignSession::DesignSession(juce::File file, AudioClient &audio)
    : DesignSession(std::move(file), nativeServices(audio))
{
}

DesignSession::DesignSession(juce::File file, DesignSessionServices supplied)
    : catalog(std::move(file)), directory(catalog.getParentDirectory()), services(std::move(supplied)),
      recovery(std::make_shared<DesignRecoveryStore>(directory.getChildFile("designer-autosave")))
{
    document.original.layout = channelLayout("stereo-v1");
    document.renderSettings.layout = "same";
    if (!services.now)
        services.now = [] { return monotonicMs(); };
    if (!services.reserveTake)
        services.reserveTake = [](bool) {};
    if (!services.post)
        services.post = [](auto callback) { juce::MessageManager::callAsync(std::move(callback)); };
    if (!services.write)
        services.write = [](const auto &path, const auto &snapshot) { saveDesignAtomic(path, snapshot); };
    if (!services.resolve)
        services.resolve = [db = catalog](auto &snapshot) { resolveDesignSources(snapshot, db); };
    if (!services.retain)
        services.retain = [store = recovery](const auto &snapshot, const auto &path, bool changed)
        { store->retain(snapshot, path, changed); };
    if (!services.submit)
    {
        worker = std::make_unique<SerialWorker>();
        services.submit = [this](auto job) { return worker->submit(std::move(job)); };
    }
    if (services.timers)
        startTimer(16);
    if (services.automaticRecovery)
    {
        const auto initialId = document.id;
        const auto initialRevision = document.revision;
        const auto initialActivity = startupActivity;
        auto token = stamp(Operation::transition);
        juce::Component::SafePointer<DesignSession> safe(this);
        enqueue(
            token,
            [store = recovery, resolve = services.resolve, safe, initialId, initialRevision, initialActivity]
            {
                auto entries = store->entries();
                bool migrated = false;
                if (!entries.empty())
                {
                    resolve(entries.front().document);
                    migrated = upgradeLiveSpatial(entries.front().document);
                    if (migrated)
                    {
                        ++entries.front().document.revision;
                        entries.front().dirty = true;
                    }
                    entries.front().dirty |= recoverTakeDrafts(entries.front().document);
                }
                return [safe, entries = std::move(entries), initialId, initialRevision, initialActivity,
                        migrated]() mutable
                {
                    if (!safe)
                        return;
                    safe->recoveries = std::move(entries);
                    if (!safe->recoveries.empty() && safe->document.id == initialId &&
                        safe->document.revision == initialRevision &&
                        safe->startupActivity == initialActivity && !safe->dirty)
                    {
                        auto latest = safe->recoveries.front();
                        safe->document = std::move(latest.document);
                        safe->savedFile = latest.savedFile;
                        safe->dirty = latest.dirty;
                        safe->autoRevision = migrated ? 0 : safe->document.revision;
                        safe->autoDocument = safe->document.id;
                        safe->message = latest.dirty ? "Recovered latest changes" : "Reopened saved design";
                        if (migrated)
                            safe->message = "Live spatial processing updated; recorded takes preserved";
                        safe->restoreDerivedState();
                    }
                    safe->notify();
                };
            });
    }
}

DesignSession::~DesignSession()
{
    invalidatePoolWork();
    stopTimer();
    services.reserveTake(false);
    // Capture on the owner thread. The worker never reads the mutable document.
    auto snapshot = document;
    auto path = savedFile;
    bool changed = dirty;
    auto retain = services.retain;
    quitting = true;
    ++*lifetimeGate;
    ++*previewSerial;
    renderCancelled = true;
    if (renderWorker.joinable())
        renderWorker.join();
    auto finalWrite = [snapshot = std::move(snapshot), path, changed, retain, store = recovery]
    {
        if (!changed && path == juce::File() && snapshot.original.layers.empty())
            return;
        try
        {
            retain(snapshot, path, changed);
            store->activate(snapshot.id);
        }
        catch (const std::exception &error)
        {
            juce::Logger::writeToLog("Final Designer recovery failed: " + juce::String(error.what()));
        }
    };
    if (worker)
        worker->finish(std::move(finalWrite));
    else
        finalWrite();
}

DesignSession::Stamp DesignSession::stamp(Operation kind)
{
    auto serial = ++operations[(size_t)kind];
    if (kind == Operation::import)
        acceptedImports.insert(serial);
    return {lifetime, serial, kind};
}
bool DesignSession::accepts(Stamp token) const
{
    return token.lifetime == lifetime && !quitting &&
           (token.kind == Operation::import ? acceptedImports.contains(token.operation)
                                            : token.operation == operations[(size_t)token.kind]);
}
void DesignSession::finishOperation(Stamp token)
{
    if (token.kind == Operation::import)
        acceptedImports.erase(token.operation);
}
void DesignSession::protectStartupEdits()
{
    ++startupActivity;
}
DesignSession::DialogToken DesignSession::beginDialog()
{
    protectStartupEdits();
    return {lifetime, ++dialogSerial};
}
bool DesignSession::accepts(DialogToken token) const
{
    return token.lifetime == lifetime && token.operation == dialogSerial && !quitting;
}

void DesignSession::enqueue(Stamp token, std::function<std::function<void()>()> task)
{
    juce::Component::SafePointer<DesignSession> safe(this);
    auto post = services.post;
    bool accepted = services.submit(
        [safe, token, task = std::move(task), post]
        {
            try
            {
                auto completion = task();
                post(
                    [safe, token, completion = std::move(completion)]
                    {
                        if (safe && safe->accepts(token))
                        {
                            safe->finishOperation(token);
                            completion();
                            safe->notify();
                        }
                    });
            }
            catch (const std::exception &error)
            {
                auto message = juce::String(error.what());
                post(
                    [safe, token, message]
                    {
                        if (!safe || !safe->accepts(token))
                            return;
                        safe->finishOperation(token);
                        safe->working = !safe->acceptedImports.empty();
                        if (token.kind == Operation::transition)
                            safe->replacing = false;
                        if (token.kind == Operation::autosave)
                        {
                            safe->autoPending = false;
                            safe->autosaveRetry = safe->services.now() + 1000;
                        }
                        if (token.kind == Operation::preview)
                            safe->previewWaiting = false;
                        // Source verification also uses this operation serial. A rejected
                        // relocation leaves an existing complete take and its audition intact.
                        if (token.kind == Operation::take && safe->takeBusy())
                        {
                            safe->services.reserveTake(false);
                            safe->takeState = TakeState::incomplete;
                            safe->takeReadPending = false;
                            safe->active = false;
                        }
                        safe->message = "Operation failed; current design retained. " + message;
                        safe->notify();
                    });
            }
        });
    if (!accepted)
    {
        finishOperation(token);
        if (token.kind == Operation::take && takeBusy())
        {
            services.reserveTake(false);
            takeState = TakeState::incomplete;
            takeReadPending = false;
        }
        working = replacing = autoPending = false;
        message = "Background queue is busy; current design retained. Try again.";
        notify();
    }
}

juce::File DesignSession::outputFolder() const
{
    if (document.outputFolder.isNotEmpty())
        return juce::File(document.outputFolder);
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("SoundPlus/Renders");
}
void DesignSession::notify()
{
    if (onChange)
        onChange();
}
void DesignSession::restoreDerivedState()
{
    restoreTakeDrafts();
    selectedIds.clear();
    if (layerById(document, document.selectedLayer))
        selectedIds.insert(document.selectedLayer);
    selectContextOutput();
}
void DesignSession::reconcileSelection(const DesignDocument *before)
{
    if (before && (before->selectedLayer != document.selectedLayer ||
                   before->activeVariation != document.activeVariation))
        selectedIds.clear();
    std::erase_if(selectedIds, [&](const auto &id) { return !layerById(document, id); });
    if (layerById(document, document.selectedLayer))
        selectedIds.insert(document.selectedLayer);
    else
        document.selectedLayer = selectedIds.empty() ? juce::String() : *selectedIds.begin();
}
void DesignSession::changed(ChangeImpact impact)
{
    if ((active || previewWaiting) && auditionContext.isNotEmpty() &&
        auditionContext != designPlaybackContext(document))
        stop(); // Undo/context restoration must not label A's audio as another take or Live Design.
    dirty = true;
    lastEdit = services.now();
    selectContextOutput();
    if (impact == ChangeImpact::audio && document.activeTake.isEmpty() && active && previewDue <= 0)
        previewDue = std::max(lastEdit, lastAudioUpdate + 25);
    notify();
}
void DesignSession::edit(const juce::String &label, const std::function<void(DesignDocument &)> &edit,
                         ChangeImpact impact)
{
    if (takeBusy() && !takeMutation)
    {
        if (takeState != TakeState::recording)
            return;
        if (label != "Macro" && label != "Macro bypass" && label != "Reset macro" &&
            label != "Blend position" && label != "Blend bypass" && label != "Spatial motion")
        {
            message = "Stop recording before editing the frozen design";
            notify();
            return;
        }
        auto controls = document;
        controls.activeMix() = takeDisplay;
        edit(controls);
        takeDisplay = controls.activeMix();
        // Capture controls do not modify the editable recipe or UI undo history.
        services.parameters(controls, expectedGeneration, services.now());
        notify();
        return;
    }
    if (document.activeTake.isNotEmpty() && !takeMutation && impact != ChangeImpact::metadata)
    {
        message = "Choose Live Design before editing";
        notify();
        return;
    }
    if (replacing)
        return;
    protectStartupEdits();
    auto before = document;
    auto beforeSelection = selectedIds;
    try
    {
        edit(document);
        if (before.activeVariation != document.activeVariation || before.activeTake != document.activeTake)
            invalidatePoolWork();
        syncPoolSelection(document.activeMix());
        if (before.id == document.id && before.activeVariation == document.activeVariation)
            unlinkManuallyEditedTargets(before.activeMix(), document.activeMix());
        reconcileSelection(&before);
        bool unchangedTakes = sameTakes(before, document);
        auto validation = document;
        // Keep current take identities available to metadata validation even
        // when immutable recipe/event content was already proved unchanged.
        validateDesign(validation, !unchangedTakes);
        if (unchangedTakes)
        {
            validation.takes.clear();
            validation.activeTake.clear();
        }
        auto comparison = before;
        if (unchangedTakes)
        {
            comparison.takes.clear();
            comparison.activeTake.clear();
        }
        if (unchangedTakes && juce::JSON::toString(designToJson(comparison), true) ==
                                  juce::JSON::toString(designToJson(validation), true))
            return;
        document.revision = before.revision + 1;
    }
    catch (const std::exception &error)
    {
        document = std::move(before);
        selectedIds = std::move(beforeSelection);
        message = error.what();
        notify();
        return;
    }
    if (ownsPlayback() && pooledSourcesDiffer(before.activeMix(), document.activeMix()))
        stop();
    if (!gesture)
    {
        undoHistory.push_back({std::move(before), std::move(beforeSelection), label});
        if (undoHistory.size() > 100)
            undoHistory.erase(undoHistory.begin());
        redoHistory.clear();
    }
    changed(impact);
}
void DesignSession::beginGesture()
{
    if (takeBusy())
    {
        takeGestureStart = mixControls(takeDisplay);
        return;
    }
    protectStartupEdits();
    if (!gesture && !replacing)
        gesture = History{document, selectedIds, "Canvas edit"};
}
void DesignSession::endGesture()
{
    if (takeBusy())
    {
        takeGestureStart.reset();
        return;
    }
    if (!gesture)
        return;
    bool committed = document.revision != gesture->document.revision;
    if (committed)
    {
        undoHistory.push_back(std::move(*gesture));
        if (undoHistory.size() > 100)
            undoHistory.erase(undoHistory.begin());
        redoHistory.clear();
    }
    gesture.reset();
    if (committed)
        notify();
}
void DesignSession::cancelGesture()
{
    if (takeBusy())
    {
        if (takeGestureStart)
        {
            applyMixControls(takeDisplay, *takeGestureStart);
            auto d = document;
            d.activeMix() = takeDisplay;
            services.parameters(d, expectedGeneration, services.now());
            takeGestureStart.reset();
            notify();
        }
        return;
    }
    if (!gesture)
        return;
    if (document.revision == gesture->document.revision)
    {
        document.selectedLayer = gesture->document.selectedLayer;
        selectedIds = gesture->selection;
        gesture.reset();
        notify();
        return;
    }
    auto revision = document.revision + 1;
    document = std::move(gesture->document);
    document.revision = revision;
    selectedIds = std::move(gesture->selection);
    gesture.reset();
    changed();
}
void DesignSession::undo()
{
    invalidatePoolWork();
    if (takeBusy())
    {
        message = "Stop recording before Undo";
        notify();
        return;
    }
    if (replacing)
        return;
    endGesture();
    if (undoHistory.empty())
        return;
    if (ownsPlayback() && pooledSourcesDiffer(document.activeMix(), undoHistory.back().document.activeMix()))
        stop();
    auto revision = document.revision + 1;
    redoHistory.push_back({document, selectedIds, undoHistory.back().label});
    document = std::move(undoHistory.back().document);
    selectedIds = std::move(undoHistory.back().selection);
    undoHistory.pop_back();
    reconcileSelection();
    document.revision = revision;
    if (auto *t = selectedTake(document))
    {
        takeDisplay = takeDisplayMix(*t);
        takeState = t->complete ? TakeState::ready : TakeState::incomplete;
    }
    else
        takeState = TakeState::idle;
    selectContextOutput();
    changed();
}
void DesignSession::redo()
{
    invalidatePoolWork();
    if (takeBusy())
        return;
    if (replacing)
        return;
    endGesture();
    if (redoHistory.empty())
        return;
    if (ownsPlayback() && pooledSourcesDiffer(document.activeMix(), redoHistory.back().document.activeMix()))
        stop();
    auto revision = document.revision + 1;
    undoHistory.push_back({document, selectedIds, redoHistory.back().label});
    document = std::move(redoHistory.back().document);
    selectedIds = std::move(redoHistory.back().selection);
    redoHistory.pop_back();
    reconcileSelection();
    document.revision = revision;
    if (auto *t = selectedTake(document))
    {
        takeDisplay = takeDisplayMix(*t);
        takeState = t->complete ? TakeState::ready : TakeState::incomplete;
    }
    else
        takeState = TakeState::idle;
    selectContextOutput();
    changed();
}
DesignLayer *DesignSession::selectedLayer()
{
    return layerById(document, document.selectedLayer);
}

void DesignSession::transition(const juce::File &file, bool recover, std::function<void()> close)
{
    if (takeBusy())
    {
        message = "Stop or cancel the take before closing or changing designs";
        notify();
        return;
    }
    if (rendering)
    {
        message = "Cancel or finish rendering before changing designs";
        notify();
        return;
    }
    endGesture();
    stop();
    invalidatePoolWork();
    ++lifetime;
    acceptedImports.clear();
    *lifetimeGate = lifetime;
    ++dialogSerial;
    replacing = working = true;
    autoPending = false;
    auto token = stamp(Operation::transition);
    auto snapshot = document;
    auto path = savedFile;
    bool changed = dirty;
    message = "Retaining current design…";
    notify();
    juce::Component::SafePointer<DesignSession> safe(this);
    auto next = DesignDocument();
    next.original.layout = channelLayout("stereo-v1");
    next.renderSettings.layout = "same";
    enqueue(token,
            [safe, snapshot, path, changed, file, recover, close, next, retain = services.retain,
             resolve = services.resolve, store = recovery, gate = lifetimeGate, epoch = lifetime]() mutable
            {
                if (changed || path != juce::File())
                    retain(snapshot, path, changed);
                DesignRecovery replacement;
                bool historicalRecall = false;
                if (file != juce::File())
                {
                    if (recover)
                        replacement = store->read(file);
                    else
                    {
                        replacement.document = loadDesign(file);
                        replacement.savedFile = file;
                        replacement.dirty = false;
                    }
                    resolve(replacement.document);
                    if (upgradeLiveSpatial(replacement.document))
                    {
                        ++replacement.document.revision;
                        replacement.dirty = true;
                    }
                    replacement.dirty |= recoverTakeDrafts(replacement.document);
                }
                else
                {
                    replacement.document = std::move(next);
                    replacement.dirty = false;
                }
                if (gate->load() == epoch)
                {
                    if (close)
                        store->activate(snapshot.id);
                    else
                    {
                        if (file != juce::File())
                        {
                            auto recalledRevision = replacement.document.revision;
                            replacement = store->prepareRecall(std::move(replacement));
                            historicalRecall = replacement.document.revision != recalledRevision;
                        }
                        retain(replacement.document, replacement.savedFile, replacement.dirty);
                        store->activate(replacement.document.id);
                    }
                }
                auto entries = store->entries();
                return [safe, replacement = std::move(replacement), close, historicalRecall,
                        entries = std::move(entries)]() mutable
                {
                    if (!safe)
                        return;
                    safe->working = safe->replacing = false;
                    safe->recoveries = std::move(entries);
                    if (close)
                    {
                        close();
                        return;
                    }
                    safe->document = std::move(replacement.document);
                    safe->savedFile = replacement.savedFile;
                    safe->dirty = replacement.dirty;
                    safe->autoDocument = safe->document.id;
                    safe->autosaveRetry = 0;
                    safe->autoRevision = safe->dirty ? 0 : safe->document.revision;
                    safe->undoHistory.clear();
                    safe->redoHistory.clear();
                    safe->gesture.reset();
                    safe->publishedStructure.clear();
                    safe->restoreDerivedState();
                    safe->message = historicalRecall
                                        ? "Opened historical recipe; newer work retained in recovery"
                                    : safe->dirty ? "Recovered latest changes"
                                                  : "Design ready";
                };
            });
}
void DesignSession::newDesign()
{
    transition({}, false);
}
void DesignSession::open(const juce::File &file)
{
    transition(file, false);
}
void DesignSession::closeWhenSafe(std::function<void()> close)
{
    cancelRender();
    rendering = false;
    transition({}, false, std::move(close));
}
void DesignSession::recoverAutosave(const juce::File &file)
{
    if (file != juce::File())
        transition(file, true);
    else if (!recoveries.empty())
        transition(recoveries.front().snapshot, true);
    else
    {
        message = "No retained designs available";
        refreshRecoveries();
    }
}
void DesignSession::refreshRecoveries()
{
    auto token = stamp(Operation::recoveryList);
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, store = recovery]
            {
                auto entries = store->entries();
                return [safe, entries = std::move(entries)]() mutable
                {
                    if (safe)
                        safe->recoveries = std::move(entries);
                };
            });
}
void DesignSession::save(const juce::File &file)
{
    if (replacing)
        return;
    auto token = stamp(Operation::save);
    auto snapshot = document;
    working = true;
    message = "Saving design…";
    notify();
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, file, snapshot, write = services.write, retain = services.retain, store = recovery,
             gate = lifetimeGate, epoch = lifetime]
            {
                write(file, snapshot);
                retain(snapshot, file, false);
                if (gate->load() == epoch)
                    store->activate(snapshot.id);
                auto entries = store->entries();
                return [safe, file, snapshot, entries = std::move(entries)]() mutable
                {
                    if (!safe)
                        return;
                    safe->savedFile = file;
                    safe->working = false;
                    if (safe->document.revision == snapshot.revision)
                        safe->dirty = false;
                    safe->autoDocument = snapshot.id;
                    safe->autoRevision = snapshot.revision;
                    safe->recoveries = std::move(entries);
                    safe->message = "Saved " + file.getFileName();
                };
            });
}

void DesignSession::add(const Asset &source, int64_t first, int64_t last, bool external, int pair,
                        int64_t placement)
{
    edit("Add layer",
         [&](auto &d)
         {
             if (!d.activeMix().layout.id.isEmpty() && d.activeMix().layers.empty() && source.channels > 2 &&
                 source.channels <= 16)
             {
                 auto detected =
                     source.layoutOverride.id.isNotEmpty() ? source.layoutOverride : source.detectedChannels;
                 setMixLayout(d.activeMix(), detected.id.isEmpty() || detected.id.startsWith("unknown-")
                                                 ? discreteLayout(source.channels)
                                                 : detected);
                 d.renderSettings.layout = "same";
             }
             auto layer = makeDesignLayer(source, first, last, external);
             if (pair == -2 && d.activeMix().layout.id.isEmpty())
                 throw std::runtime_error("Choose a Design layout before adding a complete multichannel bed; "
                                          "legacy designs retain stereo-pair semantics");
             layer.region.auditionPair = pair == -2 || pair == -3 ? 0 : pair;
             if (!d.activeMix().layout.id.isEmpty())
                 initialiseRouting(layer, d.activeMix().layout, pair >= 0);
             layer.placement = std::max<int64_t>(0, placement < 0 ? d.playhead : placement);
             d.selectedLayer = layer.id;
             d.activeMix().layers.push_back(std::move(layer));
         });
}
void DesignSession::addExternal(const juce::File &file, int64_t placement)
{
    addExternals({file.getFullPathName()}, placement < 0 ? document.playhead : placement);
}
void DesignSession::addExternals(const juce::StringArray &files, int64_t placement)
{
    if (replacing || files.isEmpty())
        return;
    protectStartupEdits();
    auto token = stamp(Operation::import);
    working = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, files, placement]
            {
                std::vector<Asset> sources;
                for (auto &file : files)
                    sources.push_back(inspectDesignSource(juce::File(file)));
                return [safe, sources = std::move(sources), placement]
                {
                    if (!safe)
                        return;
                    safe->working = !safe->acceptedImports.empty();
                    safe->edit(
                        "Drop sources",
                        [&](auto &d)
                        {
                            for (auto &source : sources)
                            {
                                if (!d.activeMix().layout.id.isEmpty() && d.activeMix().layers.empty() &&
                                    source.channels > 2 && source.channels <= 16)
                                {
                                    auto detected = source.layoutOverride.id.isNotEmpty()
                                                        ? source.layoutOverride
                                                        : source.detectedChannels;
                                    setMixLayout(d.activeMix(),
                                                 detected.id.isEmpty() || detected.id.startsWith("unknown-")
                                                     ? discreteLayout(source.channels)
                                                     : detected);
                                    d.renderSettings.layout = "same";
                                }
                                auto layer = makeDesignLayer(source, 0, source.samples, true);
                                if (!d.activeMix().layout.id.isEmpty())
                                    initialiseRouting(layer, d.activeMix().layout);
                                layer.placement = placement;
                                d.selectedLayer = layer.id;
                                d.activeMix().layers.push_back(std::move(layer));
                            }
                        });
                };
            });
}
void DesignSession::replaceSource(const juce::String &id, const juce::File &file)
{
    for (auto &l : document.activeMix().layers)
        if (l.id == id && l.pool)
        {
            relinkPoolEntry(id, l.pool->selected, file);
            return;
        }
    protectStartupEdits();
    auto token = stamp(Operation::relink);
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, file, id]
            {
                auto source = inspectDesignSource(file);
                return [safe, source, id]
                {
                    if (!safe)
                        return;
                    safe->edit("Relink source",
                               [&](auto &d)
                               {
                                   if (auto *layer = layerById(d, id))
                                   {
                                       layer->source = source;
                                       layer->external = true;
                                       layer->region.startSample = std::clamp<int64_t>(
                                           layer->region.startSample, 0, source.samples - 1);
                                       layer->region.endSample =
                                           std::clamp<int64_t>(layer->region.endSample,
                                                               layer->region.startSample + 1, source.samples);
                                       layer->sync = std::clamp(layer->sync, layer->region.startSample,
                                                                layer->region.endSample - 1);
                                       layer->region.size = source.size;
                                       layer->region.modified = source.modified;
                                       layer->region.inode = source.inode;
                                       layer->region.revision = source.revision;
                                       if (layer->region.auditionPair * 2 >= source.channels)
                                           layer->region.auditionPair = 0;
                                   }
                               });
                };
            });
}
void DesignSession::selectLayer(const juce::String &id, bool additive)
{
    protectStartupEdits();
    if (id.isEmpty())
    {
        selectedIds.clear();
        document.selectedLayer.clear();
        notify();
        return;
    }
    if (!layerById(document, id))
        return;
    if (!additive)
        selectedIds.clear();
    if (additive && selectedIds.contains(id))
        selectedIds.erase(id);
    else
        selectedIds.insert(id);
    if (selectedIds.contains(id))
        document.selectedLayer = id;
    else if (!selectedIds.contains(document.selectedLayer))
        document.selectedLayer = selectedIds.empty() ? juce::String() : *selectedIds.begin();
    notify();
}
void DesignSession::selectVariation(const juce::String &id)
{
    endGesture();
    edit("Select variation",
         [&](auto &d)
         {
             d.activeVariation = id;
             if (!d.activeMix().layers.empty())
                 d.selectedLayer = d.activeMix().layers.front().id;
             else
                 d.selectedLayer.clear();
         });
}
void DesignSession::generate(uint64_t seed)
{
    if ((document.variationSettings.enabled & varySource) && hasSourcePools(document))
    {
        generatePoolVariations(seed);
        return;
    }
    edit("Generate variations",
         [&](auto &d)
         {
             auto generated = generateVariations(d, seed);
             if (d.variations.size() + generated.size() > 32)
                 throw std::runtime_error("Discard candidates before generating more (32 maximum)");
             for (auto &variation : generated)
                 d.variations.push_back(std::move(variation));
         });
}

bool DesignSession::ownsPlayback() const
{
    return active || previewWaiting;
}
void DesignSession::saveSnapshot(const juce::String &name)
{
    endGesture();
    edit("Save Snapshot",
         [&](auto &d)
         {
             if (d.variations.size() >= 32)
                 throw std::runtime_error("Discard a candidate before saving another snapshot (32 maximum)");
             DesignVariation snapshot;
             snapshot.name =
                 name.trim().isEmpty() ? "Snapshot " + juce::String(d.variations.size() + 1) : name.trim();
             snapshot.snapshot = true;
             snapshot.kept = true;
             snapshot.mix = d.activeMix();
             auto id = snapshot.id;
             d.variations.push_back(std::move(snapshot));
             d.activeVariation = id;
         });
}
void DesignSession::schedulePreview(const juce::String &mode)
{
    if (performanceMix().layers.empty())
        return;
    auto token = stamp(Operation::preview);
    auto serial = ++*previewSerial;
    auto snapshot = document;
    auto position = document.playhead;
    auditionContext = designPlaybackContext(snapshot);
    auto file =
        directory.getChildFile("designer-preview").getChildFile(juce::Uuid().toString() + ".soundplus");
    auto cancellation = previewSerial;
    previewWaiting = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(
        token,
        [safe, serial, cancellation, snapshot, position, file, mode, resolve = services.resolve]() mutable
        {
            if (serial != *cancellation)
                throw QueryCancelled();
            resolve(snapshot);
            saveDesignAtomic(file, snapshot, [=] { return serial != *cancellation; });
            return [safe, snapshot, position, file, mode]
            {
                if (!safe || designPlaybackContext(snapshot) != designPlaybackContext(safe->document))
                    return;
                safe->expectedGeneration = safe->services.play("design:" + snapshot.id, file, position, mode);
                safe->publishedStructure = designStructure(snapshot);
                safe->previewWaiting = false;
                // Edits made while the snapshot/seek was preparing belong to
                // the newly requested generation, not the previous stream.
                if (snapshot.activeTake.isNotEmpty())
                    safe->previewDue = 0; // Immutable replay has no live catch-up path.
                else if (designPlaybackContext(snapshot) == designPlaybackContext(safe->document) &&
                         safe->publishedStructure == designStructure(safe->document))
                {
                    if (safe->document.revision != snapshot.revision)
                        safe->services.parameters(safe->document, safe->expectedGeneration, safe->lastEdit);
                    safe->previewDue = 0;
                }
                else
                    safe->previewDue = safe->services.now() + 25;
            };
        });
}
void DesignSession::play()
{
    if (takeBusy())
    {
        stopTake();
        return;
    }
    if (ownsPlayback())
    {
        stop();
        return;
    }
    if (performanceMix().layers.empty() || replacing)
        return;
    document.playhead = designPlayStart(document, document.playhead);
    if (onBeforePlay)
        onBeforePlay();
    active = true;
    schedulePreview();
    notify();
}
void DesignSession::playAccurate()
{
    if (takeBusy())
        return;
    if (performanceMix().layers.empty() || replacing)
        return;
    auto parked = document.playhead;
    stop();
    document.playhead = designPlayStart(document, parked);
    if (onBeforePlay)
        onBeforePlay();
    active = true;
    schedulePreview("accurate");
    message = "Accurate audition: full effect history. Stop cancels preparation.";
    notify();
}
void DesignSession::stop()
{
    if (takeBusy())
    {
        message = "Recording owns transport; use Stop or Cancel Take";
        return;
    }
    auto state = services.status();
    if (active && !seekGesture && (uint64_t)(int64_t)state["playGeneration"] == expectedGeneration &&
        state["id"].toString() == "design:" + document.id)
        document.playhead = (int64_t)state["sourceSample"];
    ++*previewSerial;
    stamp(Operation::preview);
    if (active || previewWaiting || state["id"].toString().startsWith("design:"))
        services.stop();
    active = previewWaiting = seekGesture = seekWasPlaying = false;
    auditionContext.clear();
    previewDue = 0;
    notify();
}
void DesignSession::seek(int64_t frame)
{
    if (takeBusy() || document.activeTake.isNotEmpty())
    {
        message = "Take playback starts at frame zero with full history";
        return;
    }
    document.playhead =
        std::clamp<int64_t>(frame, 0, mixDuration(document.activeMix(), document.sampleRate) - 1);
    if (active)
        schedulePreview("seek");
    notify();
}
void DesignSession::beginSeekGesture(bool audible)
{
    if (takeBusy() || document.activeTake.isNotEmpty())
        return;
    seekWasPlaying = active;
    seekGesture = true;
    audibleSeek = audible;
}
void DesignSession::dragSeek(int64_t frame)
{
    if (!seekGesture)
        return;
    document.playhead =
        std::clamp<int64_t>(frame, 0, mixDuration(document.activeMix(), document.sampleRate) - 1);
    if ((seekWasPlaying || audibleSeek) && services.now() - lastAudioUpdate >= 30)
    {
        lastAudioUpdate = services.now();
        active = true;
        schedulePreview(audibleSeek ? "scrub" : "seek");
    }
    notify();
}
void DesignSession::endSeekGesture()
{
    if (!seekGesture)
        return;
    bool resume = seekWasPlaying;
    auto parked = document.playhead;
    stop();
    document.playhead = parked;
    if (resume)
    {
        active = true;
        schedulePreview("seek");
    }
    notify();
}
void DesignSession::poll()
{
    timerCallback();
}
void DesignSession::timerCallback()
{
    auto state = services.status();
    auto now = services.now();
    pollTake(state);
    bool owned = state["id"].toString() == "design:" + document.id &&
                 (uint64_t)(int64_t)state["playGeneration"] == expectedGeneration;
    if (active && !previewWaiting && owned && !(bool)state["pendingPlayback"])
    {
        if (!seekGesture)
            document.playhead = (int64_t)state["sourceSample"];
        if (!(bool)state["playing"])
            active = false;
    }
    if (active && !previewWaiting && !(bool)state["pendingPlayback"] &&
        state["error"].toString().isNotEmpty())
    {
        active = false;
        message = state["error"].toString();
        notify();
    }
    if (previewDue > 0 && now >= previewDue)
    {
        previewDue = 0;
        lastAudioUpdate = now;
        if (active && document.activeTake.isEmpty())
        {
            if (publishedStructure == designStructure(document))
                services.parameters(document, expectedGeneration, lastEdit);
            else
                schedulePreview("update");
        }
    }
    if (!replacing && dirty && !autoPending && now >= autosaveRetry && now - lastEdit >= 500 &&
        (autoDocument != document.id || autoRevision != document.revision))
    {
        auto token = stamp(Operation::autosave);
        auto snapshot = document;
        auto path = savedFile;
        autoPending = true;
        juce::Component::SafePointer<DesignSession> safe(this);
        enqueue(token,
                [safe, snapshot, path, retain = services.retain, store = recovery, gate = lifetimeGate,
                 epoch = lifetime]
                {
                    retain(snapshot, path, true);
                    if (gate->load() == epoch)
                        store->activate(snapshot.id);
                    auto entries = store->entries();
                    return [safe, snapshot, entries = std::move(entries)]() mutable
                    {
                        if (!safe)
                            return;
                        safe->autoPending = false;
                        safe->autoDocument = snapshot.id;
                        safe->autoRevision = snapshot.revision;
                        safe->recoveries = std::move(entries);
                    };
                });
    }
}

void DesignSession::cancelRender()
{
    renderCancelled = true;
}
void DesignSession::startOutput(DesignDocument snapshot, bool copy, const juce::File &folder,
                                juce::String kind)
{
    if (rendering || replacing)
        return;
    if (renderWorker.joinable())
        renderWorker.join();
    auto token = stamp(Operation::output);
    renderCancelled = false;
    rendering = true;
    renderProgress = 0;
    juce::Component::SafePointer<DesignSession> safe(this);
    auto resolve = services.resolve;
    auto post = services.post;
    renderWorker = std::thread(
        [this, safe, token, snapshot = std::move(snapshot), copy, folder, kind, resolve, post]() mutable
        {
            try
            {
                resolve(snapshot);
                DesignOutput output;
                if (copy)
                {
                    auto *layer = layerById(snapshot, snapshot.selectedLayer);
                    if (!layer)
                        throw std::runtime_error("Select a layer");
                    output = copyDesignOriginal(*layer, folder, snapshot.revision,
                                                [&] { return quitting || renderCancelled; });
                }
                else
                {
                    output = renderDesign(
                        snapshot, folder, snapshot.renderSettings,
                        [&] { return quitting || renderCancelled; },
                        [&](double progress) { renderProgress = progress; });
                    output.kind = kind;
                }
                post(
                    [safe, token, output]
                    {
                        if (!safe || !safe->accepts(token))
                            return;
                        safe->rendering = false;
                        safe->edit(
                            "Record completed output", [&](auto &d) { d.outputs.push_back(output); },
                            ChangeImpact::metadata);
                        safe->selectContextOutput();
                        safe->message = "Rendered: " + juce::File(output.path).getFileName();
                        safe->notify();
                    });
            }
            catch (const std::exception &error)
            {
                auto message = juce::String(error.what());
                post(
                    [safe, token, message]
                    {
                        if (safe && safe->accepts(token))
                        {
                            safe->rendering = false;
                            safe->message = message;
                            safe->notify();
                        }
                    });
            }
        });
}
void DesignSession::render(bool selection)
{
    if (takeBusy())
        return;
    if (document.activeTake.isNotEmpty() && selection)
    {
        message = "Render Take exports the complete performance";
        return;
    }
    auto snapshot = document.activeTake.isEmpty() ? resolvePerformance(document) : document;
    if (document.activeTake.isNotEmpty())
    {
        snapshot.renderSettings.first = 0;
        snapshot.renderSettings.last = -1;
    }
    if (selection)
    {
        auto *layer = layerById(snapshot, document.selectedLayer);
        if (!layer)
            return;
        auto chosen = *layer;
        chosen.placement = 0;
        chosen.mute = chosen.solo = false;
        snapshot.activeMix().layers = {chosen};
        snapshot.activeMix().duration = 0;
        snapshot.renderSettings.first = 0;
        snapshot.renderSettings.last = -1;
    }
    startOutput(std::move(snapshot), false, outputFolder(),
                document.activeTake.isNotEmpty() ? "take-render"
                : selection                      ? "layer-render"
                                                 : "design-render");
}
void DesignSession::copyOriginal(const juce::File &folder)
{
    if (selectedLayer())
        startOutput(document, true, folder, "original-copy");
}
} // namespace sp
