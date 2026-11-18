#include "DesignSession.h"
#include "DesignLive.h"
#include "Performance.h"
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
DesignSessionServices nativeServices(AudioClient &audio)
{
    DesignSessionServices services;
    services.status = [&audio] { return audio.status(); };
    services.play = [&audio](const auto &id, const auto &file, int64_t frame, const auto &mode)
    { return audio.playDesign(id, file, frame, {}, mode); };
    services.parameters = [&audio](const auto &document, uint64_t generation, double event)
    { audio.updateDesign(document, generation, event); };
    services.stop = [&audio] { audio.stop(); };
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
    if (!services.now)
        services.now = [] { return monotonicMs(); };
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
        auto token = stamp(Operation::transition);
        juce::Component::SafePointer<DesignSession> safe(this);
        enqueue(token,
                [store = recovery, resolve = services.resolve, safe]
                {
                    auto entries = store->entries();
                    if (!entries.empty())
                        resolve(entries.front().document);
                    return [safe, entries = std::move(entries)]() mutable
                    {
                        if (!safe)
                            return;
                        safe->recoveries = std::move(entries);
                        if (!safe->recoveries.empty())
                        {
                            auto latest = safe->recoveries.front();
                            safe->document = std::move(latest.document);
                            safe->savedFile = latest.savedFile;
                            safe->dirty = latest.dirty;
                            safe->autoRevision = safe->document.revision;
                            safe->autoDocument = safe->document.id;
                            safe->message =
                                latest.dirty ? "Recovered latest changes" : "Reopened saved design";
                            safe->restoreDerivedState();
                        }
                        safe->notify();
                    };
                });
    }
}

DesignSession::~DesignSession()
{
    stopTimer();
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
    return {lifetime, ++operations[(size_t)kind], kind};
}
bool DesignSession::accepts(Stamp token) const
{
    return token.lifetime == lifetime && token.operation == operations[(size_t)token.kind] && !quitting;
}
DesignSession::DialogToken DesignSession::beginDialog()
{
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
                        safe->working = false;
                        if (token.kind == Operation::transition)
                            safe->replacing = false;
                        if (token.kind == Operation::autosave)
                        {
                            safe->autoPending = false;
                            safe->autosaveRetry = safe->services.now() + 1000;
                        }
                        if (token.kind == Operation::preview)
                            safe->previewWaiting = false;
                        safe->message = "Operation failed; current design retained. " + message;
                        safe->notify();
                    });
            }
        });
    if (!accepted)
    {
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
    selectedIds.clear();
    if (layerById(document, document.selectedLayer))
        selectedIds.insert(document.selectedLayer);
    lastOutput = document.outputs.empty() ? juce::File() : juce::File(document.outputs.back().path);
}
void DesignSession::changed()
{
    dirty = true;
    lastEdit = services.now();
    if (active && previewDue <= 0)
        previewDue = std::max(lastEdit, lastAudioUpdate + 25);
    notify();
}
void DesignSession::edit(const juce::String &label, const std::function<void(DesignDocument &)> &edit)
{
    if (replacing)
        return;
    auto before = document;
    try
    {
        edit(document);
        validateDesign(document);
        if (juce::JSON::toString(designToJson(before), true) ==
            juce::JSON::toString(designToJson(document), true))
            return;
        document.revision = before.revision + 1;
    }
    catch (const std::exception &error)
    {
        document = std::move(before);
        message = error.what();
        notify();
        return;
    }
    if (!gesture)
    {
        undoHistory.push_back({std::move(before), selectedIds, label});
        if (undoHistory.size() > 100)
            undoHistory.erase(undoHistory.begin());
        redoHistory.clear();
    }
    changed();
}
void DesignSession::beginGesture()
{
    if (!gesture && !replacing)
        gesture = History{document, selectedIds, "Canvas edit"};
}
void DesignSession::endGesture()
{
    if (!gesture)
        return;
    if (document.revision != gesture->document.revision)
    {
        undoHistory.push_back(std::move(*gesture));
        if (undoHistory.size() > 100)
            undoHistory.erase(undoHistory.begin());
        redoHistory.clear();
    }
    gesture.reset();
}
void DesignSession::cancelGesture()
{
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
    if (replacing)
        return;
    endGesture();
    if (undoHistory.empty())
        return;
    auto revision = document.revision + 1;
    redoHistory.push_back({document, selectedIds, undoHistory.back().label});
    document = std::move(undoHistory.back().document);
    selectedIds = std::move(undoHistory.back().selection);
    undoHistory.pop_back();
    document.revision = revision;
    lastOutput = document.outputs.empty() ? juce::File() : juce::File(document.outputs.back().path);
    changed();
}
void DesignSession::redo()
{
    if (replacing || redoHistory.empty())
        return;
    endGesture();
    auto revision = document.revision + 1;
    undoHistory.push_back({document, selectedIds, redoHistory.back().label});
    document = std::move(redoHistory.back().document);
    selectedIds = std::move(redoHistory.back().selection);
    redoHistory.pop_back();
    document.revision = revision;
    lastOutput = document.outputs.empty() ? juce::File() : juce::File(document.outputs.back().path);
    changed();
}
DesignLayer *DesignSession::selectedLayer()
{
    return layerById(document, document.selectedLayer);
}

