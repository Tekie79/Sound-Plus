#include "MetadataFields.h"
#include "LibraryService.h"
#include "RetrievalIndex.h"
#include "MacSources.h"
#include "Performance.h"
#include "AIProcess.h"
#include "AudioIPC.h"
#include "ClapRuntime.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <set>
#include <sys/stat.h>
#include <sys/mount.h>
#include <thread>
#include <cstdio>
#include <map>

namespace sp
{
namespace
{
namespace fs = std::filesystem;
bool same(const FileStamp &a, const FileStamp &b)
{
    return a.size == b.size && a.modified == b.modified && a.inode == b.inode;
}
FileStamp stampOf(const Asset &a)
{
    return {a.size, a.modified, a.inode};
}
juce::String childPath(const juce::String &directory, const juce::String &child)
{
    return directory.isEmpty() ? child : directory + "/" + child;
}
} // namespace
struct LibraryService::Impl
{
    struct Runtime
    {
        Root root;
        std::set<juce::String> pending;
        bool full = true, force = false, active = false, local = true, watchWorking = true;
        double lastCheck = 0, lastDirectories = monotonicMs(), lastRecovery = monotonicMs(), started = 0,
               pendingDue = 0;
        std::atomic<int> stage{0};
        std::atomic<bool> cancelled{false};
        std::atomic<int> directoriesQueued{0};
        std::unique_ptr<SourceWatch> watch;
        juce::String watchedPath;
    };
    struct Job
    {
        std::shared_ptr<Runtime> runtime;
        Root root;
        std::set<juce::String> dirs;
        bool force = false, probe = false, targetsForced = false;
    };
    struct Name
    {
        juce::String relative, rename;
        FileStamp stamp;
    };
    struct Header
    {
        juce::String relative;
        FileStamp stamp;
        double seconds = 0, rate = 0;
        int channels = 0;
        ChannelLayout layout;
        juce::String confidence;
        juce::var embedded;
        juce::String error;
        int64_t samples = 0;
    };
    struct Batch
    {
        std::shared_ptr<Runtime> runtime;
        Root root;
        std::vector<Name> names;
        std::vector<Header> headers;
        std::optional<DirectoryRecord> directory;
        juce::String removed;
        bool rootInfo = false, finished = false, local = true, watchWorking = true;
        int64_t scan = 0;
        std::shared_ptr<std::promise<void>> barrier;
        std::optional<AnalysisUpdate> analysis;
        bool seedAnalysis = false, retryAnalysis = false;
    };
    juce::File database;
    juce::File modelDirectory;
    std::unique_ptr<AIProcess> inference;
    std::thread analysisWorker, semanticReader;
    std::atomic<bool> aiLoaded{false}, aiAnalyzing{false}, installCancelled{false}, indexWorking{false};
    std::atomic<bool> auditionActive{false};
    std::atomic<uint64_t> analysisGeneration{0};
    bool installRequested = false, aiRetryRequested = false;
    juce::File importModelDirectory;
    juce::String preferredAnalysis;
    double semanticDue = 0;
    uint64_t semanticInputVersion = 0, semanticResultVersion = 0;
    std::atomic<uint64_t> semanticWorkRequest{0}, semanticHandledInput{0}, readerSession{0},
        readerSemanticSeen{0};
    std::atomic<int> semanticWorkPhase{0};
    AudioReference reference;
    bool exampleMode = false;
    struct SemanticResult
    {
        uint64_t request = 0;
        std::vector<Asset> candidates;
        LibrarySnapshot::SemanticTiming timing;
        bool example = false;
        double minimumSimilarity = .55;
    } semanticResult;
    std::atomic<bool> stopping{false}, initialized{false}, pauseRequested{false};
    std::atomic<uint64_t> version{1}, workToken{1};
    std::atomic<uint64_t> membershipVersion{1};
    std::atomic<int> activeWorkers{0}, parkedWorkers{0};
    std::atomic<int64_t> headerReads{0};
    std::thread writer, reader;
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<juce::File> additions;
    struct LayoutEdit
    {
        juce::String id;
        int64_t revision;
        ChannelLayout layout;
    };
    std::vector<LayoutEdit> layoutEdits;
    struct GeneratedImport
    {
        Asset verified;
        juce::String collection;
        std::promise<Asset> result;
        juce::var provenance;
    };
    std::deque<GeneratedImport> generatedImports;
    std::deque<CollectionEdit> collectionEdits;
    std::optional<std::pair<BookmarkRequest, juce::String>> bookmarkInspection;
    std::deque<Job> jobs;
    std::deque<Batch> batches;
    bool requestedRescan = false;
    juce::String text, filter, collectionScope, exampleFacets;
    uint64_t request = 1;
    int desiredOffset = 0, desiredLimit = 20;
    bool pagePending = false, pinned = false;
    std::function<void()> changed;
    LibrarySnapshot current;
    DirectoryEvents events; // Outlives root watchers and all owned workers.
    std::vector<std::shared_ptr<Runtime>> roots;
    int completedJobs = 0;
    explicit Impl(juce::File file, juce::File executable, juce::File model, bool controlledSemantic)
        : database(std::move(file)), modelDirectory(model == juce::File() ? defaultModelDirectory() : model)
    {
        current.semantic.enabled = executable != juce::File();
        current.semantic.model = modelId();
        current.semantic.preprocessing = preprocessingId();
        if (current.semantic.enabled && !controlledSemantic)
            inference = std::make_unique<AIProcess>(executable, modelDirectory);
        if (controlledSemantic)
        {
            current.semantic.enabled = true;
            current.semantic.ready = true;
        }
        writer = std::thread([this] { indexLoop(); });
        reader = std::thread([this] { searchLoop(); });
        if (inference)
        {
            analysisWorker = std::thread([this] { analysisLoop(); });
            semanticReader = std::thread([this] { semanticLoop(); });
        }
    }
    ~Impl()
    {
        stopping = true;
        pauseRequested = false;
        installCancelled = true;
        if (inference)
            inference->cancel();
        wake.notify_all();
        if (writer.joinable())
            writer.join();
        if (reader.joinable())
            reader.join();
        if (analysisWorker.joinable())
            analysisWorker.join();
        if (semanticReader.joinable())
            semanticReader.join();
    }
    void notifyLocked()
    {
        if (changed)
            changed();
        wake.notify_all();
    }
    void publish()
    {
        ++version;
        wake.notify_all();
    }
    bool gate()
    {
        if (stopping)
            return false;
        if (pauseRequested)
        {
            ++parkedWorkers;
            wake.notify_all();
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || !pauseRequested; });
            --parkedWorkers;
        }
        return !stopping;
    }
    bool emit(Batch batch)
    {
        if (batch.runtime && batch.runtime->cancelled)
            return false;
        if (!gate())
            return false;
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stopping || batches.size() < 8; });
        if (stopping)
            return false;
        batches.push_back(std::move(batch));
        performance.peakBatches.store(
            std::max<int64_t>(performance.peakBatches.load(), (int64_t)batches.size()));
        wake.notify_all();
        return true;
    }
    bool barrier(const Job &job)
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        Batch b;
        b.runtime = job.runtime;
        b.barrier = promise;
        if (!emit(std::move(b)))
            return false;
        while (!stopping && future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
        {
        }
        return !stopping;
    }
    void readHeader(const Job &job, juce::AudioFormatManager &formats, const juce::String &relative,
                    const FileStamp &source, std::vector<Header> &output)
    {
        if (!gate())
            return;
        job.runtime->stage = 3;
        juce::File file = juce::File(job.root.path).getChildFile(relative);
        FileStamp before, after;
        if (!fileStamp(file, before) || !same(before, source))
        {
            events.push(file.getParentDirectory().getFullPathName());
            return;
        }
        if (juce::Time::currentTimeMillis() - before.modified / 1000000 < 750)
            return;
        ++headerReads;
        ++performance.headers;
        std::unique_ptr<juce::AudioFormatReader> audio(formats.createReaderFor(file));
        juce::StringPairArray tags;
        if (audio)
        {
            tags = audio->metadataValues;
            if (file.hasFileExtension("wav;bwf"))
                addWaveTags(file, tags);
        }
        if (!fileStamp(file, after) || !same(before, after))
        {
            events.push(file.getParentDirectory().getFullPathName());
            return;
        }
        Header h;
        h.relative = relative;
        h.embedded = embeddedFields(tags);
        h.stamp = after;
        if (audio && audio->sampleRate > 0 && audio->lengthInSamples > 0 && audio->numChannels > 0)
        {
            h.seconds = (double)audio->lengthInSamples / audio->sampleRate;
            h.rate = audio->sampleRate;
            h.channels = (int)audio->numChannels;
            h.samples = audio->lengthInSamples;
            h.layout = detectedLayout(*audio);
            h.confidence = detectedLayoutConfidence(*audio);
        }
        else
            h.error = "Unsupported codec, empty or unreadable audio. Rescan to retry.";
        output.push_back(std::move(h));
    }
    void runJob(Job job, juce::AudioFormatManager &formats)
    {
        auto runtime = job.runtime;
        auto root = job.root;
        bool previousOnline = root.online;
        runtime->stage = 1;
        if (root.path == performance.delayedRoot && !performance.delayed.exchange(true))
        {
            auto until = monotonicMs() + performance.delayMs;
            while (monotonicMs() < until && !stopping)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        try
        {
            if (!gate())
                return;
            auto resolved = resolveRoot(root.path, root.volume, root.volumeRelative);
            root.online = resolved.isNotEmpty();
            root.error.clear();
            root.scanning = !job.probe;
            if (root.online)
            {
                root.path = juce::String::fromUTF8(fs::canonical(resolved.toStdString()).c_str());
                if (root.volume.isEmpty())
                {
                    auto location = volumeLocation(juce::File(root.path));
                    root.volume = location.identity;
                    root.volumeRelative = location.relative;
                }
            }
            else
                root.scanning = false;
            if (root.online && (!previousOnline || root.path != job.root.path))
            {
                job.dirs.insert("");
                job.force = true;
                root.scanning = true;
            }
            job.root = root;
            struct statfs volume
            {
            };
            bool local = root.online && statfs(root.path.toRawUTF8(), &volume) == 0 &&
                         (volume.f_flags & MNT_LOCAL) != 0;
            if (root.online && runtime->watchedPath != root.path)
            {
                runtime->watch =
                    std::make_unique<SourceWatch>(std::vector<juce::String>{root.path}, events,
                                                  database.getParentDirectory().getFullPathName());
                runtime->watchedPath = root.path;
            }
            Batch info;
            info.runtime = runtime;
            info.root = root;
            info.rootInfo = true;
            info.local = local;
            info.watchWorking = runtime->watch && runtime->watch->started();
            if (!emit(std::move(info)))
                return;
            if (root.online)
            {
                Catalog read(database, true);
                bool newRoot = true;
                for (const auto &existingRoot : read.roots())
                    if (existingRoot.id == root.id)
                        newRoot = existingRoot.discovered == 0;
                auto scanId = juce::Time::getHighResolutionTicks();
                std::deque<std::pair<juce::String, bool>> directories;
                for (auto &dir : job.dirs)
                    directories.emplace_back(dir, job.force || job.targetsForced);
                bool first = true;
                while (!directories.empty() && gate())
                {
                    runtime->directoriesQueued = (int)directories.size();
                    auto [relative, force] = directories.front();
                    directories.pop_front();
                    runtime->stage = 2;
                    ++performance.directories;
                    auto folder = juce::File(root.path).getChildFile(relative);
                    struct stat st
                    {
                    };
                    if (lstat(folder.getFullPathName().toRawUTF8(), &st) != 0 || !S_ISDIR(st.st_mode))
                    {
                        if (relative.isNotEmpty())
                        {
                            Batch b;
                            b.runtime = runtime;
                            b.root = root;
                            b.removed = relative;
                            if (!emit(std::move(b)))
                                return;
                        }
                        continue;
                    }
                    DirectoryRecord dir;
                    dir.relative = relative;
                    dir.valid = true;
                    dir.modified = st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
                    dir.inode = (int64_t)st.st_ino;
                    auto cached = read.directory(root.id, relative);
                    if (!force && cached.valid && cached.modified == dir.modified &&
                        cached.inode == dir.inode)
                    {
                        for (auto &child : cached.children)
                            directories.emplace_back(childPath(relative, child), false);
                        continue;
                    }
                    ++performance.directoryEnumerations;
                    std::vector<Name> names;
                    std::vector<std::pair<juce::String, FileStamp>> needsHeaders;
                    auto flush = [&]
                    {
                        if (!names.empty())
                        {
                            Batch b;
                            b.runtime = runtime;
                            b.root = root;
                            b.scan = scanId;
                            b.names.swap(names);
                            if (!emit(std::move(b)))
                                return false;
                        }
                        needsHeaders.clear();
                        return true;
                    };
                    for (fs::directory_iterator it(folder.getFullPathName().toStdString()), end;
                         it != end && gate(); ++it)
                    {
                        ++performance.entries;
                        runtime->stage = 2;
                        const auto &entry = *it;
                        auto filename = juce::String::fromUTF8(entry.path().filename().c_str());
                        if (entry.is_symlink())
                            continue;
                        if (entry.is_directory())
                        {
                            if (filename.startsWithChar('.') || filename == "__MACOSX" ||
                                filename.endsWith(".app") ||
                                entry.path() ==
                                    fs::path(database.getParentDirectory().getFullPathName().toStdString()))
                                continue;
                            dir.children.add(filename);
                            directories.emplace_back(childPath(relative, filename), job.force);
                            continue;
                        }
                        juce::File file(juce::String::fromUTF8(entry.path().c_str()));
                        if (!supportedExtension(file))
                            continue;
                        double zero = 0;
                        performance.firstEligibleMs.compare_exchange_strong(zero, monotonicMs());
                        FileStamp stamp;
                        if (!fileStamp(file, stamp))
                            continue;
                        auto path = childPath(relative, filename);
                        auto old = newRoot ? Asset{} : read.existing(root.id, path);
                        juce::String rename;
                        if (old.id.isEmpty() && !newRoot)
                        {
                            std::vector<Asset> candidates;
                            for (auto &a : read.inodeCandidates(root.id, stamp.inode))
                                if (!juce::File(root.path).getChildFile(a.relative).exists())
                                    candidates.push_back(a);
                            if (candidates.size() == 1)
                            {
                                old = candidates.front();
                                rename = old.id;
                            }
                        }
                        if (old.id.isNotEmpty() && !same(stampOf(old), stamp) &&
                            juce::Time::currentTimeMillis() - stamp.modified / 1000000 < 750)
                        {
                            names.push_back({path, rename, stampOf(old)});
                            events.push(file.getParentDirectory().getFullPathName());
                        }
                        else
                        {
                            names.push_back({path, rename, stamp});
                            if (old.id.isEmpty() || old.state == "pending" || !same(stampOf(old), stamp))
                                needsHeaders.emplace_back(path, stamp);
                        }
                        if (first || names.size() >= 64)
                        {
                            first = false;
                            if (!flush())
                                return;
                        }
                    }
                    if (stopping)
                        return;
                    if (!flush())
                        return;
                    for (auto &old : cached.children)
                        if (!dir.children.contains(old))
                        {
                            Batch b;
                            b.runtime = runtime;
                            b.root = root;
                            b.removed = childPath(relative, old);
                            if (!emit(std::move(b)))
                                return;
                        }
                    Batch b;
                    b.runtime = runtime;
                    b.root = root;
                    b.directory = dir;
                    b.scan = scanId;
                    if (!emit(std::move(b)))
                        return;
                }
                // A writer barrier makes pending recovery idempotent with this job's own header batches.
                if (!barrier(job))
                    return;
                while (gate())
                {
                    auto pending = read.pendingRoot(root.id, 64);
                    if (pending.empty())
                        break;
                    std::vector<Header> headers;
                    for (auto &asset : pending)
                    {
                        if (!gate())
                            return;
                        readHeader(job, formats, asset.relative, stampOf(asset), headers);
                    }
                    if (headers.empty())
                        break;
                    Batch b;
                    b.runtime = runtime;
                    b.root = root;
                    b.headers.swap(headers);
                    if (!emit(std::move(b)) || !barrier(job))
                        return;
                }
            }
        }
        catch (const std::exception &error)
        {
            root.error = juce::String::fromUTF8(error.what());
        }
        root.scanning = false;
        runtime->directoriesQueued = 0;
        Batch finished;
        finished.runtime = runtime;
        finished.root = root;
        finished.rootInfo = true;
        finished.finished = true;
        emit(std::move(finished));
        runtime->stage = 0;
    }
    void workerLoop()
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        while (!stopping)
        {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping)
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
                ++activeWorkers;
            }
            ++performance.scans;
            runJob(std::move(job), formats);
            --activeWorkers;
            wake.notify_all();
        }
    }
    bool derived(Batch batch)
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto done = promise->get_future();
        batch.barrier = promise;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || batches.size() < 8; });
            if (stopping)
                return false;
            batches.push_back(std::move(batch));
            performance.peakBatches =
                std::max<int64_t>(performance.peakBatches.load(), (int64_t)batches.size());
        }
        wake.notify_all();
        while (!stopping && done.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
        {
        }
        return !stopping;
    }
    void aiStatus(const juce::String &status, const juce::String &error = {})
    {
        std::lock_guard lock(mutex);
        current.semantic.status = status;
        current.semantic.error = error;
        current.semantic.ready = aiLoaded;
        current.semantic.active = aiAnalyzing;
        notifyLocked();
    }
    void analysisLoop()
    {
        while (!initialized && !stopping)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (stopping)
            return;
        if (!localAiSupported())
        {
            aiStatus("Local AI needs macOS 13.3 or later");
            return;
        }
        bool attempted = false;
        int transportFailures = 0;
        try
        {
            Catalog read(database, true);
            double lastCoverage = 0;
            uint64_t coverageVersion = ~uint64_t{}, coverageAnalysis = ~uint64_t{};
            while (!stopping)
            {
                bool install = false, retry = false;
                juce::File import;
                {
                    std::lock_guard lock(mutex);
                    install = std::exchange(installRequested, false);
                    retry = std::exchange(aiRetryRequested, false);
                    import = importModelDirectory;
                }
                if (install)
                {
                    {
                        std::lock_guard lock(mutex);
                        current.semantic.installing = true;
                    }
                    aiStatus("Installing verified local model");
                    juce::String error;
                    bool installed = installModelPack(
                        modelDirectory, import, installCancelled,
                        [&](int64_t bytes, int64_t total)
                        {
                            std::lock_guard lock(mutex);
                            current.semantic.downloadedBytes = bytes;
                            current.semantic.downloadTotalBytes = total;
                            notifyLocked();
                        },
                        error);
                    {
                        std::lock_guard lock(mutex);
                        current.semantic.installing = false;
                    }
                    aiStatus(installed ? "Model installed; loading" : "Model installation stopped", error);
                    attempted = !installed;
                }
                if (retry)
                {
                    inference->retry();
                    attempted = false;
                    transportFailures = 0;
                    Batch b;
                    b.retryAnalysis = true;
                    if (!derived(std::move(b)))
                        break;
                    aiLoaded = false;
                }
                if (!aiLoaded)
                {
                    if (!attempted && modelDirectory.isDirectory())
                    {
                        attempted = true;
                        aiStatus("Verifying and loading local model");
                        try
                        {
                            auto response = inference->call(message("status"), true);
                            if (response["error"].toString().isNotEmpty())
                                throw std::runtime_error(response["error"].toString().toStdString());
                            aiLoaded = true;
                            Batch b;
                            b.seedAnalysis = true;
                            if (!derived(std::move(b)))
                                break;
                            {
                                std::lock_guard lock(mutex);
                                current.semantic.modelLoadMs = inference->lastLoadMs;
                                current.semantic.helperPeakBytes = (int64_t)response["peakResidentBytes"];
                                ++semanticInputVersion;
                                semanticDue = monotonicMs();
                            }
                            aiStatus("Local AI ready");
                        }
                        catch (const std::exception &e)
                        {
                            aiLoaded = false;
                            aiStatus("Model unavailable; filename search is ready", e.what());
                        }
                    }
                    else if (!attempted)
                        aiStatus("Install AI Model to search by sound description");
                    if (!aiLoaded)
                    {
                        std::unique_lock lock(mutex);
                        wake.wait_for(lock, std::chrono::milliseconds(500));
                        continue;
                    }
                }
                if (monotonicMs() - lastCoverage > 500 &&
                    (coverageVersion != version || coverageAnalysis != analysisGeneration))
                {
                    coverageVersion = version;
                    coverageAnalysis = analysisGeneration;
                    auto coverage = read.analysisCounts(modelId(), preprocessingId());
                    std::lock_guard lock(mutex);
                    current.semantic.coverage = coverage;
                    lastCoverage = monotonicMs();
                    notifyLocked();
                }
                if (pauseRequested)
                {
                    aiAnalyzing = false;
                    aiStatus("Analysis paused; search and audition stay available");
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(100),
                                  [&] { return stopping || !pauseRequested || aiRetryRequested; });
                    continue;
                }
                juce::String preferred;
                {
                    std::lock_guard lock(mutex);
                    preferred = preferredAnalysis;
                }
                auto task = read.nextAnalysis(modelId(), preprocessingId(), preferred);
                if (!task)
                {
                    aiAnalyzing = false;
                    aiStatus("Local AI ready");
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(200));
                    continue;
                }
                auto started = monotonicMs();
                {
                    std::lock_guard lock(mutex);
                    if (pauseRequested)
                        continue;
                    aiAnalyzing = true;
                    current.semantic.asset = task->asset.name;
                    if (!current.semantic.firstWindowStartedMs)
                        current.semantic.firstWindowStartedMs = started;
                }
                aiStatus("Analyzing " + task->asset.name);
                AnalysisUpdate result;
                result.asset = task->asset;
                result.model = modelId();
                result.preprocessing = preprocessingId();
                result.start = task->nextStart;
                auto window = (int64_t)std::llround(10 * task->asset.sampleRate);
                auto stride = (int64_t)std::llround(5 * task->asset.sampleRate);
                result.end = std::min(task->asset.samples, result.start + window);
                result.nextStart = result.start + stride;
                result.done = result.end == task->asset.samples;
                bool stale = false;
                try
                {
                    auto command = message("audio");
                    auto *o = command.getDynamicObject();
                    o->setProperty("path", task->asset.path);
                    o->setProperty("startSample", (juce::int64)result.start);
                    o->setProperty("endSample", (juce::int64)result.end);
                    o->setProperty("sourceRate", task->asset.sampleRate);
                    o->setProperty("sourceSamples", (juce::int64)task->asset.samples);
                    o->setProperty("size", (juce::int64)task->asset.size);
                    o->setProperty("modified", (juce::int64)task->asset.modified);
                    o->setProperty("inode", (juce::int64)task->asset.inode);
                    auto response = inference->call(command);
                    stale = (bool)response["staleSource"];
                    result.error = response["error"].toString();
                    if (auto *values = response["embedding"].getArray())
                        for (auto &value : *values)
                            result.embedding.push_back((float)(double)value);
                    transportFailures = 0;
                    {
                        std::lock_guard lock(mutex);
                        current.semantic.helperPeakBytes = std::max<int64_t>(
                            current.semantic.helperPeakBytes, (int64_t)response["peakResidentBytes"]);
                    }
                }
                catch (const std::exception &e)
                {
                    result.error = e.what();
                    ++transportFailures;
                }
                if (stopping)
                    break;
                if (stale)
                {
                    events.push(juce::File(task->asset.path).getParentDirectory().getFullPathName());
                    aiAnalyzing = false;
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(500));
                    continue;
                }
                if (result.error.isEmpty() && result.embedding.size() != 512)
                    result.error = "Invalid model output; Retry Analysis";
                Batch b;
                b.analysis = result;
                if (!derived(std::move(b)))
                    break;
                {
                    std::lock_guard lock(mutex);
                    current.semantic.analysisWallMs += monotonicMs() - started;
                    current.semantic.lastWindowPublishedMs = monotonicMs();
                    if (result.error.isEmpty())
                    {
                        ++current.semantic.windowsThisSession;
                        auto previousEnd =
                            result.start == 0 ? 0
                                              : std::min(task->asset.samples, result.start - stride + window);
                        current.semantic.sourceSecondsThisSession +=
                            (double)std::max<int64_t>(0, result.end - previousEnd) / task->asset.sampleRate;
                    }
                    current.semantic.error = result.error;
                }
                aiAnalyzing = false;
                if (transportFailures >= 3)
                {
                    aiLoaded = false;
                    aiStatus("AI worker stopped; choose Retry Analysis", result.error);
                }
                else
                    aiStatus("Local AI ready", result.error);
                // Leave headroom during audition/indexing; at most one inference window is in flight.
                bool yieldingToAudition = auditionActive;
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(yieldingToAudition ? 150 : 20),
                              [&]
                              {
                                  return stopping || pauseRequested || aiRetryRequested || installRequested ||
                                         (yieldingToAudition && !auditionActive);
                              });
            }
        }
        catch (const std::exception &e)
        {
            if (!stopping)
                aiStatus("Analysis stopped; choose Retry Analysis", e.what());
        }
        aiAnalyzing = false;
    }
    void semanticLoop()
    {
        while (!initialized && !stopping)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (stopping)
            return;
        try
        {
            Catalog read(database, true);
            ReferenceEncoder referenceEncoder(database.getParentDirectory().getChildFile("references"));
            RetrievalIndex index(
                database,
                [&](const IndexStats &s)
                {
                    indexWorking = s.building;
                    std::lock_guard lock(mutex);
                    auto &ai = current.semantic;
                    bool published = s.ready && (!ai.indexReady || ai.indexGeneration != s.sequence);
                    ai.indexReady = s.ready;
                    ai.indexReused = s.reused;
                    ai.indexBuilding = s.building;
                    ai.indexStatus = s.status;
                    ai.indexRegions = s.regions;
                    ai.indexBytes = s.bytes;
                    ai.indexOpenMs = s.openMs;
                    ai.indexBuildMs = s.buildMs;
                    ai.indexGeneration = s.sequence;
                    if (published)
                        ++semanticInputVersion;
                    notifyLocked();
                },
                [&] { return pauseRequested.load(); });
            uint64_t lastInput = 0, lastAnalysis = 0;
            double lastCompleted = 0;
            std::deque<std::pair<juce::String, std::vector<float>>> cache;
            while (!stopping)
            {
                juce::String q, root, scope;
                juce::String facets;
                AudioReference example;
                bool usingExample = false;
                uint64_t id, input, token, corpus;
                {
                    std::unique_lock lock(mutex);
                    input = semanticInputVersion;
                    corpus = analysisGeneration;
                    if (!aiLoaded ||
                        (!exampleMode &&
                         (text.trim().isEmpty() ||
                          (isMetadataQuery(text) && metadataTokens(text, false).isEmpty()))) ||
                        (input == lastInput &&
                         (corpus == lastAnalysis || monotonicMs() - lastCompleted < 500)) ||
                        monotonicMs() < semanticDue)
                    {
                        wake.wait_for(lock, std::chrono::milliseconds(20));
                        continue;
                    }
                    q = text;
                    root = filter;
                    scope = collectionScope;
                    id = request;
                    token = workToken;
                    usingExample = exampleMode;
                    example = reference;
                    facets = usingExample ? exampleFacets : metadataTokens(q, true);
                    if (!usingExample && facets.isNotEmpty())
                        q = metadataTokens(q, false);
                }
                semanticWorkRequest = id;
                semanticWorkPhase = 1;
                SemanticResult result;
                result.request = id;
                result.example = usingExample;
                result.minimumSimilarity = example.minimumSimilarity;
                if (!usingExample && q.length() == 32 && q.containsOnly("0123456789abcdefABCDEF"))
                {
                    std::lock_guard lock(mutex);
                    if (request == id && workToken == token)
                    {
                        semanticResult = result;
                        ++semanticResultVersion;
                        wake.notify_all();
                    }
                    lastInput = input;
                    lastAnalysis = corpus;
                    lastCompleted = monotonicMs();
                    continue;
                }
                try
                {
                    std::vector<float> vector;
                    juce::String excluded;
                    std::optional<ReferenceEmbedding> encoded;
                    if (usingExample)
                    {
                        if (!example.external)
                        {
                            auto actual = read.asset(example.source.id);
                            if (actual.id.isEmpty() || actual.revision != example.source.revision)
                                throw std::runtime_error(
                                    "Reference revision changed. Refresh and select it again.");
                            example.source = std::move(
                                actual); // A confirmed relink changes location, not source identity.
                        }
                        else
                        {
                            std::error_code pathError;
                            auto canonical =
                                std::filesystem::canonical(example.source.path.toStdString(), pathError);
                            if (!pathError)
                                example.source.path = juce::String::fromUTF8(canonical.c_str());
                            for (const auto &r : read.roots())
                                if (example.source.path.startsWith(r.path + "/"))
                                {
                                    auto known = read.existing(
                                        r.id, example.source.path.substring(r.path.length() + 1));
                                    if (known.id.isNotEmpty())
                                    {
                                        example.source.id = known.id;
                                        example.source.revision = known.revision;
                                        break;
                                    }
                                }
                        }
                        encoded = referenceEncoder.encode(
                            example, *inference, [&] { return stopping || workToken != token; },
                            [&](const AudioReference &ref, int done, int total)
                            {
                                std::lock_guard lock(mutex);
                                if (request != id || workToken != token)
                                    return;
                                auto &state = current.reference;
                                state.reference = ref;
                                state.loading = true;
                                state.completedWindows = done;
                                state.totalWindows = total;
                                state.status = "Reference: " + juce::String(done) + " / " +
                                               juce::String(total) + " windows";
                                notifyLocked();
                            });
                        vector = encoded->vector;
                        if (!example.includeSource)
                            excluded = encoded->reference.source.id;
                    }
                    else
                    {
                        auto found = std::find_if(cache.begin(), cache.end(),
                                                  [&](const auto &v) { return v.first == q; });
                        if (found != cache.end())
                        {
                            vector = found->second;
                            result.timing.textCacheHit = true;
                            auto entry = std::move(*found);
                            cache.erase(found);
                            cache.push_back(std::move(entry));
                        }
                        else
                        {
                            auto begin = monotonicMs();
                            auto command = message("text");
                            command.getDynamicObject()->setProperty("text", q);
                            semanticWorkPhase = 2;
                            auto response = inference->call(command, true);
                            result.timing.workerRoundTripMs = monotonicMs() - begin;
                            if (response["error"].toString().isNotEmpty())
                                throw std::runtime_error(response["error"].toString().toStdString());
                            if (response["model"].toString() != modelId() ||
                                response["preprocessing"].toString() != preprocessingId())
                                throw std::runtime_error("Query/index embedding-space mismatch");
                            if (auto *values = response["embedding"].getArray())
                                for (auto &v : *values)
                                    vector.push_back((float)(double)v);
                            if (vector.size() != 512)
                                throw std::runtime_error("Invalid text embedding");
                            result.timing.textEncodeMs = (double)response["inferenceMs"];
                            result.timing.textPreprocessMs = (double)response["preprocessMs"];
                            {
                                std::lock_guard lock(mutex);
                                current.semantic.helperPeakBytes = std::max<int64_t>(
                                    current.semantic.helperPeakBytes, (int64_t)response["peakResidentBytes"]);
                            }
                            cache.emplace_back(q, vector);
                            if (cache.size() > 64)
                                cache.pop_front();
                        }
                    }
                    if (workToken != token)
                    {
                        lastInput = input;
                        semanticHandledInput = input;
                        semanticWorkPhase = 5;
                        continue;
                    }
                    read.cancelWhen(&workToken, token);
                    RetrievalMetrics metrics;
                    auto retrievalBegan = monotonicMs();
                    semanticWorkPhase = 3;
                    read.setScope(scope);
                    read.setSemanticFilter(facets);
                    result.candidates = index.search(read, vector, root, excluded, 100, &metrics,
                                                     [&] { return stopping || workToken != token; });
                    if (encoded)
                    {
                        FileStamp stamp;
                        const auto &a = encoded->reference.source;
                        if (!fileStamp(juce::File(a.path), stamp) || stamp.size != a.size ||
                            stamp.modified != a.modified || stamp.inode != a.inode)
                            throw std::runtime_error("Reference changed during search. Choose it again.");
                    }
                    result.timing.fetchFilterMs = metrics.fetchFilterMs;
                    result.timing.vectorScoreMs = metrics.vectorScoreMs;
                    result.timing.rankGroupMs = metrics.rankGroupMs;
                    result.timing.vectors = metrics.vectors;
                    std::lock_guard lock(mutex);
                    if (request == id && workToken == token)
                    {
                        if (encoded)
                        {
                            auto &state = current.reference;
                            state.reference = encoded->reference;
                            state.loading = false;
                            state.cacheHit = encoded->cacheHit;
                            state.encodingMs = encoded->encodingMs;
                            state.retrievalMs = monotonicMs() - retrievalBegan;
                            state.status = encoded->cacheHit ? "Cached reference ready" : "Reference ready";
                            state.error.clear();
                            state.silentWindows = encoded->silentWindows;
                            if (encoded->silentWindows)
                                state.status +=
                                    " (" + juce::String(encoded->silentWindows) + " silent windows skipped)";
                        }
                        semanticResult = std::move(result);
                        ++semanticResultVersion;
                        semanticWorkPhase = 4;
                        wake.notify_all();
                    }
                }
                catch (const QueryCancelled &)
                {
                    semanticWorkPhase = 5;
                }
                catch (const std::exception &e)
                {
                    if (!stopping)
                    {
                        std::lock_guard lock(mutex);
                        if (request == id)
                        {
                            if (usingExample)
                            {
                                current.reference.error = e.what();
                                current.reference.status = "Reference unavailable";
                                current.reference.loading = false;
                                semanticResult = result;
                                ++semanticResultVersion;
                            }
                            else
                                current.semantic.error = e.what();
                            notifyLocked();
                        }
                    }
                }
                lastInput = input;
                semanticHandledInput = input;
                lastAnalysis = corpus;
                lastCompleted = monotonicMs();
            }
        }
        catch (const std::exception &e)
        {
            if (!stopping)
                aiStatus("Semantic reader stopped", e.what());
        }
    }
    void updateState(Catalog &db)
    {
        auto rows = db.roots();
        IndexProgress progress;
        progress.pauseRequested = pauseRequested;
        progress.activeWorkers = activeWorkers;
        progress.parkedWorkers = parkedWorkers;
        progress.completedJobs = completedJobs;
        {
            std::lock_guard lock(mutex);
            progress.queuedBatches = (int)batches.size();
            progress.paused = pauseRequested && batches.empty() && jobs.empty() &&
                              activeWorkers == parkedWorkers && !aiAnalyzing && !indexWorking;
        }
        for (auto &r : rows)
            progress.metadataBacklog += r.online ? r.discovered - r.ready - r.failed : 0;
        for (auto &runtime : roots)
        {
            progress.queuedDirectories +=
                (int)runtime->pending.size() + (runtime->full ? 1 : 0) + runtime->directoriesQueued.load();
            if (runtime->active)
            {
                progress.root = runtime->root.path;
                switch (runtime->stage.load())
                {
                case 1:
                    progress.stage = "Checking source";
                    break;
                case 2:
                    progress.stage = "Discovering filenames";
                    break;
                case 3:
                    progress.stage = "Reading headers";
                    break;
                default:
                    progress.stage = "Committing";
                }
                if (monotonicMs() - runtime->started > 2000 && runtime->stage == 1)
                    progress.stage = "Waiting on source I/O";
            }
        }
        if (progress.paused)
            progress.stage = "Paused";
        else if (progress.pauseRequested)
            progress.stage = "Pausing at next I/O boundary";
        else if (!progress.activeWorkers)
            progress.stage = aiAnalyzing ? "Analyzing sounds" : "Up to date";
        juce::String selected;
        {
            std::lock_guard lock(mutex);
            selected = current.selectedAssetId;
        }
        bool removed = selected.isNotEmpty() && db.asset(selected).id.isEmpty();
        std::lock_guard lock(mutex);
        current.roots = std::move(rows);
        if (current.selectedAssetId == selected)
            current.selectedAssetRemoved = removed;
        current.progress = progress;
        current.headersRead = headerReads.load();
        notifyLocked();
    }
    void indexLoop()
    {
        try
        {
            if (!database.getParentDirectory().createDirectory())
                throw std::runtime_error("Cannot create catalog directory");
            Catalog db(database);
            for (auto &root : db.roots())
            {
                auto state = std::make_shared<Runtime>();
                state->root = root;
                roots.push_back(state);
            }
            for (int i = 0; i < 4; ++i)
                workers.emplace_back([this] { workerLoop(); });
            {
                std::lock_guard lock(mutex);
                current.organization = db.collections();
                current.organization.version = 1;
            }
            initialized = true;
            publish();
            auto lastStatus = monotonicMs();
            auto lastWake = lastStatus;
            auto lastWall = juce::Time::currentTimeMillis();
            while (!stopping)
            {
                std::vector<juce::File> added;
                std::vector<LayoutEdit> edits;
                std::deque<Batch> ready;
                std::deque<CollectionEdit> metadata;
                std::deque<GeneratedImport> generated;
                std::optional<std::pair<BookmarkRequest, juce::String>> inspection;
                bool rescan = false;
                {
                    std::lock_guard lock(mutex);
                    added.swap(additions);
                    generated.swap(generatedImports);
                    edits.swap(layoutEdits);
                    metadata.swap(collectionEdits);
                    inspection = std::move(bookmarkInspection);
                    bookmarkInspection.reset();
                    ready.swap(batches);
                    rescan = std::exchange(requestedRescan, false);
                    wake.notify_all();
                }
                if (inspection)
                {
                    auto value = db.bookmarkFavorite(inspection->first);
                    std::lock_guard lock(mutex);
                    current.bookmarkInspection = inspection->second;
                    current.bookmarkFavorite = value;
                    notifyLocked();
                }
                for (auto &edit : metadata)
                {
                    juce::String error;
                    try
                    {
                        if (edit.action == "meta-import")
                        {
                            juce::File f(edit.name);
                            if (!f.existsAsFile() || f.getSize() > 16 * 1024 * 1024)
                                throw std::runtime_error("Invalid metadata manifest");
                            db.importMetadata(juce::JSON::parse(f));
                        }
                        else if (edit.action == "meta-export")
                        {
                            juce::File f(edit.name);
                            if (f.exists())
                                throw std::runtime_error("Choose a new metadata manifest name");
                            auto payload = db.metadataManifest(edit.items);
                            juce::TemporaryFile tmp(f);
                            if (!tmp.getFile().replaceWithText(juce::JSON::toString(payload)) ||
                                ::renamex_np(tmp.getFile().getFullPathName().toRawUTF8(),
                                             f.getFullPathName().toRawUTF8(), RENAME_EXCL) != 0)
                                throw std::runtime_error("Cannot publish metadata manifest");
                        }
                        else if (edit.action.startsWith("meta-"))
                            db.editMetadata(edit);
                        else
                            db.editCollections(edit);
                    }
                    catch (const std::exception &e)
                    {
                        error = e.what();
                    }
                    auto organization = db.collections();
                    std::lock_guard lock(mutex);
                    organization.version = current.organization.version + 1;
                    organization.error = error;
                    current.organization = std::move(organization);
                    ++version;
                    if (edit.action == "used" && collectionScope == "recent" && pinned)
                        current.newResultsAvailable = true;
                    if (edit.action.startsWith("meta-"))
                    {
                        ++current.metadataVersion;
                        ++membershipVersion;
                        ++workToken;
                    }
                    if (collectionScope.isNotEmpty() &&
                        (edit.action != "used" || (collectionScope == "recent" && !pinned)))
                    {
                        pinned = false;
                        ++membershipVersion;
                        ++workToken;
                        ++semanticInputVersion;
                        semanticDue = monotonicMs();
                    }
                    notifyLocked();
                }
                for (auto &edit : edits)
                {
                    db.setLayoutOverride(edit.id, edit.revision, edit.layout);
                    publish();
                }
                for (auto &file : added)
                {
                    auto root = db.addRootRecord(file);
                    bool found = false;
                    for (auto &r : roots)
                        found |= r->root.id == root.id;
                    if (!found)
                    {
                        auto state = std::make_shared<Runtime>();
                        state->root = root;
                        roots.push_back(state);
                    }
                    publish();
                }
                for (auto &job : generated)
                {
                    try
                    {
                        juce::File file(job.verified.path);
                        FileStamp stamp;
                        if (!fileStamp(file, stamp) || !same(stamp, stampOf(job.verified)))
                            throw std::runtime_error("Generated WAV changed before indexing");
                        std::shared_ptr<Runtime> owner;
                        for (auto &r : roots)
                            if (file.isAChildOf(juce::File(r->root.path)) &&
                                (!owner || r->root.path.length() > owner->root.path.length()))
                                owner = r;
                        if (!owner)
                        {
                            owner = std::make_shared<Runtime>();
                            owner->root = db.addRoot(file.getParentDirectory());
                            roots.push_back(owner);
                        }
                        auto relative = file.getRelativePathFrom(juce::File(owner->root.path));
                        auto a = db.existing(owner->root.id, relative);
                        if (a.id.isEmpty() || !same(stamp, stampOf(a)) || a.state != "ready")
                        {
                            db.discover(owner->root, relative, stamp, juce::Time::currentTimeMillis(), false);
                            a = db.existing(owner->root.id, relative);
                            a.detectedChannels = job.verified.detectedChannels;
                            a.layoutConfidence = job.verified.layoutConfidence;
                            db.metadata(a, job.verified.duration, job.verified.channels, job.verified.sampleRate,
                                        {}, job.verified.samples);
                            a = db.asset(a.id);
                        }
                        db.providerMetadata(a, job.provenance);
                        CollectionEdit e;
                        e.action = "member";
                        e.id = job.collection;
                        if (e.id.isEmpty())
                        {
                            for (const auto &c : db.collections().nodes)
                                if (c.parent.isEmpty() && c.name.equalsIgnoreCase("AI Generated"))
                                    e.id = c.id;
                            if (e.id.isEmpty())
                            {
                                e.action = "create";
                                e.name = "AI Generated";
                            }
                        }
                        e.items.push_back({a});
                        db.editCollections(e);
                        auto organization = db.collections();
                        {
                            std::lock_guard lock(mutex);
                            organization.version = current.organization.version + 1;
                            current.organization = std::move(organization);
                            ++membershipVersion; ++workToken; ++semanticInputVersion;
                            notifyLocked();
                        }
                        publish();
                        job.result.set_value(a);
                    }
                    catch (...) { job.result.set_exception(std::current_exception()); }
                }
                if (rescan)
                {
                    db.exec("UPDATE assets SET state='pending',error='' WHERE state='failed'");
                    db.retryAnalysis(modelId(), preprocessingId());
                    for (auto &r : roots)
                    {
                        r->full = true;
                        r->force = true;
                    }
                    publish();
                }
                for (auto &b : ready)
                {
                    if (b.analysis || b.seedAnalysis || b.retryAnalysis)
                    {
                        db.begin();
                        try
                        {
                            if (b.analysis && db.commitAnalysis(*b.analysis))
                                ++analysisGeneration;
                            if (b.seedAnalysis)
                                db.seedAnalysis(modelId(), preprocessingId());
                            if (b.retryAnalysis)
                                db.retryAnalysis(modelId(), preprocessingId());
                            db.commit();
                        }
                        catch (const std::exception &e)
                        {
                            db.rollback();
                            std::lock_guard lock(mutex);
                            current.semantic.error = e.what();
                            notifyLocked();
                        }
                        if (b.barrier)
                            b.barrier->set_value();
                        wake.notify_all();
                        continue;
                    }
                    if (b.barrier)
                    {
                        b.barrier->set_value();
                        continue;
                    }
                    if (b.runtime && b.runtime->cancelled)
                        continue;
                    if (b.rootInfo)
                    {
                        bool duplicate = false;
                        for (auto &other : roots)
                            if (other != b.runtime && other->root.path == b.root.path)
                                duplicate = true;
                        if (duplicate)
                        {
                            b.runtime->cancelled = true;
                            db.removeEmptyRoot(b.runtime->root.id);
                            std::erase(roots, b.runtime);
                            publish();
                            continue;
                        }
                    }
                    bool writes = !b.names.empty() || !b.headers.empty() || b.directory.has_value() ||
                                  b.removed.isNotEmpty();
                    if (writes)
                        db.begin();
                    for (auto &n : b.names)
                        db.discover(b.root, n.relative, n.stamp, b.scan, false, n.rename);
                    for (auto &h : b.headers)
                    {
                        auto a = db.existing(b.root.id, h.relative);
                        if (a.id.isNotEmpty() && same(stampOf(a), h.stamp))
                        {
                            a.detectedChannels = h.layout;
                            a.layoutConfidence = h.confidence;
                            db.metadata(a, h.seconds, h.channels, h.rate, h.error, h.samples);
                            db.snapshotMetadata(a, h.embedded);
                            if (aiLoaded)
                                db.seedAnalysis(modelId(), preprocessingId(), a.id);
                        }
                    }
                    if (b.directory)
                        db.directoryDone(b.root, *b.directory, b.scan);
                    if (b.removed.isNotEmpty())
                        db.removeTree(b.root.id, b.removed);
                    if (writes)
                    {
                        db.commit();
                        performance.discovered.fetch_add((int64_t)b.names.size());
                        double zero = 0;
                        if (!b.names.empty())
                            performance.firstCommitMs.compare_exchange_strong(zero, monotonicMs());
                        if (!b.names.empty() && (!performance.expectedFiles ||
                                                 performance.discovered <= performance.expectedFiles))
                            performance.discoveryFinishedMs = monotonicMs();
                        if (!b.names.empty() || !b.headers.empty())
                            ++membershipVersion;
                        publish();
                    }
                    if (b.rootInfo)
                    {
                        auto &old = b.runtime->root;
                        if (old.path != b.root.path || old.online != b.root.online ||
                            old.scanning != b.root.scanning || old.error != b.root.error ||
                            old.volume != b.root.volume)
                        {
                            db.rootState(b.root);
                            publish();
                        }
                        b.runtime->root = b.root;
                        if (!b.finished)
                        {
                            b.runtime->local = b.local;
                            b.runtime->watchWorking = b.watchWorking;
                        }
                    }
                    if (b.finished)
                    {
                        b.runtime->active = false;
                        b.runtime->lastCheck = monotonicMs();
                        ++completedJobs;
                    }
                }
                auto [paths, recovery] = events.take();
                for (auto &runtime : roots)
                {
                    if (recovery)
                    {
                        runtime->full = true;
                        runtime->force = true;
                    }
                    for (auto path : paths)
                    {
                        if (path.startsWith("/tmp/"))
                            path = "/private" + path;
                        auto root = runtime->root.path;
                        if (path == root)
                        {
                            runtime->pending.insert("");
                            runtime->pendingDue = monotonicMs() + 750;
                        }
                        else if (path.startsWith(root + "/"))
                        {
                            runtime->pending.insert(path.substring(root.length() + 1));
                            runtime->pendingDue = monotonicMs() + 750;
                        }
                        if (runtime->pending.size() > 2048)
                        {
                            runtime->pending.clear();
                            runtime->full = true;
                            runtime->force = true;
                        }
                    }
                }
                auto now = monotonicMs();
                auto wall = juce::Time::currentTimeMillis();
                if (now - lastWake > 5000 || wall - lastWall > 5000 || wall - lastWall < -5000)
                {
                    for (auto &r : roots)
                    {
                        r->full = true;
                        r->force = true;
                    }
                }
                lastWake = now;
                lastWall = wall;
                if (!pauseRequested)
                {
                    auto schedulingRoots = db.roots();
                    for (auto &r : roots)
                    {
                        int slots = 0;
                        for (auto &other : roots)
                            slots += other->active ? 1 : 0;
                        if (slots >= 4)
                            break;
                        if (r->active)
                            continue;
                        if (!r->full && !r->pending.empty() && now < r->pendingDue)
                            continue;
                        bool directories = now - r->lastDirectories >= 30000;
                        bool recoveryDue = now - r->lastRecovery >= 300000;
                        if (directories)
                        {
                            r->full = true;
                            r->lastDirectories = now;
                        }
                        if (recoveryDue || (directories && (!r->local || !r->watchWorking)))
                        {
                            r->force = true;
                            r->full = true;
                            r->lastRecovery = now;
                        }
                        bool pendingHeaders = false;
                        for (auto &row : schedulingRoots)
                            if (row.id == r->root.id)
                                pendingHeaders = row.online && row.discovered > row.ready + row.failed;
                        if (!r->full && r->pending.empty() &&
                            now - r->lastCheck < (pendingHeaders ? 1000 : 2000))
                            continue;
                        Job job;
                        job.runtime = r;
                        job.root = r->root;
                        job.force = r->force;
                        job.targetsForced = !r->pending.empty();
                        job.dirs.swap(r->pending);
                        if (r->full)
                            job.dirs.insert("");
                        job.probe = job.dirs.empty();
                        r->full = false;
                        r->force = false;
                        r->active = true;
                        r->started = now;
                        {
                            std::lock_guard lock(mutex);
                            jobs.push_back(std::move(job));
                        }
                        wake.notify_all();
                    }
                }
                if (now - lastStatus >= 50 || !ready.empty() || !added.empty())
                {
                    updateState(db);
                    lastStatus = now;
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(10),
                              [&]
                              {
                                  return stopping || !batches.empty() || !additions.empty() ||
                                         !layoutEdits.empty() || !collectionEdits.empty() || !generatedImports.empty() ||
                                         bookmarkInspection.has_value() || requestedRescan;
                              });
            }
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "Library writer failure: %s\n", error.what());
            std::lock_guard lock(mutex);
            current.error = juce::String::fromUTF8(error.what());
            initialized = true;
            notifyLocked();
            stopping = true;
        }
        wake.notify_all();
        for (auto &worker : workers)
            if (worker.joinable())
                worker.join();
    }
    void searchLoop()
    {
        while (!initialized && !stopping)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (stopping)
            return;
        try
        {
            Catalog db(database, true);
            uint64_t lastRequest = 0, lastVersion = 0, session = 0, lastMembership = 0, lastSemantic = 0;
            double lastBackground = 0;
            bool transaction = false;
            std::map<juce::String, Asset> annotations;
            auto annotate = [&](std::vector<Asset> &rows, const juce::String &q)
            {
                for (auto &a : rows)
                {
                    a.matchReason = q.isEmpty()                  ? juce::String()
                                    : a.id.equalsIgnoreCase(q)   ? "Asset ID"
                                    : a.name.equalsIgnoreCase(q) ? "Exact filename"
                                                                 : "Filename";
                    auto found = annotations.find(a.resultKey());
                    if (found != annotations.end() && found->second.revision == a.revision)
                    {
                        a.matches = found->second.matches;
                        a.matchReason = found->second.matchReason;
                    }
                }
            };
            auto fuse = [&](const SemanticResult &semantic, const juce::String &q, const juce::String &root)
            {
                struct Candidate
                {
                    Asset asset;
                    double rank = 0;
                    bool lexical = false;
                };
                std::map<juce::String, Candidate> candidates;
                auto lexical = semantic.example ? std::vector<Asset>{} : db.search(q, root, 128);
                for (size_t i = 0; i < lexical.size(); ++i)
                    candidates[lexical[i].resultKey()] = {lexical[i], 1.0 / (60 + i + 1), true};
                annotations.clear();
                auto expanded = db.expandScope(semantic.candidates);
                for (size_t i = 0; i < expanded.size(); ++i)
                {
                    const auto &hit = expanded[i];
                    auto now = hit.libraryReference.isEmpty() ? db.asset(hit.id)
                                                              : db.referenceAsset(hit.libraryReference);
                    if (now.id.isEmpty() || now.revision != hit.revision ||
                        (root.isNotEmpty() && now.rootId != root))
                        continue;
                    auto &candidate = candidates[hit.resultKey()];
                    now.matches = hit.matches;
                    if (semantic.example)
                    {
                        std::erase_if(now.matches,
                                      [&](auto &match) {
                                          return !std::isfinite(match.similarity) ||
                                                 match.similarity < (float)semantic.minimumSimilarity;
                                      });
                        if (now.matches.empty())
                        {
                            candidates.erase(hit.resultKey());
                            continue;
                        }
                        std::sort(now.matches.begin(), now.matches.end(),
                                  [](auto &a, auto &b) {
                                      return a.similarity != b.similarity ? a.similarity > b.similarity
                                                                          : a.startSample < b.startSample;
                                  });
                        now.matchReason = "Similarity " + juce::String(now.matches.front().similarity, 3);
                        candidate.rank = now.matches.front().similarity;
                    }
                    else
                    {
                        now.matchReason =
                            candidate.lexical ? "Filename + sound similarity" : "Sound similarity";
                        candidate.rank += 1.0 / (60 + i + 1);
                    }
                    candidate.asset = now;
                    annotations[now.resultKey()] = now;
                }
                std::vector<Candidate> ranked;
                for (auto &[id, candidate] : candidates)
                {
                    if (!semantic.example &&
                        (candidate.asset.id.equalsIgnoreCase(q) || candidate.asset.name.equalsIgnoreCase(q)))
                        candidate.rank += 1000;
                    ranked.push_back(std::move(candidate));
                }
                std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b)
                          { return a.rank != b.rank ? a.rank > b.rank : a.asset.id < b.asset.id; });
                std::vector<Asset> prefix;
                for (auto &candidate : ranked)
                    prefix.push_back(std::move(candidate.asset));
                return db.promote(prefix, !semantic.example);
            };
            while (!stopping)
            {
                juce::String q, root, scope;
                uint64_t id, token, v = version.load(), membership = membershipVersion.load(),
                                    semanticVersion;
                int offset = 0, limit = 20;
                bool page = false, isPinned = false;
                bool usingExample = false;
                SemanticResult semantic;
                {
                    std::unique_lock lock(mutex);
                    semanticVersion = semanticResultVersion;
                    if (request == lastRequest && v == lastVersion && !pagePending &&
                        semanticVersion == lastSemantic)
                    {
                        wake.wait_for(lock, std::chrono::milliseconds(50));
                        continue;
                    }
                    if (request == lastRequest && !pagePending && !pinned && membership != lastMembership &&
                        monotonicMs() - lastBackground < 100 && semanticVersion == lastSemantic)
                    {
                        wake.wait_for(lock, std::chrono::milliseconds(5));
                        continue;
                    }
                    id = request;
                    token = workToken;
                    q = text;
                    root = filter;
                    scope = collectionScope;
                    usingExample = exampleMode;
                    page = pagePending;
                    offset = desiredOffset;
                    limit = desiredLimit;
                    isPinned = pinned;
                    pagePending = false;
                    if (semanticResult.request == id)
                        semantic = semanticResult;
                    else
                    {
                        lastSemantic = semanticVersion;
                        readerSemanticSeen = lastSemantic;
                    }
                }
                try
                {
                    db.cancelWhen(&workToken, token);
                    db.setScope(scope);
                    // A pinned result sequence never silently moves under a user's selected row.
                    if (id == session && semantic.request == id && semanticVersion != lastSemantic)
                    {
                        if (isPinned)
                        {
                            std::lock_guard lock(mutex);
                            if (id == request)
                            {
                                current.newResultsAvailable = true;
                                ++current.pageSerial;
                                notifyLocked();
                            }
                        }
                        else
                        {
                            auto previousAnnotations = annotations;
                            auto began = monotonicMs();
                            db.exec("BEGIN");
                            transaction = true;
                            int total = fuse(semantic, q, root);
                            auto rows = db.page(0, 20);
                            annotate(rows, q);
                            std::lock_guard lock(mutex);
                            // Serialize sequence adoption with explicit selection pinning.
                            // A click arriving during retrieval must preserve the old browse table.
                            if (id != request || token != workToken || pinned)
                            {
                                db.cancelWhen(nullptr, 0);
                                db.rollback();
                                transaction = false;
                                annotations = std::move(previousAnnotations);
                                if (id == request && pinned)
                                {
                                    current.newResultsAvailable = true;
                                    ++current.pageSerial;
                                    notifyLocked();
                                }
                                lastSemantic = semanticVersion;
                                readerSemanticSeen = lastSemantic;
                                continue;
                            }
                            db.exec("COMMIT");
                            transaction = false;
                            if (id == request && token == workToken)
                            {
                                current.results = std::move(rows);
                                current.offset = 0;
                                current.total = total;
                                current.semanticTiming = semantic.timing;
                                current.semanticTiming.fusionMs = monotonicMs() - began;
                                current.semanticCompletedRequest = id;
                                ++current.semanticSerial;
                                current.version = v;
                                ++current.pageSerial;
                                notifyLocked();
                            }
                        }
                        lastSemantic = semanticVersion;
                        readerSemanticSeen = lastSemantic;
                        lastVersion = v;
                        continue;
                    }
                    if (page && id == session)
                    {
                        auto rows = db.page(offset, limit);
                        annotate(rows, q);
                        std::lock_guard lock(mutex);
                        if (id == request && token == workToken)
                        {
                            current.results = std::move(rows);
                            current.offset = offset;
                            ++current.pageSerial;
                            notifyLocked();
                        }
                        lastVersion = v;
                        continue;
                    }
                    if (id == lastRequest && v != lastVersion && (isPinned || membership == lastMembership))
                    {
                        int at = 0, size = 20;
                        {
                            std::lock_guard lock(mutex);
                            at = current.offset;
                            size = (int)current.results.size();
                        }
                        db.exec("BEGIN");
                        transaction = true;
                        int remaining = db.reconcileBrowse();
                        auto rows = db.page(at, size);
                        annotate(rows, q);
                        db.exec("COMMIT");
                        transaction = false;
                        std::lock_guard lock(mutex);
                        if (id != request)
                            continue;
                        current.results = std::move(rows);
                        ++current.pageSerial;
                        current.total = remaining;
                        current.newResultsAvailable = isPinned && membership != lastMembership;
                        current.version = v;
                        lastVersion = v;
                        notifyLocked();
                        continue;
                    }
                    bool newQuery = id != lastRequest;
                    auto previousAnnotations = annotations;
                    if (newQuery)
                        annotations.clear();
                    db.exec("BEGIN");
                    transaction = true;
                    auto rows = usingExample ? std::vector<Asset>{} : db.search(q, root, 20);
                    int count = usingExample ? 0 : db.countMatches(q, root);
                    // Publish the fast lexical path before building the disk-backed browse sequence.
                    if (newQuery)
                    {
                        annotate(rows, q);
                        std::lock_guard lock(mutex);
                        if (id == request && token == workToken)
                        {
                            current.results = rows;
                            current.total = count;
                            current.offset = 0;
                            current.newResultsAvailable = false;
                            current.request = id;
                            current.version = v;
                            current.semanticCompletedRequest = 0;
                            current.semanticTiming = {};
                            ++current.pageSerial;
                            notifyLocked();
                        }
                    }
                    db.materialize(q, root, usingExample);
                    if (!newQuery && semantic.request == id)
                    {
                        count = fuse(semantic, q, root);
                        rows = db.page(0, 20);
                    }
                    std::unique_lock publication(mutex);
                    if (id != request || token != workToken || (!newQuery && pinned))
                    {
                        db.cancelWhen(nullptr, 0);
                        db.rollback();
                        transaction = false;
                        annotations = std::move(previousAnnotations);
                        if (id == request && pinned)
                        {
                            current.newResultsAvailable = true;
                            ++current.pageSerial;
                            notifyLocked();
                        }
                        continue;
                    }
                    db.exec("COMMIT");
                    transaction = false;
                    publication.unlock();
                    if (!newQuery)
                    {
                        annotate(rows, q);
                        std::lock_guard lock(mutex);
                        if (id == request && token == workToken)
                        {
                            current.results = std::move(rows);
                            current.total = count;
                            current.offset = 0;
                            current.newResultsAvailable = false;
                            current.request = id;
                            current.version = v;
                            if (semantic.request == id)
                                current.semanticCompletedRequest = id;
                            ++current.pageSerial;
                            notifyLocked();
                        }
                    }
                    session = id;
                    readerSession = id;
                    lastRequest = id;
                    lastVersion = v;
                    lastMembership = membership;
                    lastBackground = monotonicMs();
                }
                catch (const QueryCancelled &)
                {
                    if (transaction)
                    {
                        db.cancelWhen(nullptr, 0);
                        db.rollback();
                        transaction = false;
                    }
                    ++performance.cancelled;
                }
            }
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "Library reader failure: %s\n", e.what());
            std::lock_guard lock(mutex);
            current.error = e.what();
            notifyLocked();
        }
    }
};
LibraryService::LibraryService(juce::File file, juce::File executable, juce::File model,
                               bool controlledSemantic)
    : impl(std::make_unique<Impl>(std::move(file), std::move(executable), std::move(model),
                                  controlledSemantic))
{
}
void LibraryService::publishControlledSemantic(uint64_t request, std::vector<Asset> candidates)
{
    std::lock_guard lock(impl->mutex);
    impl->semanticResult.request = request;
    impl->semanticResult.example = impl->exampleMode;
    impl->semanticResult.minimumSimilarity = impl->reference.minimumSimilarity;
    impl->semanticResult.candidates = std::move(candidates);
    ++impl->semanticResultVersion;
    impl->wake.notify_all();
}
LibraryService::~LibraryService() = default;
juce::File LibraryService::catalogFile() const { return impl->database; }
void LibraryService::addFolder(const juce::File &f)
{
    {
        std::lock_guard lock(impl->mutex);
        impl->additions.push_back(f);
    }
    impl->wake.notify_all();
}
void LibraryService::setLayoutOverride(const Asset &asset, const ChannelLayout &layout)
{
    std::lock_guard lock(impl->mutex);
    if (impl->layoutEdits.size() < 64)
        impl->layoutEdits.push_back({asset.id, asset.revision, layout});
    impl->wake.notify_all();
}
std::future<Asset> LibraryService::indexGenerated(Asset asset, juce::String collection, juce::var receipt)
{
    Impl::GeneratedImport job{std::move(asset), std::move(collection), {}, receipt};
    auto result = job.result.get_future();
    std::lock_guard lock(impl->mutex);
    if (impl->stopping || impl->generatedImports.size() >= 8)
        throw std::runtime_error("Library index is unavailable");
    impl->generatedImports.push_back(std::move(job));
    impl->wake.notify_all();
    return result;
}
void LibraryService::rescan()
{
    {
        std::lock_guard lock(impl->mutex);
        impl->requestedRescan = true;
    }
    impl->wake.notify_all();
}
void LibraryService::inspectBookmark(BookmarkRequest item, const juce::String &key)
{
    std::lock_guard lock(impl->mutex);
    impl->bookmarkInspection = std::pair{std::move(item), key};
    impl->wake.notify_all();
}
bool LibraryService::editCollections(CollectionEdit edit)
{
    std::lock_guard lock(impl->mutex);
    if (edit.items.size() > 4096 || impl->collectionEdits.size() >= 64)
    {
        impl->current.organization.error = "Metadata queue is full; wait and try again";
        impl->notifyLocked();
        return false;
    }
    impl->collectionEdits.push_back(std::move(edit));
    impl->wake.notify_all();
    return true;
}
void LibraryService::pause(bool value)
{
    {
        std::lock_guard lock(impl->mutex);
        impl->pauseRequested = value;
    }
    impl->wake.notify_all();
}
uint64_t LibraryService::search(const juce::String &q, const juce::String &root, const juce::String &scope)
{
    std::lock_guard lock(impl->mutex);
    impl->exampleMode = false;
    impl->current.exampleMode = false;
    impl->current.reference.loading = false;
    impl->text = q;
    impl->filter = root;
    impl->collectionScope = scope;
    impl->desiredOffset = 0;
    impl->desiredLimit = 20;
    impl->pagePending = false;
    impl->pinned = false;
    impl->current.selectedAssetId.clear();
    impl->current.selectedAssetRemoved = false;
    impl->semanticDue = monotonicMs() + 200;
    ++impl->semanticInputVersion;
    ++impl->workToken;
    auto id = ++impl->request;
    impl->wake.notify_all();
    return id;
}
void LibraryService::page(int offset, int limit)
{
    std::lock_guard lock(impl->mutex);
    impl->desiredOffset = std::max(0, offset);
    impl->desiredLimit = juce::jlimit(1, 512, limit);
    impl->pagePending = true;
    impl->pinned = true;
    impl->wake.notify_all();
}
bool LibraryService::pinSelection(uint64_t id, uint64_t serial, const juce::String &assetId)
{
    std::lock_guard lock(impl->mutex);
    // A displayed older page of this same query still names a stable identity.
    // The view retains that identity even if it has left the current prefix.
    if (id != impl->request || id != impl->current.request || serial > impl->current.pageSerial ||
        assetId.isEmpty())
        return false;
    impl->pinned = true;
    impl->current.selectedAssetId = assetId;
    impl->current.selectedAssetRemoved = false;
    return true;
}
bool LibraryService::pageForRequest(uint64_t id, int offset, int limit)
{
    std::lock_guard lock(impl->mutex);
    if (id != impl->request || id != impl->current.request)
        return false;
    impl->desiredOffset = std::max(0, offset);
    impl->desiredLimit = juce::jlimit(1, 512, limit);
    impl->pagePending = true;
    impl->pinned = true;
    impl->wake.notify_all();
    return true;
}
void LibraryService::setChangeCallback(std::function<void()> callback)
{
    std::lock_guard lock(impl->mutex);
    impl->changed = std::move(callback);
}
LibrarySnapshot LibraryService::snapshot()
{
    std::lock_guard lock(impl->mutex);
    return impl->current;
}
juce::var LibraryService::queryDiagnostics()
{
    std::lock_guard lock(impl->mutex);
    auto *o = new juce::DynamicObject();
    o->setProperty("request", (juce::int64)impl->request);
    o->setProperty("workToken", (juce::int64)impl->workToken.load());
    o->setProperty("input", (juce::int64)impl->semanticInputVersion);
    o->setProperty("handledInput", (juce::int64)impl->semanticHandledInput.load());
    o->setProperty("semanticWorkRequest", (juce::int64)impl->semanticWorkRequest.load());
    o->setProperty("semanticWorkPhase", impl->semanticWorkPhase.load());
    o->setProperty("semanticResultRequest", (juce::int64)impl->semanticResult.request);
    o->setProperty("semanticResultVersion", (juce::int64)impl->semanticResultVersion);
    o->setProperty("readerSession", (juce::int64)impl->readerSession.load());
    o->setProperty("readerSemanticSeen", (juce::int64)impl->readerSemanticSeen.load());
    o->setProperty("completed", (juce::int64)impl->current.semanticCompletedRequest);
    o->setProperty("pinned", impl->pinned);
    return juce::var(o);
}
bool LibraryService::ready() const
{
    return impl->initialized;
}
void LibraryService::installModel(const juce::File &source)
{
    std::lock_guard lock(impl->mutex);
    if (!impl->inference || impl->current.semantic.installing)
        return;
    impl->importModelDirectory = source;
    impl->installRequested = true;
    impl->installCancelled = false;
    impl->wake.notify_all();
}
void LibraryService::cancelModelInstall()
{
    impl->installCancelled = true;
}
void LibraryService::retryAnalysis()
{
    std::lock_guard lock(impl->mutex);
    impl->aiRetryRequested = true;
    impl->wake.notify_all();
}
void LibraryService::semanticNow()
{
    std::lock_guard lock(impl->mutex);
    impl->semanticDue = monotonicMs();
    ++impl->semanticInputVersion;
    impl->wake.notify_all();
}
void LibraryService::prioritizeAnalysis(const juce::String &id)
{
    std::lock_guard lock(impl->mutex);
    impl->preferredAnalysis = id;
    impl->wake.notify_all();
}
void LibraryService::setAuditionActive(bool value)
{
    impl->auditionActive = value;
}
uint64_t LibraryService::findSimilar(const AudioReference &reference, const juce::String &root,
                                     const juce::String &scope)
{
    if (!std::isfinite(reference.minimumSimilarity) || reference.minimumSimilarity < 0 ||
        reference.minimumSimilarity > 1)
        throw std::runtime_error("Minimum similarity must be between 0 and 1");
    std::lock_guard lock(impl->mutex);
    impl->reference = reference;
    if (!impl->exampleMode)
        impl->exampleFacets = metadataTokens(impl->text, true);
    impl->exampleMode = true;
    impl->current.exampleMode = true;
    impl->text.clear();
    impl->filter = root;
    impl->collectionScope = scope;
    impl->desiredOffset = 0;
    impl->desiredLimit = 20;
    impl->pagePending = false;
    impl->pinned = false;
    impl->semanticDue = monotonicMs();
    ++impl->semanticInputVersion;
    ++impl->workToken;
    auto id = ++impl->request;
    auto &state = impl->current.reference;
    state = {};
    state.reference = reference;
    state.present = true;
    state.generation = id;
    state.loading = true;
    state.status =
        impl->aiLoaded ? "Preparing reference" : "Waiting for the local model; install it if needed";
    impl->notifyLocked();
    return id;
}
void LibraryService::clearReference()
{
    std::lock_guard lock(impl->mutex);
    impl->exampleMode = false;
    impl->current.exampleMode = false;
    impl->current.reference = {};
    ++impl->workToken;
    ++impl->semanticInputVersion;
    impl->wake.notify_all();
}
} // namespace sp
