#include "PluginProcessing.h"
#include <juce_cryptography/juce_cryptography.h>
#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <set>
#include <cmath>
#include <limits.h>
#include <mutex>
#include <map>
extern char **environ;
namespace sp
{
std::vector<juce::File> defaultVst3Locations()
{
    return {juce::File("/Library/Audio/Plug-Ins/VST3"),
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Library/Audio/Plug-Ins/VST3")};
}
juce::String pluginFormatLabel(const juce::var &plugin)
{
    if (plugin["format"].toString() == "AudioUnit")
        return (int)plugin["auVersion"] == 3 ? "AUv3" : "AUv2";
    return plugin["format"].toString();
}
juce::String pluginParameterSchemaSignature(const juce::var &parameters)
{
    juce::Array<juce::var> fields;
    if (auto *list = parameters.getArray())
        for (auto &p : *list)
        {
            auto *o = new juce::DynamicObject();
            for (auto key : {"index", "id", "stableId", "name", "steps", "automatable"})
                o->setProperty(key, p[key]);
            fields.add(juce::var(o));
        }
    auto text = juce::JSON::toString(juce::var(fields), true);
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}
juce::String pluginCandidateSelectionKey(const juce::var &row)
{
    auto p = row["plugin"];
    auto format = p["format"].toString();
    auto canonical = row["canonicalIdentity"].toString();
    if (canonical.isEmpty())
        canonical = row["path"].toString();
    auto id = p["id"].toString();
    return (id.isNotEmpty() ? "class:" : "bundle:") + format + "\n" + canonical + "\n" + id;
}
juce::StringArray pluginDisplayNames(const juce::Array<juce::var> &plugins)
{
    juce::StringArray names;
    for (auto &p : plugins)
        names.add(p["name"].toString());
    for (auto key : {"format", "vendor"})
    {
        auto before = names;
        for (int i = 0; i < names.size(); ++i)
            if (std::count(before.begin(), before.end(), before[i]) > 1)
            {
                auto detail = juce::String(key) == "format" ? pluginFormatLabel(plugins[i])
                                                            : plugins[i][key].toString();
                bool differs = false;
                for (int j = 0; j < names.size(); ++j)
                    if (before[i] == before[j] && plugins[i][key] != plugins[j][key])
                        differs = true;
                if (differs && detail.isNotEmpty())
                    names.set(i, before[i] + " · " + detail);
            }
    }
    auto before = names;
    for (int i = 0; i < names.size(); ++i)
        if (std::count(before.begin(), before.end(), before[i]) > 1)
        {
            int ordinal = 1;
            for (int j = 0; j < i; ++j)
                if (before[j] == before[i])
                    ++ordinal;
            names.set(i, before[i] + " · " + juce::String(ordinal));
        }
    return names;
}
juce::String pluginEffectGroup(const juce::var &plugin)
{
    if ((bool)plugin["instrument"])
        return "Instruments (not supported)";
    auto category = plugin["category"].toString();
    for (auto [token, group] :
         std::initializer_list<std::pair<const char *, const char *>>{{"Reverb", "Reverb"},
                                                                      {"Delay", "Delay"},
                                                                      {"Dynamics", "Dynamics"},
                                                                      {"Compressor", "Dynamics"},
                                                                      {"Limiter", "Dynamics"},
                                                                      {"EQ", "EQ / Filters"},
                                                                      {"Filter", "EQ / Filters"},
                                                                      {"Distortion", "Distortion"},
                                                                      {"Modulation", "Modulation"},
                                                                      {"Pitch", "Pitch"},
                                                                      {"Spatial", "Spatial / Surround"},
                                                                      {"Surround", "Spatial / Surround"},
                                                                      {"Restoration", "Restoration"},
                                                                      {"Analyzer", "Analyzers / Meters"},
                                                                      {"Tools", "Utilities"},
                                                                      {"Utility", "Utilities"}})
        for (auto &reported : juce::StringArray::fromTokens(category, "|", ""))
            if (reported.trim().equalsIgnoreCase(token))
                return group;
    return "Other effects";
}
namespace
{
void require(bool b, const juce::String &s)
{
    if (!b)
        throw std::runtime_error(s.toStdString());
}
juce::var object()
{
    return juce::var(new juce::DynamicObject());
}
void set(juce::var &o, const char *k, const juce::var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
} // namespace
juce::String processingHash(const juce::File &f)
{
    juce::FileInputStream stream(f);
    require(stream.openedOk(), "Missing artifact: " + f.getFullPathName());
    return juce::SHA256(stream).toHexString();
}
juce::String pluginFingerprint(const juce::File &f)
{
    require(f.exists(), "Plugin is missing; choose its replacement and run a new Preview");
    juce::StringArray lines;
    auto files = f.isDirectory() ? f.findChildFiles(juce::File::findFiles, true) : juce::Array<juce::File>{f};
    require(files.size() <= 4096, "Plugin bundle exceeds file-count limit");
    int64_t totalBytes = 0;
    for (auto &p : files)
    {
        totalBytes += p.getSize();
        require(totalBytes <= 1024LL * 1024 * 1024, "Plugin bundle exceeds 1 GiB total fingerprint bound");
        require(p.getSize() <= 1024LL * 1024 * 1024, "Plugin member exceeds 1 GiB");
        lines.add(p.getRelativePathFrom(f) + ":" + processingHash(p));
    }
    lines.sort(false);
    auto s = lines.joinIntoString("\n");
    return juce::SHA256(s.toRawUTF8(), s.getNumBytesAsUTF8()).toHexString();
}
void saveProcessingJson(const juce::File &file, const juce::var &v)
{
    file.getParentDirectory().createDirectory();
    juce::TemporaryFile temp(file);
    require(temp.getFile().replaceWithText(juce::JSON::toString(v, true)), "Cannot stage processing record");
    require(temp.overwriteTargetFileWithTemporary(), "Cannot publish processing record");
}
juce::var loadProcessingJson(const juce::File &f)
{
    require(f.existsAsFile() && f.getSize() <= 32 * 1024 * 1024, "Missing or oversized processing record");
    auto v = juce::JSON::parse(f);
    require(v.isObject(), "Invalid processing record");
    return v;
}
juce::var pluginInputJson(const PluginInput &i)
{
    auto o = object();
    auto src = object();
    set(src, "id", i.source.id);
    set(src, "path", i.source.path);
    set(src, "name", i.source.name);
    set(src, "revision", i.source.revision);
    set(src, "size", i.source.size);
    set(src, "modified", i.source.modified);
    set(src, "inode", i.source.inode);
    set(src, "rate", i.source.sampleRate);
    set(src, "channels", i.source.channels);
    set(src, "samples", i.source.samples);
    set(o, "source", src);
    set(o, "first", i.first);
    set(o, "last", i.last);
    set(o, "layout", layoutJson(i.layout));
    juce::Array<juce::var> a;
    for (auto n : i.inputs)
        a.add(n);
    set(o, "inputs", a);
    set(o, "mean", i.mean);
    set(o, "scope", i.scope);
    return o;
}
PluginInput pluginInputFromJson(const juce::var &v)
{
    PluginInput i;
    auto src = v["source"];
    i.source.id = src["id"].toString();
    i.source.path = src["path"].toString();
    i.source.name = src["name"].toString();
    i.source.revision = (int64_t)src["revision"];
    i.source.size = (int64_t)src["size"];
    i.source.modified = (int64_t)src["modified"];
    i.source.inode = (int64_t)src["inode"];
    i.source.sampleRate = (double)src["rate"];
    i.source.channels = (int)src["channels"];
    i.source.samples = (int64_t)src["samples"];
    i.first = (int64_t)v["first"];
    i.last = (int64_t)v["last"];
    i.layout = layoutFromJson(v["layout"]);
    i.mean = (bool)v["mean"];
    i.scope = v["scope"].toString();
    if (auto *a = v["inputs"].getArray())
        for (auto &n : *a)
            i.inputs.push_back((int)n);
    return i;
}
juce::var stagePluginInput(const PluginInput &i, const juce::File &file, const std::function<bool()> &cancel)
{
    validateLayout(i.layout);
    require(!i.layout.id.startsWith("unknown-"), "Choose an explicit ordered interpretation first");
    auto source = juce::File(i.source.path);
    FileStamp stamp;
    require(fileStamp(source, stamp), "Input is offline");
    if (i.source.size > 0)
        require(stamp.size == i.source.size && stamp.modified == i.source.modified &&
                    stamp.inode == i.source.inode,
                "Source revision changed; select its current revision");
    auto before = processingHash(source);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(source));
    require(r != nullptr, "Cannot decode source");
    require((int)r->numChannels == i.source.channels && r->lengthInSamples == i.source.samples &&
                r->sampleRate == i.source.sampleRate,
            "Source header/revision changed; select its current revision");
    auto last = i.last < 0 ? r->lengthInSamples : i.last;
    require(i.first >= 0 && last > i.first && last <= r->lengthInSamples, "Invalid finite source range");
    require(r->numChannels <= 64 && r->sampleRate >= 8000 && r->sampleRate <= 192000,
            "Unsupported physical channels/sample rate");
    require((last - i.first) / r->sampleRate <= 600, "Processing input exceeds 600 seconds");
    require(i.mean ? i.layout.size() == 1 : ((int)i.inputs.size() == i.layout.size()),
            "Explicit physical subset must match ordered layout");
    std::set<int> seen;
    for (auto n : i.inputs)
        require(n >= 0 && n < (int)r->numChannels && seen.insert(n).second, "Invalid physical subset");
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    juce::StringPairArray meta;
    meta.set(juce::WavAudioFormat::bwavDescription,
             "SoundPlus layout:" + i.layout.id + "; order:" + i.layout.roles.joinIntoString(","));
    auto stream = file.createOutputStream();
    require(stream != nullptr, "Cannot stage input");
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(stream.release(), r->sampleRate, juceChannelSet(i.layout), 32, meta, 0));
    require(w != nullptr, "Cannot encode input");
    juce::AudioBuffer<float> physical((int)r->numChannels, 4096), ordered(i.layout.size(), 4096);
    auto order = wavOrder(i.layout);
    for (int64_t at = i.first; at < last; at += 4096)
    {
        require(!cancel(), "Cancelled");
        int n = (int)std::min<int64_t>(4096, last - at);
        require(r->read(&physical, 0, n, at, true, true), "Source decode failed");
        ordered.clear();
        for (int c = 0; c < i.layout.size(); ++c)
            for (int s = 0; s < n; ++s)
            {
                float x = 0;
                if (i.mean)
                {
                    for (int p = 0; p < (int)r->numChannels; ++p)
                        x += physical.getSample(p, s) / (float)r->numChannels;
                }
                else
                    x = physical.getSample(i.inputs[(size_t)c], s);
                require(std::isfinite(x), "Non-finite input");
                ordered.setSample(order[(size_t)c], s, x);
            }
        require(w->writeFromAudioSampleBuffer(ordered, 0, n), "Input disk write failed");
    }
    w.reset();
    require(before == processingHash(source), "Input changed during preparation");
    auto o = object();
    set(o, "version", 1);
    set(o, "selection", pluginInputJson(i));
    set(o, "sourceHash", before);
    set(o, "path", file.getFullPathName());
    set(o, "sha256", processingHash(file));
    set(o, "frames", last - i.first);
    set(o, "rate", r->sampleRate);
    set(o, "layout", layoutJson(i.layout));
    return o;
}
juce::var PluginWorker::call(juce::var request, const juce::File &workspace, int timeout,
                             const std::function<bool()> &cancel)
{
    require(executable.existsAsFile(), "Bundled plugin worker is missing");
    workspace.createDirectory();
    auto id = juce::Uuid().toString();
    auto in = workspace.getChildFile(id + "-request.json"), out = workspace.getChildFile(id + "-reply.json");
    lastRequest = in;
    lastDiagnostics = object();
    auto diagnostics = workspace.getChildFile(id + "-diagnostic.json");
    auto health = workspace.getChildFile(id + "-health");
    auto checkpoint = workspace.getChildFile(id + "-editor-recovery.soundplus-insert");
    auto log = workspace.getChildFile(id + "-stderr.log");
    set(request, "diagnostic", diagnostics.getFullPathName());
    if (request["mode"].toString() == "editor")
    {
        set(request, "health", health.getFullPathName());
        set(request, "checkpoint", checkpoint.getFullPathName());
    }
    set(request, "protocol", 1);
    set(request, "reply", out.getFullPathName());
    saveProcessingJson(in, request);
    auto exe = executable.getFullPathName().toStdString(), arg = in.getFullPathName().toStdString();
    char *args[]{exe.data(), arg.data(), nullptr};
    pid_t pid = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.getFullPathName().toRawUTF8(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    auto spawned = posix_spawn(&pid, exe.c_str(), &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    require(spawned == 0, "Cannot launch plugin worker");
    auto start = juce::Time::getMillisecondCounterHiRes();
    int status = 0;
    bool done = false, memoryLimit = false;
    struct rusage usage
    {
    };
    bool unhealthy = false;
    while ((timeout <= 0 || juce::Time::getMillisecondCounterHiRes() - start < timeout) && !cancel())
    {
        auto n = wait4(pid, &status, WNOHANG, &usage);
        if (n == pid)
        {
            done = true;
            break;
        }
        if (n < 0 && errno != EINTR)
            break;
        if (timeout <= 0 && juce::Time::getMillisecondCounterHiRes() - start > 30000 &&
            (!health.existsAsFile() || juce::Time::getCurrentTime().toMilliseconds() -
                                               health.getLastModificationTime().toMilliseconds() >
                                           15000))
        {
            unhealthy = true;
            break;
        }
        rusage_info_v4 footprint{};
        if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&footprint) == 0 &&
            footprint.ri_phys_footprint > 1024ULL * 1024 * 1024)
        {
            memoryLimit = true;
            break;
        }
        juce::Thread::sleep(10);
    }
    auto retainDiagnostic = [&]
    {
        try
        {
            if (diagnostics.existsAsFile())
                lastDiagnostics = loadProcessingJson(diagnostics);
        }
        catch (...)
        {
        }
        set(lastDiagnostics, "request", in.getFullPathName());
        set(lastDiagnostics, "stderr", log.getFullPathName());
        set(lastDiagnostics, "elapsedMs", juce::Time::getMillisecondCounterHiRes() - start);
        set(lastDiagnostics, "exit", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        if (checkpoint.existsAsFile())
            set(lastDiagnostics, "editorCheckpoint", checkpoint.getFullPathName());
    };
    if (!done)
    {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {
        };
        retainDiagnostic();
        set(lastDiagnostics, "failureKind",
            cancel()      ? "cancelled"
            : unhealthy   ? "editor-unresponsive"
            : memoryLimit ? "memory-limit"
                          : "timeout");
        auto detail = " / stage: " + lastDiagnostics["stage"].toString() +
                      " / diagnostics: " + diagnostics.getFullPathName();
        throw std::runtime_error(
            (cancel()      ? "Cancelled"
             : memoryLimit ? "Plugin worker exceeded 1 GiB resident-memory limit; candidate quarantined"
             : unhealthy   ? "Plugin editor heartbeat stalled; recoverable checkpoint retained"
                           : "Plugin worker timed out; candidate quarantined") +
            detail.toStdString());
    }
    retainDiagnostic();
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0 && out.existsAsFile(),
            "Plugin worker crashed or exited without a result; candidate quarantined");
    auto reply = loadProcessingJson(out);
    require((int)reply["protocol"] == 1, "Incompatible plugin worker");
    if (reply["diagnostics"].isObject())
    {
        auto value = reply["diagnostics"].clone();
        for (auto key : {"request", "stderr", "elapsedMs", "exit", "editorCheckpoint"})
            if (lastDiagnostics.hasProperty(key))
                set(value, key, lastDiagnostics[key]);
        lastDiagnostics = value;
    }
    auto error = reply["error"].toString();
    require(error.isEmpty(), error + " / stage: " + lastDiagnostics["stage"].toString());
    if (lastDiagnostics["editorCheckpoint"].toString().isNotEmpty())
        set(reply, "editorCheckpoint", lastDiagnostics["editorCheckpoint"]);
    set(reply, "workerMs", juce::Time::getMillisecondCounterHiRes() - start);
    set(reply, "workerPeakBytes", (juce::int64)usage.ru_maxrss);
    set(reply, "workerCpuMs",
        usage.ru_utime.tv_sec * 1000. + usage.ru_utime.tv_usec / 1000. + usage.ru_stime.tv_sec * 1000. +
            usage.ru_stime.tv_usec / 1000.);
    return reply;
}
namespace
{
juce::String canonicalRegistry(const juce::File &file)
{
    file.getParentDirectory().createDirectory();
    char resolved[PATH_MAX]{};
    return (::realpath(file.getParentDirectory().getFullPathName().toRawUTF8(), resolved)
                ? juce::File(juce::String::fromUTF8(resolved)).getChildFile(file.getFileName())
                : file)
        .getFullPathName();
}
struct RegistryGuard
{
    std::shared_ptr<std::mutex> mutex;
    std::unique_lock<std::mutex> local;
    juce::InterProcessLock process;
    static std::shared_ptr<std::mutex> shared(const juce::String &key)
    {
        static std::mutex tableMutex;
        static std::map<juce::String, std::weak_ptr<std::mutex>> table;
        std::lock_guard lock(tableMutex);
        auto value = table[key].lock();
        if (!value)
        {
            value = std::make_shared<std::mutex>();
            table[key] = value;
        }
        return value;
    }
    RegistryGuard(const juce::File &file, const std::function<bool()> &cancel)
        : mutex(shared(canonicalRegistry(file))), local(*mutex, std::defer_lock),
          process("SoundPlus-plugin-registry-" + juce::String(canonicalRegistry(file).hashCode64()))
    {
        while (!local.try_lock())
        {
            if (cancel())
                throw QueryCancelled();
            juce::Thread::sleep(5);
        }
        while (!process.enter(50))
            if (cancel())
                throw QueryCancelled();
    }
    ~RegistryGuard()
    {
        process.exit();
    }
};
void appendRoots(std::vector<juce::File> &roots, const juce::var &saved)
{
    if (auto *array = saved.getArray())
        for (auto &v : *array)
        {
            juce::File f(v.toString());
            if (std::find(roots.begin(), roots.end(), f) == roots.end())
                roots.push_back(f);
        }
}
} // namespace
juce::var PluginCatalog::snapshot() const
{
    return file.existsAsFile() ? loadProcessingJson(file) : object();
}
juce::var PluginCatalog::scan(PluginWorker &worker, const std::vector<juce::File> &requestedRoots, bool retry,
                              const std::function<bool()> &cancel,
                              const std::function<void(juce::var)> &progress, bool registeredAudioUnits,
                              const juce::String &targetPath)
{
    juce::var cache;
    std::vector<juce::File> roots = requestedRoots;
    int64_t operation = 0;
    {
        RegistryGuard coordinated(file, cancel);
        cache = snapshot();
        // Durable ordinal, reserved under the same canonical interprocess lock.
        // Legacy monotonic-clock operations are epoch 0, irrespective of size.
        auto counterFile = juce::File(canonicalRegistry(file) + ".operations.json");
        auto counter = counterFile.existsAsFile() ? loadProcessingJson(counterFile) : object();
        int64_t latestOrdinal = (int64_t)counter["next"];
        if (auto *existing = cache["entries"].getArray())
            for (auto &entry : *existing)
                if ((int)entry["registryOrderEpoch"] == 1)
                    latestOrdinal = std::max(latestOrdinal, (int64_t)entry["registryOperation"]);
        latestOrdinal = std::max(latestOrdinal, (int64_t)cache["fullScanOrdinal"]);
        require(latestOrdinal >= 0 && latestOrdinal < std::numeric_limits<int64_t>::max(),
                "Plugin registry operation counter exhausted");
        operation = latestOrdinal + 1;
        set(counter, "next", (juce::int64)operation);
        saveProcessingJson(counterFile, counter);
        appendRoots(roots, cache["roots"]);
    }
    if (afterSnapshot)
        afterSnapshot();
    juce::Array<juce::var> entries;
    juce::StringArray seen;
    int launched = 0, hits = 0;
    std::vector<juce::File> candidates;
    for (auto &root : roots)
    {
        if (root.hasFileExtension("vst3"))
            candidates.push_back(root);
        else
            for (auto &p : root.findChildFiles(juce::File::findDirectories, true, "*.vst3"))
            {
                // A plug-in bundle is an opaque candidate. Embedded implementation
                // bundles are not additional user-installed processors.
                bool embedded = false;
                for (auto parent = p.getParentDirectory();
                     parent != root && parent != parent.getParentDirectory();
                     parent = parent.getParentDirectory())
                    if (parent.hasFileExtension("vst3"))
                        embedded = true;
                if (!embedded)
                    candidates.push_back(p);
            }
    }
    std::vector<juce::var> descriptions;
    for (auto &p : candidates)
    {
        auto descriptor = object();
        set(descriptor, "path", p.getFullPathName());
        set(descriptor, "format", "VST3");
        descriptions.push_back(descriptor);
    }
    bool targetedAU = targetPath.startsWith("AudioUnit:");
    if (targetPath.isNotEmpty())
        if (auto *old = cache["entries"].getArray())
            for (auto &v : *old)
                if (v["path"].toString() == targetPath)
                    targetedAU = v["format"].toString() == "AudioUnit" || targetedAU;
    juce::Array<juce::var> registrations;
    if ((registeredAudioUnits && targetPath.isEmpty()) || targetedAU)
    {
        auto request = object();
        set(request, "mode", "enumerate-au");
        auto registry = worker.call(request, file.getParentDirectory().getChildFile("scan"), 30000, cancel);
        if (auto *units = registry["candidates"].getArray())
            registrations = *units;
    }
    if (targetPath.isEmpty())
        for (auto &unit : registrations)
            descriptions.push_back(unit);
    constexpr int probePolicy = pluginProbePolicy;
    if (targetPath.isNotEmpty())
    {
        descriptions.clear();
        bool found = false;
        if (auto *old = cache["entries"].getArray())
            for (auto &v : *old)
            {
                auto descriptor = v.clone();
                if (descriptor["format"].toString().isEmpty())
                    set(descriptor, "format",
                        v["path"].toString().startsWith("AudioUnit:") ? "AudioUnit" : "VST3");
                if (v["path"].toString() == targetPath)
                {
                    found = true;
                    if (targetedAU)
                    {
                        descriptor = object();
                        set(descriptor, "path", targetPath);
                        set(descriptor, "format", "AudioUnit");
                        set(descriptor, "error", "Audio Unit is no longer registered");
                        for (auto &unit : registrations)
                            if (unit["path"].toString() == targetPath)
                                descriptor = unit.clone();
                    }
                }
                descriptions.push_back(descriptor);
            }
        require(found, "Selected plugin is no longer in this registry; run Rescan");
    }
    else
    {
        juce::StringArray legacy;
        for (auto &root : roots)
        {
            auto directory = root.getSiblingFile("VST");
            if (directory.isDirectory())
                for (auto &f : directory.findChildFiles(juce::File::findDirectories, true, "*.vst"))
                    legacy.addIfNotAlreadyThere(f.getFullPathName());
        }
        for (auto &path : legacy)
        {
            auto d = object();
            set(d, "path", path);
            set(d, "format", "VST2");
            descriptions.push_back(d);
        }
    }
    require(descriptions.size() <= 2048, "Too many plugin candidates (limit 2048)");
    for (auto &descriptor : descriptions)
    {
        if (cancel())
            throw QueryCancelled();
        auto path = descriptor["path"].toString();
        if (seen.contains(path))
            continue;
        seen.add(path);
        if (targetPath.isNotEmpty() && path != targetPath)
            continue; // Retry publishes only the touched target, never stale siblings.
        auto e = descriptor.clone();
        // Preserve logs as bounded history, never as the current success/failure.
        if (auto *old = cache["entries"].getArray())
            for (auto &v : *old)
                if (v["path"].toString() == path)
                {
                    auto history = v["failureHistory"].isArray() ? *v["failureHistory"].getArray()
                                                                 : juce::Array<juce::var>();
                    if (v["error"].toString().isNotEmpty())
                    {
                        auto past = object();
                        set(past, "attempt", v["lastAttempt"]);
                        set(past, "error", v["error"]);
                        set(past, "stage", v["failureStage"]);
                        set(past, "diagnostics", v["diagnostics"]);
                        history.add(past);
                        while (history.size() > 8)
                            history.remove(0);
                    }
                    if (!history.isEmpty())
                        set(e, "failureHistory", history);
                }
        juce::String metadataStage = "registration";
        bool reusedEntry = false;
        set(e, "path", path);
        set(e, "format", descriptor["format"]);
        try
        {
            bool au = descriptor["format"].toString() == "AudioUnit";
            if (!au && descriptor["format"].toString() != "VST3")
            {
                set(e, "error",
                    "Legacy VST2 adapter unavailable: required SDK/distribution path not established");
                set(e, "failureStage", "unsupported-format");
                set(e, "quarantined", false);
                entries.add(e);
                continue;
            }
            if (au)
                require(descriptor["error"].toString().isEmpty(), descriptor["error"].toString());
            metadataStage = "bundle-fingerprint";
            auto bundle = au ? juce::File(descriptor["bundle"].toString()) : juce::File(path);
            auto hash = pluginFingerprint(bundle);
            if (au)
                set(descriptor, "fingerprint", hash); // Fresh expected identity; worker checks it again.
            char canonical[PATH_MAX]{};
            auto identity = au ? path.fromLastOccurrenceOf("/", false, false)
                               : (::realpath(path.toRawUTF8(), canonical) ? juce::String::fromUTF8(canonical)
                                                                          : bundle.getFullPathName());
            set(e, "canonicalIdentity", identity);
            require(hash.isNotEmpty(), "Missing plugin bundle fingerprint");
            bool reused = false;
            if (auto *old = cache["entries"].getArray())
                for (auto &v : *old)
                    if (v["path"].toString() == path && v["fingerprint"].toString() == hash &&
                        (int)v["probePolicy"] == probePolicy && targetPath.isEmpty() &&
                        !(retry && (v["error"].toString().isNotEmpty() ||
                                    [&]
                                    {
                                        if (auto *ps = v["plugins"].getArray())
                                            for (auto &p : *ps)
                                                if (p["reason"].toString().isNotEmpty() ||
                                                    p["layouts"].size() == 0)
                                                    return true;
                                        return false;
                                    }())))
                    {
                        e = v;
                        reused = true;
                        reusedEntry = true;
                        break;
                    }
            if (reused)
                ++hits;
            else
            {
                ++launched;
                set(e, "probePolicy", probePolicy);
                set(e, "lastAttempt", juce::Time::getCurrentTime().toISO8601(true));
                set(e, "fingerprint", hash);
                auto cmd = descriptor.clone();
                set(cmd, "mode", "scan");
                set(cmd, "path", path);
                try
                {
                    auto result =
                        worker.call(cmd, file.getParentDirectory().getChildFile("scan"), 15000, cancel);
                    set(e, "plugins", result["plugins"]);
                    set(e, "scanMs", result["workerMs"]);
                    set(e, "workerPeakBytes", result["workerPeakBytes"]);
                    set(e, "architecture", worker.lastDiagnostics["architecture"]);
                    set(e, "diagnostics", worker.lastDiagnostics);
                    set(e, "quarantined", false);
                    set(e, "error", "");
                    e.getDynamicObject()->removeProperty("failureStage");
                    e.getDynamicObject()->removeProperty("failureKind");
                }
                catch (const std::exception &x)
                {
                    if (cancel())
                        throw QueryCancelled();
                    set(e, "error", juce::String(x.what()));
                    set(e, "diagnostics", worker.lastDiagnostics);
                    set(e, "failureStage", worker.lastDiagnostics["stage"]);
                    set(e, "architecture", worker.lastDiagnostics["architecture"]);
                    set(e, "quarantined",
                        worker.lastDiagnostics["failureKind"].toString() == "timeout" ||
                            worker.lastDiagnostics["failureKind"].toString() == "memory-limit" ||
                            worker.lastDiagnostics["exit"].toString() != "0");
                }
            }
        }
        catch (const QueryCancelled &)
        {
            throw;
        }
        catch (const std::exception &x)
        {
            if (cancel())
                throw QueryCancelled();
            set(e, "error", juce::String(x.what()));
            set(e, "failureStage", metadataStage);
            set(e, "lastAttempt", juce::Time::getCurrentTime().toISO8601(true));
            set(e, "quarantined", false);
        }
        if (auto *plugins = e["plugins"].getArray())
            for (auto &plugin : *plugins)
            {
                if (plugin["format"].toString() == "AudioUnit" && !plugin.hasProperty("auVersion"))
                    set(plugin, "auVersion",
                        juce::File(plugin["bundle"].toString()).hasFileExtension("appex") ? 3 : 2);
                if (!plugin.hasProperty("category"))
                    if (auto xml = juce::parseXML(plugin["xml"].toString()))
                        set(plugin, "category", xml->getStringAttribute("category"));
            }
        if (!reusedEntry)
        {
            set(e, "registryOperation", (juce::int64)operation);
            set(e, "registryOrderEpoch", 1);
        }
        entries.add(e);
        if (progress)
            progress(e);
    }
    juce::Array<juce::var> savedRoots;
    for (auto &root : roots)
        savedRoots.add(root.getFullPathName());
    auto result = object();
    set(result, "version", 1);
    set(result, "registeredAudioUnits",
        targetPath.isEmpty() ? juce::var(registeredAudioUnits) : cache["registeredAudioUnits"]);
    set(result, "entries", entries);
    set(result, "roots", targetPath.isEmpty() ? juce::var(savedRoots) : cache["roots"]);
    set(result, "launched", launched);
    set(result, "cacheHits", hits);
    // Commit only a complete snapshot, including roots. Interrupted scans leave
    // the last complete registry byte-identical, including unvisited candidates.
    if (cancel())
        throw QueryCancelled();
    {
        RegistryGuard coordinated(file, cancel);
        if (cancel())
            throw QueryCancelled();
        auto latest = snapshot();
        std::map<juce::String, juce::var> reconciled;
        if (auto *current = latest["entries"].getArray())
            for (auto &e : *current)
                reconciled[e["path"].toString()] = e;
        auto ordinal = [](const juce::var &entry) -> int64_t
        { return (int)entry["registryOrderEpoch"] == 1 ? (int64_t)entry["registryOperation"] : 0; };
        const auto deletionBarrier = (int64_t)latest["fullScanOrdinal"];
        if (targetPath.isEmpty())
            std::erase_if(reconciled,
                          [&](auto &item)
                          {
                              return !seen.contains(item.first) && ordinal(item.second) <= operation &&
                                     (int64_t)item.second["registryPresenceOrdinal"] <= operation;
                          });
        for (auto &e : entries)
        {
            auto path = e["path"].toString();
            auto current = reconciled.find(path);
            if (current == reconciled.end() && deletionBarrier > operation)
                continue; // A newer complete scan explicitly observed it absent.
            if (current == reconciled.end() || ordinal(e) >= ordinal(current->second))
                reconciled[path] = e;
            auto &present = reconciled[path];
            set(present, "registryPresenceOrdinal",
                (juce::int64)std::max(operation, (int64_t)present["registryPresenceOrdinal"]));
        }
        set(result, "fullScanOrdinal",
            (juce::int64)(targetPath.isEmpty() ? std::max(operation, deletionBarrier) : deletionBarrier));
        juce::Array<juce::var> published;
        // Keep operation discovery ordering, then any newly published candidates.
        for (auto &e : entries)
        {
            auto at = reconciled.find(e["path"].toString());
            if (at != reconciled.end())
            {
                published.add(at->second);
                reconciled.erase(at);
            }
        }
        for (auto &[path, e] : reconciled)
            published.add(e);
        appendRoots(roots, latest["roots"]);
        juce::Array<juce::var> finalRoots;
        for (auto &root : roots)
            finalRoots.add(root.getFullPathName());
        set(result, "entries", published);
        set(result, "roots",
            targetPath.isNotEmpty() && latest["roots"].isArray() ? latest["roots"] : juce::var(finalRoots));
        set(result, "registryRevision", (juce::int64)((int64_t)latest["registryRevision"] + 1));
        set(result, "registeredAudioUnits", registeredAudioUnits || (bool)latest["registeredAudioUnits"]);
        saveProcessingJson(file, result);
    }
    return result;
}
juce::var publishPluginCopy(const juce::var &result, const juce::File &folder, bool normalize, bool split,
                            const std::function<bool()> &cancel, const std::function<void()> &afterStaging)
{
    auto stage = juce::File(result["path"].toString());
    require(processingHash(stage) == result["sha256"].toString(), "Processed artifact changed");
    auto layout = layoutFromJson(result["layout"]);
    validateLayout(layout);
    auto destination = folder.getChildFile("Processed-" + juce::Uuid().toString());
    auto pending = folder.getChildFile(".staging-" + juce::Uuid().toString());
    require(pending.createDirectory().wasOk(), "Cannot create delivery staging directory");
    try
    {
        if (afterStaging)
            afterStaging();
        require(!cancel(), "Cancelled");
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(stage));
        require(r != nullptr, "Cannot read completed PCM");
        double gain =
            normalize && (double)result["peak"] > 0 ? std::pow(10., -1. / 20) / (double)result["peak"] : 1.;
        juce::WavAudioFormat wav;
        juce::StringPairArray meta;
        meta.set(juce::WavAudioFormat::bwavDescription,
                 "SoundPlus layout:" + layout.id + "; order:" + layout.roles.joinIntoString(","));
        std::unique_ptr<juce::AudioFormatWriter> master;
        std::vector<std::unique_ptr<juce::AudioFormatWriter>> stems;
        if (!normalize)
            require(stage.copyFileTo(pending.getChildFile("Mix.wav")), "Cannot copy authoritative PCM");
        else
            master.reset(wav.createWriterFor(pending.getChildFile("Mix.wav").createOutputStream().release(),
                                             r->sampleRate, juceChannelSet(layout), 32, meta, 0));
        require(!normalize || master != nullptr, "Cannot encode normalized delivery");
        if (split)
            for (auto &role : layout.roles)
            {
                stems.emplace_back(
                    wav.createWriterFor(pending.getChildFile(role + ".wav").createOutputStream().release(),
                                        r->sampleRate, 1, 32, {}, 0));
                require(stems.back() != nullptr, "Cannot encode split delivery");
            }
        juce::AudioBuffer<float> b(layout.size(), 4096);
        auto order = wavOrder(layout);
        if (normalize || split)
            for (int64_t at = 0; at < r->lengthInSamples; at += 4096)
            {
                require(!cancel(), "Cancelled");
                int n = (int)std::min<int64_t>(4096, r->lengthInSamples - at);
                require(r->read(&b, 0, n, at, true, true), "Processed read failed");
                b.applyGain((float)gain);
                if (master)
                    require(master->writeFromAudioSampleBuffer(b, 0, n), "Delivery write failed");
                for (size_t c = 0; c < stems.size(); ++c)
                {
                    const float *data[]{b.getReadPointer(order[c])};
                    require(stems[c]->writeFromFloatArrays(data, 1, n), "Split write failed");
                }
            }
        master.reset();
        stems.clear();
        require(!cancel(), "Cancelled");
        require(processingHash(stage) == result["sha256"].toString(),
                "Processed artifact changed before publication");
        auto receipt = result.clone();
        set(receipt, "kind", "plugin-copy-v1");
        set(receipt, "authoritativePcmHash", result["sha256"]);
        set(receipt, "normalizationGain", gain);
        set(receipt, "rawPeak", result["peak"]);
        set(receipt, "peak", (double)result["peak"] * gain);
        if (normalize)
            set(receipt, "clippedSamples", (juce::int64)0);
        set(receipt, "path", destination.getChildFile("Mix.wav").getFullPathName());
        set(receipt, "sha256", processingHash(pending.getChildFile("Mix.wav")));
        juce::Array<juce::var> files;
        if (split)
            for (auto &role : layout.roles)
            {
                auto item = object();
                set(item, "role", role);
                set(item, "path", destination.getChildFile(role + ".wav").getFullPathName());
                set(item, "sha256", processingHash(pending.getChildFile(role + ".wav")));
                files.add(item);
            }
        set(receipt, "files", files);
        saveProcessingJson(pending.getChildFile("receipt.json"), receipt);
        require(pending.moveFileTo(destination), "Cannot atomically publish delivery");
        return receipt;
    }
    catch (...)
    {
        pending.deleteRecursively();
        throw;
    }
}
} // namespace sp

