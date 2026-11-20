#include "PluginProcessing.h"
#include <juce_cryptography/juce_cryptography.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <set>
#include <cmath>
extern char **environ;
namespace sp
{
std::vector<juce::File> defaultVst3Locations()
{
    return {juce::File("/Library/Audio/Plug-Ins/VST3"),
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Library/Audio/Plug-Ins/VST3")};
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
    set(request, "protocol", 1);
    set(request, "reply", out.getFullPathName());
    saveProcessingJson(in, request);
    auto exe = executable.getFullPathName().toStdString(), arg = in.getFullPathName().toStdString();
    char *args[]{exe.data(), arg.data(), nullptr};
    pid_t pid = -1;
    require(posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args, environ) == 0,
            "Cannot launch plugin worker");
    auto start = juce::Time::getMillisecondCounterHiRes();
    int status = 0;
    bool done = false, memoryLimit = false;
    struct rusage usage
    {
    };
    while (juce::Time::getMillisecondCounterHiRes() - start < timeout && !cancel())
    {
        auto n = wait4(pid, &status, WNOHANG, &usage);
        if (n == pid)
        {
            done = true;
            break;
        }
        if (n < 0 && errno != EINTR)
            break;
        rusage_info_v4 footprint{};
        if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&footprint) == 0 &&
            footprint.ri_phys_footprint > 1024ULL * 1024 * 1024)
        {
            memoryLimit = true;
            break;
        }
        juce::Thread::sleep(10);
    }
    if (!done)
    {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {
        };
        throw std::runtime_error(
            cancel()      ? "Cancelled"
            : memoryLimit ? "Plugin worker exceeded 1 GiB resident-memory limit; candidate quarantined"
                          : "Plugin worker timed out; candidate quarantined");
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0 && out.existsAsFile(),
            "Plugin worker crashed or exited without a result; candidate quarantined");
    auto reply = loadProcessingJson(out);
    require((int)reply["protocol"] == 1, "Incompatible plugin worker");
    auto error = reply["error"].toString();
    require(error.isEmpty(), error);
    set(reply, "workerMs", juce::Time::getMillisecondCounterHiRes() - start);
    set(reply, "workerPeakBytes", (juce::int64)usage.ru_maxrss);
    set(reply, "workerCpuMs",
        usage.ru_utime.tv_sec * 1000. + usage.ru_utime.tv_usec / 1000. + usage.ru_stime.tv_sec * 1000. +
            usage.ru_stime.tv_usec / 1000.);
    return reply;
}
juce::var PluginCatalog::snapshot() const
{
    return file.existsAsFile() ? loadProcessingJson(file) : object();
}
juce::var PluginCatalog::scan(PluginWorker &worker, const std::vector<juce::File> &roots, bool retry,
                              const std::function<bool()> &cancel,
                              const std::function<void(juce::var)> &progress)
{
    auto cache = snapshot();
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
                candidates.push_back(p);
    }
    require(candidates.size() <= 2048, "Too many plugin candidates (limit 2048)");
    for (auto &p : candidates)
    {
        if (cancel())
            throw QueryCancelled();
        auto path = p.getFullPathName();
        if (seen.contains(path))
            continue;
        seen.add(path);
        auto e = object();
        set(e, "path", path);
        try
        {
            auto hash = pluginFingerprint(p);
            bool reused = false;
            if (auto *old = cache["entries"].getArray())
                for (auto &v : *old)
                    if (v["path"].toString() == path && v["fingerprint"].toString() == hash &&
                        !(retry && v["error"].toString().isNotEmpty()))
                    {
                        e = v;
                        reused = true;
                        break;
                    }
            if (reused)
                ++hits;
            else
            {
                ++launched;
                set(e, "fingerprint", hash);
                auto cmd = object();
                set(cmd, "mode", "scan");
                set(cmd, "path", path);
                try
                {
                    auto result =
                        worker.call(cmd, file.getParentDirectory().getChildFile("scan"), 15000, cancel);
                    set(e, "plugins", result["plugins"]);
                    set(e, "scanMs", result["workerMs"]);
                    set(e, "workerPeakBytes", result["workerPeakBytes"]);
                    set(e, "architecture", "arm64");
                    set(e, "error", "");
                }
                catch (const std::exception &x)
                {
                    if (cancel())
                        throw QueryCancelled();
                    set(e, "error", juce::String(x.what()));
                    set(e, "quarantined", true);
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
            set(e, "quarantined", true);
        }
        if (auto *plugins = e["plugins"].getArray())
            for (auto &plugin : *plugins)
                if (!plugin.hasProperty("category"))
                    if (auto xml = juce::parseXML(plugin["xml"].toString()))
                        set(plugin, "category", xml->getStringAttribute("category"));
        entries.add(e);
        if (progress)
            progress(e);
    }
    juce::Array<juce::var> savedRoots;
    for (auto &root : roots)
        savedRoots.add(root.getFullPathName());
    auto result = object();
    set(result, "version", 1);
    set(result, "entries", entries);
    set(result, "roots", savedRoots);
    set(result, "launched", launched);
    set(result, "cacheHits", hits);
    // Commit only a complete snapshot, including roots. Interrupted scans leave
    // the last complete registry byte-identical, including unvisited candidates.
    if (cancel())
        throw QueryCancelled();
    saveProcessingJson(file, result);
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