void DesignSession::transition(const juce::File &file, bool recover, std::function<void()> close)
{
    if (rendering)
    {
        message = "Cancel or finish rendering before changing designs";
        notify();
        return;
    }
    endGesture();
    stop();
    ++lifetime;
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
    enqueue(token,
            [safe, snapshot, path, changed, file, recover, close, next, retain = services.retain,
             resolve = services.resolve, store = recovery, gate = lifetimeGate, epoch = lifetime]() mutable
            {
                if (changed || path != juce::File())
                    retain(snapshot, path, changed);
                DesignRecovery replacement;
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
                        retain(replacement.document, replacement.savedFile, replacement.dirty);
                        store->activate(replacement.document.id);
                    }
                }
                auto entries = store->entries();
                return [safe, replacement = std::move(replacement), close,
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
                    safe->message = safe->dirty ? "Recovered latest changes" : "Design ready";
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
    auto token = stamp(Operation::import);
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
             auto layer = makeDesignLayer(source, first, last, external);
             layer.region.auditionPair = pair;
             layer.placement = std::max<int64_t>(0, placement < 0 ? d.playhead : placement);
             d.selectedLayer = layer.id;
             d.activeMix().layers.push_back(std::move(layer));
         });
    selectedIds = {document.selectedLayer};
}
void DesignSession::addExternal(const juce::File &file, int64_t placement)
{
    addExternals({file.getFullPathName()}, placement < 0 ? document.playhead : placement);
}
void DesignSession::addExternals(const juce::StringArray &files, int64_t placement)
{
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
                    safe->working = false;
                    safe->edit("Drop sources",
                               [&](auto &d)
                               {
                                   for (auto &source : sources)
                                   {
                                       auto layer = makeDesignLayer(source, 0, source.samples, true);
                                       layer.placement = placement;
                                       d.selectedLayer = layer.id;
                                       d.activeMix().layers.push_back(std::move(layer));
                                   }
                               });
                    safe->selectedIds = {safe->document.selectedLayer};
                };
            });
}
void DesignSession::replaceSource(const juce::String &id, const juce::File &file)
{
    auto token = stamp(Operation::import);
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
    if (!additive)
        selectedIds.clear();
    if (additive && selectedIds.contains(id))
        selectedIds.erase(id);
    else
        selectedIds.insert(id);
    if (selectedIds.contains(id))
        document.selectedLayer = id;
    else if (!selectedIds.empty())
        document.selectedLayer = *selectedIds.begin();
    notify();
}
void DesignSession::selectVariation(const juce::String &id)
{
    edit("Select variation",
         [&](auto &d)
         {
             d.activeVariation = id;
             if (!d.activeMix().layers.empty())
                 d.selectedLayer = d.activeMix().layers.front().id;
         });
    selectedIds = {document.selectedLayer};
}
void DesignSession::generate(uint64_t seed)
{
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
void DesignSession::schedulePreview(const juce::String &mode)
{
    if (document.activeMix().layers.empty())
        return;
    auto token = stamp(Operation::preview);
    auto serial = ++*previewSerial;
    auto snapshot = document;
    auto position = document.playhead;
    auto file =
        directory.getChildFile("designer-preview").getChildFile(juce::Uuid().toString() + ".soundplus");
    auto cancellation = previewSerial;
    previewWaiting = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, serial, cancellation, snapshot, position, file, mode, resolve = services.resolve]() mutable
            {
                if (serial != *cancellation)
                    throw QueryCancelled();
                resolve(snapshot);
                saveDesignAtomic(file, snapshot, [=] { return serial != *cancellation; });
                return [safe, snapshot, position, file, mode]
                {
                    if (!safe)
                        return;
                    safe->expectedGeneration =
                        safe->services.play("design:" + snapshot.id, file, position, mode);
                    safe->publishedStructure = designStructure(snapshot);
                    safe->previewWaiting = false;
                };
            });
}
void DesignSession::play()
{
    if (ownsPlayback())
    {
        stop();
        return;
    }
    if (document.activeMix().layers.empty() || replacing)
        return;
    document.playhead = designPlayStart(document, document.playhead);
    if (onBeforePlay)
        onBeforePlay();
    active = true;
    schedulePreview();
    notify();
}
void DesignSession::stop()
{
    auto state = services.status();
    if (active && !seekGesture && (uint64_t)(int64_t)state["playGeneration"] == expectedGeneration &&
        state["id"].toString() == "design:" + document.id)
        document.playhead = (int64_t)state["sourceSample"];
    ++*previewSerial;
    stamp(Operation::preview);
    if (active || previewWaiting || state["id"].toString().startsWith("design:"))
        services.stop();
    active = previewWaiting = seekGesture = seekWasPlaying = false;
    previewDue = 0;
    notify();
}
void DesignSession::seek(int64_t frame)
{
    document.playhead =
        std::clamp<int64_t>(frame, 0, mixDuration(document.activeMix(), document.sampleRate) - 1);
    if (active)
        schedulePreview("seek");
    notify();
}
void DesignSession::beginSeekGesture(bool audible)
{
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
        if (active)
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
                        safe->lastOutput = juce::File(output.path);
                        safe->edit("Record completed output", [&](auto &d) { d.outputs.push_back(output); });
                        safe->message = "Ready to drag: " + safe->lastOutput.getFileName();
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
    auto snapshot = document;
    if (selection)
    {
        auto *layer = selectedLayer();
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
    startOutput(std::move(snapshot), false, outputFolder(), selection ? "layer-render" : "design-render");
}
void DesignSession::copyOriginal(const juce::File &folder)
{
    if (selectedLayer())
        startOutput(document, true, folder, "original-copy");
}
} // namespace sp