namespace sp
{
juce::Array<juce::var> pluginCandidateRows(const juce::var &snapshot, const ChannelLayout &bus,
                                           const juce::String &query, bool attentionOnly)
{
    juce::Array<juce::var> rows;
    if (auto *entries = snapshot["entries"].getArray())
        for (auto &entry : *entries)
        {
            juce::Array<juce::var> classes;
            if (auto *ps = entry["plugins"].getArray())
                classes = *ps;
            if (classes.isEmpty())
            {
                auto p = object();
                auto path = entry["path"].toString();
                set(p, "name",
                    path.startsWith("AudioUnit:") ? path.fromFirstOccurrenceOf(":", false, false)
                                                  : juce::File(path).getFileNameWithoutExtension());
                set(p, "format",
                    entry["format"].toString().isEmpty()
                        ? (path.startsWith("AudioUnit:") ? "AudioUnit" : "VST3")
                        : entry["format"]);
                set(p, "architecture", entry["architecture"]);
                classes.add(p);
            }
            for (auto &p : classes)
            {
                auto row = object();
                set(row, "plugin", p);
                set(row, "path", entry["path"]);
                set(row, "canonicalIdentity",
                    entry["canonicalIdentity"].toString().isNotEmpty() ? entry["canonicalIdentity"]
                                                                       : entry["path"]);
                set(row, "name", p["name"]);
                set(row, "vendor", p["vendor"].toString().isEmpty() ? "Vendor not reported" : p["vendor"]);
                set(row, "format", pluginFormatLabel(p));
                set(row, "architecture",
                    p["architecture"].toString().isNotEmpty()       ? p["architecture"]
                    : entry["architecture"].toString().isNotEmpty() ? entry["architecture"]
                                                                    : juce::var("Not established"));
                auto reason = entry["error"].toString().isNotEmpty() ? entry["error"].toString()
                                                                     : p["reason"].toString();
                juce::String state = "Available", stageName = p["failureStage"].toString();
                if (stageName.isEmpty())
                    stageName = entry["failureStage"].toString();
                if (stageName.isEmpty())
                    stageName = entry["diagnostics"]["stage"].toString();
                if (p["format"].toString() == "VST2")
                    state = "Unsupported format";
                else if (reason.containsIgnoreCase("arm64 required"))
                    state = "Unsupported architecture";
                else if ((bool)entry["quarantined"])
                    state = "Quarantined";
                else if (reason.isNotEmpty())
                    state = "Failed";
                else if ((int)entry["probePolicy"] != pluginProbePolicy)
                    state = "Needs rescan";
                else
                {
                    bool compatible = false;
                    if (auto *layouts = p["layouts"].getArray())
                        for (auto &l : *layouts)
                            if (l.toString() == bus.id)
                                compatible = true;
                    if (!compatible)
                    {
                        state = "Incompatible layout";
                        reason = "Requested " + layoutName(bus) +
                                 "; no exact supported main layout. No conversion will be applied.";
                        stageName = "main-bus-negotiation";
                    }
                }
                if (state == "Needs rescan")
                    reason = "Cached result predates the current host probing policy";
                set(row, "status", state);
                set(row, "reason", reason);
                set(row, "stage", stageName);
                set(row, "canInsert", state == "Available");
                set(row, "lastAttempt", entry["lastAttempt"]);
                set(row, "layouts", p["layouts"]);
                set(row, "diagnostics", entry["diagnostics"]);
                auto haystack = p["name"].toString() + p["vendor"].toString() + p["category"].toString() +
                                p["format"].toString() + state + reason + entry["path"].toString();
                set(row, "matchesFilter",
                    haystack.containsIgnoreCase(query) && (!attentionOnly || state != "Available"));
                rows.add(row);
            }
        }
    juce::Array<juce::var> identities;
    for (auto &row : rows)
        identities.add(row["plugin"]);
    auto names = pluginDisplayNames(identities);
    for (int i = 0; i < rows.size(); ++i)
        set(rows.getReference(i), "displayName", names[i]);
    for (int i = rows.size(); --i >= 0;)
        if (!(bool)rows[i]["matchesFilter"])
            rows.remove(i);
    std::sort(rows.begin(), rows.end(),
              [](auto &a, auto &b)
              {
                  if ((bool)a["canInsert"] != (bool)b["canInsert"])
                      return (bool)a["canInsert"];
                  return a["name"].toString().compareIgnoreCase(b["name"].toString()) < 0;
              });
    return rows;
}
} // namespace sp
