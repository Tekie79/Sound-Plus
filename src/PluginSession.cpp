#include "PluginSession.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
namespace
{
juce::var obj()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *k, const juce::var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
juce::var represent(const juce::var &completed, bool normalize, const juce::File &directory,
                    const std::function<bool()> &cancel)
{
    auto raw = completed.clone();
    if (raw["rawProcessedPcm"].isObject())
    {
        auto meta = raw["rawProcessedPcm"];
        for (auto key : {"path", "sha256", "peak", "frames", "rate", "layout", "clippedSamples"})
            put(raw, key, meta[key]);
        for (auto key : {"rawProcessedPcm", "normalizationGain", "authoritativePcmHash", "rawPeak", "files"})
            raw.getDynamicObject()->removeProperty(key);
    }
    put(raw, "presentationNormalized", false);
    put(raw, "kind", "plugin-preview-v1");
    if (cancel())
        throw std::runtime_error("Cancelled");
    if (processingHash(juce::File(raw["path"].toString())) != raw["sha256"].toString())
        throw std::runtime_error("Completed raw PCM changed or unavailable");
    if (!normalize)
        return raw;
    auto normalized =
        publishPluginCopy(raw, directory.getChildFile("normalized-previews"), true, false, cancel);
    auto meta = obj();
    for (auto key : {"path", "sha256", "peak", "frames", "rate", "layout", "clippedSamples"})
        put(meta, key, raw[key]);
    put(normalized, "rawProcessedPcm", meta);
    put(normalized, "presentationNormalized", true);
    put(normalized, "kind", "plugin-preview-v1");
    return normalized;
}
juce::var verifiedFallback(const juce::var &completed, const std::function<bool()> &cancel)
{
    // The raw pass is the authority even when a derived representation survives.
    auto raw = represent(completed, false, {}, cancel);
    if (cancel())
        throw std::runtime_error("Cancelled");
    try
    {
        if (processingHash(juce::File(completed["path"].toString())) == completed["sha256"].toString())
            return completed;
    }
    catch (const std::exception &)
    {
    }
    return raw;
}
} // namespace
PluginSession::PluginSession(juce::File helper, juce::File dir)
    : directory(std::move(dir)), child(std::move(helper))
{
    directory.createDirectory();
}
PluginSession::~PluginSession()
{
    ++*epoch;
    worker.finish({});
}
void PluginSession::notify()
{
    if (onChange)
        onChange();
}
void PluginSession::invalidate()
{
    generation = ++*epoch;
    result = juce::var();
    receipt = juce::var();
    retainingPass = false;
    verifyingCancellation = false;
    error = {};
    status = "Stale - render Preview for these settings";
}
void PluginSession::launch(const juce::String &label,
                           std::function<juce::var(const std::function<bool()> &)> run,
                           std::function<void(juce::var)> adopt, bool retainPass)
{
    auto completed = retainPass ? result.clone() : juce::var();
    if (retainPass)
    {
        generation = ++*epoch;
        receipt = juce::var();
        error = {};
    }
    else
        invalidate();
    retainingPass = retainPass;
    verifyingCancellation = false;
    busy = true;
    status = label;
    auto token = generation;
    auto serial = epoch;
    juce::Component::SafePointer<PluginSession> safe(this);
    if (!worker.submit(
            [safe, token, serial, completed, run = std::move(run), adopt = std::move(adopt)]() mutable
            {
                juce::var value;
                juce::var recovered;
                juce::String failure;
                try
                {
                    if (serial->load() != token)
                        return;
                    value = run([serial, token] { return serial->load() != token; });
                }
                catch (const std::exception &e)
                {
                    failure = e.what();
                    if (completed.isObject() && serial->load() == token)
                        try
                        {
                            recovered = verifiedFallback(completed,
                                                         [serial, token] { return serial->load() != token; });
                        }
                        catch (const std::exception &x)
                        {
                            failure += "; completed PCM unavailable: " + juce::String(x.what());
                        }
                }
                juce::MessageManager::callAsync(
                    [safe, token, value = std::move(value), recovered, failure,
                     adopt = std::move(adopt)]() mutable
                    {
                        if (!safe || safe->generation != token)
                            return;
                        safe->busy = false;
                        safe->retainingPass = false;
                        safe->verifyingCancellation = false;
                        if (!safe->contextValid())
                        {
                            safe->invalidate();
                            safe->status = "Document context changed; choose the input again";
                        }
                        else if (failure.isNotEmpty())
                        {
                            safe->result = recovered;
                            safe->receipt = juce::var();
                            safe->error = failure;
                            safe->status = "Failed: " + failure +
                                           (recovered.isObject()
                                                ? " - verified completed PCM available; audition or retry"
                                                : " - no usable completed PCM");
                        }
                        else
                        {
                            try
                            {
                                adopt(value);
                                safe->error = {};
                            }
                            catch (const std::exception &e)
                            {
                                safe->result = juce::var();
                                safe->receipt = juce::var();
                                safe->error = e.what();
                                safe->status = "Failed: " + safe->error;
                            }
                        }
                        safe->notify();
                    });
            }))
    {
        busy = false;
        result = juce::var();
        receipt = juce::var();
        error = "Worker queue is full";
        status = error;
    }
    notify();
}
void PluginSession::clearInput()
{
    invalidate();
    busy = false;
    input = juce::var();
    settings = juce::var();
    past.clear();
    future.clear();
    status = "Input selection changed - Prepare input";
    notify();
}
void PluginSession::prepare(PluginInput selected)
{
    settings = juce::var();
    past.clear();
    future.clear();
    auto path = directory.getChildFile(juce::Uuid().toString() + "-input.wav");
    launch(
        "Preparing verified input",
        [selected, path](auto &cancel) { return stagePluginInput(selected, path, cancel); },
        [this](auto v)
        {
            input = v;
            status = "Input ready - choose a compatible effect";
        });
}
void PluginSession::recallCatalog(std::vector<juce::File> standardRoots)
{
    registeredDiscovery = standardRoots == defaultVst3Locations();
    auto file = directory.getParentDirectory().getChildFile("plugins-v1.json");
    launch(
        "Loading cached plugin registry", [file](auto &) { return PluginCatalog(file).snapshot(); },
        [this, standardRoots](auto v)
        {
            discovery = v;
            status = "Registry loaded - checking VST3 folders and Audio Unit registration";
            auto roots = standardRoots;
            if (auto *saved = v["roots"].getArray())
                for (auto &path : *saved)
                {
                    juce::File root(path.toString());
                    if (std::find(roots.begin(), roots.end(), root) == roots.end())
                        roots.push_back(root);
                }
            scan(roots);
        });
}
void PluginSession::scan(std::vector<juce::File> roots, bool retry)
{
    auto cache = directory.getParentDirectory().getChildFile("plugins-v1.json");
    launch(
        "Scanning installed plugin candidates",
        [this, roots, cache, retry](auto &cancel)
        {
            PluginCatalog catalog(cache);
            return catalog.scan(child, roots, retry, cancel, {}, registeredDiscovery);
        },
        [this](auto v)
        {
            discovery = v;
            int count = 0;
            if (auto *entries = v["entries"].getArray())
                for (auto &e : *entries)
                    if (auto *plugins = e["plugins"].getArray())
                        count += plugins->size();
            status = count ? "Scan complete - choose a compatible effect; failures remain quarantined"
                           : "No compatible native effects found. Choose a VST3 folder or review "
                             "registration/quarantine errors.";
        });
}
void PluginSession::choose(juce::var plugin)
{
    if (!input.isObject())
    {
        status = "Prepare input first";
        notify();
        return;
    }
    auto job = obj();
    put(job, "mode", "describe");
    put(job, "plugin", plugin);
    put(job, "rate", input["rate"]);
    put(job, "block", 512);
    put(job, "layout", input["layout"]);
    auto base = job.clone();
    launch(
        "Loading effect parameters",
        [this, job](auto &cancel) { return child.call(job, directory, 20000, cancel); },
        [this, base](auto v) mutable
        {
            put(base, "state", v["state"]);
            put(base, "stateHash", v["stateHash"]);
            put(base, "parameters", v["parameters"]);
            put(base, "defaultState", v["state"]);
            put(base, "defaultStateHash", v["stateHash"]);
            put(base, "tailSeconds", 2.);
            put(base, "bypass", false);
            settings = base;
            past.clear();
            future.clear();
            status = "Parameters ready - render Preview";
        });
}
void PluginSession::change(const juce::var &v)
{
    if (juce::JSON::toString(v, true) == juce::JSON::toString(settings, true))
        return;
    if (!gesture.isObject())
    {
        past.push_back(settings.clone());
        if (past.size() > 100)
            past.erase(past.begin());
    }
    future.clear();
    settings = v.clone();
    invalidate();
    busy = false;
    notify();
}
void PluginSession::beginGesture()
{
    if (!gesture.isObject())
        gesture = settings.clone();
}
void PluginSession::endGesture()
{
    if (gesture.isObject() && juce::JSON::toString(gesture, true) != juce::JSON::toString(settings, true))
    {
        past.push_back(gesture);
        if (past.size() > 100)
            past.erase(past.begin());
        future.clear();
    }
    gesture = juce::var();
}
void PluginSession::undo()
{
    endGesture();
    if (past.empty())
        return;
    future.push_back(settings);
    settings = past.back();
    past.pop_back();
    invalidate();
    busy = false;
    notify();
}
void PluginSession::redo()
{
    if (future.empty())
        return;
    past.push_back(settings);
    settings = future.back();
    future.pop_back();
    invalidate();
    busy = false;
    notify();
}
void PluginSession::reset()
{
    if (!settings.isObject())
        return;
    auto v = settings.clone();
    put(v, "state", v["defaultState"]);
    put(v, "stateHash", v["defaultStateHash"]);
    if (auto *p = v["parameters"].getArray())
        for (auto &item : *p)
            item.getDynamicObject()->setProperty("value", item["default"]);
    change(v);
}
void PluginSession::cancel()
{
    if (!busy || verifyingCancellation)
        return;
    if (busy && retainingPass && result.isObject())
    {
        auto completed = result.clone();
        launch(
            "Verifying completed PCM after cancellation",
            [completed](auto &cancel) { return verifiedFallback(completed, cancel); },
            [this](auto v)
            {
                result = v;
                status = "Operation cancelled - verified completed PCM available";
            },
            true);
        // This integrity check finishes the cancellation; it is not another
        // cancellable delivery. A second click must not discard its authority.
        verifyingCancellation = busy;
        notify();
        return;
    }
    invalidate();
    busy = false;
    status = "Cancelled - no Ready output";
    notify();
}
void PluginSession::setNormalization(bool enabled)
{
    if (deliveryNormalize == enabled && (!ready() || (bool)result["presentationNormalized"] == enabled))
        return;
    deliveryNormalize = enabled;
    if (!ready())
    {
        if (busy)
            cancel();
        status = "Delivery normalization changed - render Preview when parameters are ready";
        notify();
        return;
    }
    auto completed = result.clone();
    launch(
        "Updating audition representation from completed PCM",
        [this, completed, enabled](auto &cancel) { return represent(completed, enabled, directory, cancel); },
        [this](auto v)
        {
            result = v;
            status = "Ready - representation updated from the same completed plugin pass";
        },
        true);
}
void PluginSession::preview()
{
    endGesture();
    if (!input.isObject() || !settings.isObject() || !contextValid())
    {
        status = "Choose a valid input and effect";
        notify();
        return;
    }
    auto job = settings.clone();
    put(job, "normalize", deliveryNormalize);
    put(job, "mode", "process");
    put(job, "input", input);
    put(job, "output", directory.getChildFile(juce::Uuid().toString() + "-processed.wav").getFullPathName());
    put(job, "generation", (juce::int64)(generation + 1));
    launch(
        "Processing finite PCM",
        [this, job](auto &cancel)
        {
            auto v = child.call(job, directory, 120000, cancel);
            auto outcome = obj();
            try
            {
                put(outcome, "pass", represent(v, (bool)job["normalize"], directory, cancel));
            }
            catch (const std::exception &e)
            {
                put(outcome, "pass", verifiedFallback(v, cancel));
                put(outcome, "representationError", juce::String(e.what()));
            }
            auto frozen = outcome["pass"]["job"].clone();
            put(frozen, "version", 1);
            put(frozen, "result", outcome["pass"]);
            try
            {
                saveProcessingJson(directory.getChildFile("job.json"), frozen);
            }
            catch (const std::exception &e)
            {
                put(outcome, "recordError", juce::String(e.what()));
            }
            return outcome;
        },
        [this, job](auto v)
        {
            result = v["pass"];
            status = "Ready - preview is the authoritative processed sound";
            if (v["recordError"].toString().isNotEmpty())
                status =
                    "Verified completed PCM available; job record failed: " + v["recordError"].toString();
            if (v["representationError"].toString().isNotEmpty())
                status = "Normalization failed - verified raw PCM available: " +
                         v["representationError"].toString();
        });
}
void PluginSession::createCopy(juce::File folder, bool normalize, bool split)
{
    if (!ready())
        return;
    auto frozen = result.clone();
    auto staged = deliveryStaged;
    launch(
        "Creating durable copy from completed PCM",
        [frozen, folder, normalize, split, staged](auto &cancel)
        {
            verifiedFallback(frozen, cancel);
            return publishPluginCopy(frozen, folder, normalize, split, cancel, staged);
        },
        [this, frozen](auto v)
        {
            result = frozen;
            receipt = v;
            status = "Copy ready - index, Print or drag the finalized WAV";
        },
        true);
}
void PluginSession::save(const juce::File &file)
{
    auto payload = obj();
    put(payload, "version", 1);
    put(payload, "deliverySplit", deliverySplit);
    put(payload, "deliveryNormalize", deliveryNormalize);
    put(payload, "input", input);
    put(payload, "settings", settings);
    put(payload, "result", result);
    put(payload, "receipt", receipt);
    auto bytes = juce::JSON::toString(payload, true);
    auto envelope = obj();
    put(envelope, "payload", payload);
    put(envelope, "sha256", juce::SHA256(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8()).toHexString());
    saveProcessingJson(file, envelope);
}
void PluginSession::open(const juce::File &file)
{
    launch(
        "Verifying recalled job and completed sound",
        [file](auto &cancel)
        {
            auto envelope = loadProcessingJson(file);
            auto v = envelope["payload"];
            auto bytes = juce::JSON::toString(v, true);
            if (!v.isObject() || (int)v["version"] != 1 ||
                juce::SHA256(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8()).toHexString() !=
                    envelope["sha256"].toString())
                throw std::runtime_error("Processing job version/checksum mismatch");
            if (cancel())
                throw std::runtime_error("Cancelled");
            // A saved receipt is a historical delivery record, not current delivery
            // readiness. A new verified atomic copy is required before Print/index/drag.
            v.getDynamicObject()->removeProperty("receipt");
            if (v["result"].isObject())
                try
                {
                    auto completed = v["result"];
                    auto verified = verifiedFallback(completed, cancel);
                    if ((bool)completed["presentationNormalized"] &&
                        !(bool)verified["presentationNormalized"])
                        put(v, "recallNotice",
                            "Normalized output unavailable - verified raw PCM recovered; "
                            "normalization may be retried without a new Preview");
                    put(v, "result", verified);
                }
                catch (const std::exception &e)
                {
                    if (cancel())
                        throw;
                    v.getDynamicObject()->removeProperty("result");
                    put(v, "recallNotice", "Completed raw authority unavailable: " + juce::String(e.what()));
                }
            if (cancel())
                throw std::runtime_error("Cancelled");
            return v;
        },
        [this](auto v)
        {
            input = v["input"];
            settings = v["settings"];
            result = v["result"];
            receipt = v["receipt"];
            deliverySplit = (bool)v["deliverySplit"];
            deliveryNormalize = v.hasProperty("deliveryNormalize") ? (bool)v["deliveryNormalize"]
                                                                   : (bool)settings["normalize"];
            past.clear();
            future.clear();
            status = result.isObject() ? "Ready - retained sound; a new Preview requires the matching plugin"
                                       : "Recalled settings - completed PCM unavailable; render Preview";
            if (v["recallNotice"].toString().isNotEmpty())
                status = v["recallNotice"].toString();
        });
}
} // namespace sp
