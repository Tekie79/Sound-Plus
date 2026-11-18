#include "LibraryService.h"
#include "MacSources.h"
#include "Performance.h"
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
        juce::String error;
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
    };
    juce::File database;
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
    std::deque<Job> jobs;
    std::deque<Batch> batches;
    bool requestedRescan = false;
    juce::String text, filter;
    uint64_t request = 1;
    int desiredOffset = 0, desiredLimit = 20;
    bool pagePending = false, pinned = false;
    std::function<void()> changed;
    LibrarySnapshot current;
    DirectoryEvents events; // Outlives root watchers and all owned workers.
    std::vector<std::shared_ptr<Runtime>> roots;
    int completedJobs = 0;
    explicit Impl(juce::File file) : database(std::move(file))
    {
        writer = std::thread([this] { indexLoop(); });
        reader = std::thread([this] { searchLoop(); });
    }
    ~Impl()
    {
        stopping = true;
        pauseRequested = false;
        wake.notify_all();
        if (writer.joinable())
            writer.join();
        if (reader.joinable())
            reader.join();
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
        if (!fileStamp(file, after) || !same(before, after))
        {
            events.push(file.getParentDirectory().getFullPathName());
            return;
        }
        Header h;
        h.relative = relative;
        h.stamp = after;
        if (audio && audio->sampleRate > 0 && audio->lengthInSamples > 0 && audio->numChannels > 0)
        {
            h.seconds = (double)audio->lengthInSamples / audio->sampleRate;
            h.rate = audio->sampleRate;
            h.channels = (int)audio->numChannels;
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
            struct statfs volume{};
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
                    struct stat st{};
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
            progress.paused =
                pauseRequested && batches.empty() && jobs.empty() && activeWorkers == parkedWorkers;
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
            progress.stage = "Up to date";
        std::lock_guard lock(mutex);
        current.roots = std::move(rows);
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
            initialized = true;
            publish();
            auto lastStatus = monotonicMs();
            auto lastWake = lastStatus;
            auto lastWall = juce::Time::currentTimeMillis();
            while (!stopping)
            {
                std::vector<juce::File> added;
                std::deque<Batch> ready;
                bool rescan = false;
                {
                    std::lock_guard lock(mutex);
                    added.swap(additions);
                    ready.swap(batches);
                    rescan = std::exchange(requestedRescan, false);
                    wake.notify_all();
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
                if (rescan)
                {
                    db.exec("UPDATE assets SET state='pending',error='' WHERE state='failed'");
                    for (auto &r : roots)
                    {
                        r->full = true;
                        r->force = true;
                    }
                    publish();
                }
                for (auto &b : ready)
                {
                    if (b.barrier)
                    {
                        b.barrier->set_value();
                        continue;
                    }
                    if (b.runtime->cancelled)
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
                            db.metadata(a, h.seconds, h.channels, h.rate, h.error);
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
                        if (!b.names.empty())
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
                wake.wait_for(
                    lock, std::chrono::milliseconds(10),
                    [&] { return stopping || !batches.empty() || !additions.empty() || requestedRescan; });
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
            uint64_t lastRequest = 0, lastVersion = 0, session = 0, lastMembership = 0;
            double lastBackground = 0;
            bool transaction = false;
            while (!stopping)
            {
                juce::String q, root;
                uint64_t id, token, v = version.load(), membership = membershipVersion.load();
                int offset = 0, limit = 20;
                bool page = false, isPinned = false;
                {
                    std::unique_lock lock(mutex);
                    if (request == lastRequest && v == lastVersion && !pagePending)
                    {
                        wake.wait_for(lock, std::chrono::milliseconds(50));
                        continue;
                    }
                    if (request == lastRequest && !pagePending && !pinned && membership != lastMembership &&
                        monotonicMs() - lastBackground < 100)
                    {
                        wake.wait_for(lock, std::chrono::milliseconds(5));
                        continue;
                    }
                    id = request;
                    token = workToken.load();
                    q = text;
                    root = filter;
                    page = pagePending;
                    offset = desiredOffset;
                    limit = desiredLimit;
                    isPinned = pinned;
                    pagePending = false;
                }
                try
                {
                    db.cancelWhen(&workToken, token);
                    if (id == lastRequest && v != lastVersion && (isPinned || membership == lastMembership) &&
                        !page)
                    {
                        int at = 0, size = 20;
                        {
                            std::lock_guard lock(mutex);
                            at = current.offset;
                            size = (int)current.results.size();
                        }
                        auto rows = db.page(at, size);
                        std::lock_guard lock(mutex);
                        if (id != request)
                            continue;
                        current.results = std::move(rows);
                        ++current.pageSerial;
                        current.newResultsAvailable = isPinned && membership != lastMembership;
                        current.version = v;
                        lastVersion = v;
                        notifyLocked();
                        continue;
                    }
                    if (page && id == session)
                    {
                        auto rows = db.page(offset, limit);
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
                    db.exec("BEGIN");
                    transaction = true;
                    auto rows = db.search(q, root, 20);
                    int count = db.countMatches(q, root);
                    {
                        std::lock_guard lock(mutex);
                        if (id == request && token == workToken)
                        {
                            current.results = std::move(rows);
                            current.total = count;
                            current.offset = 0;
                            current.newResultsAvailable = false;
                            current.request = id;
                            current.version = v;
                            ++current.pageSerial;
                            notifyLocked();
                        }
                    }
                    db.materialize(q, root);
                    db.exec("COMMIT");
                    transaction = false;
                    session = id;
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
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "Library reader failure: %s\n", error.what());
            std::lock_guard lock(mutex);
            current.error = juce::String::fromUTF8(error.what());
            notifyLocked();
        }
    }
};
LibraryService::LibraryService(juce::File file) : impl(std::make_unique<Impl>(std::move(file))) {}
LibraryService::~LibraryService() = default;
void LibraryService::addFolder(const juce::File &f)
{
    {
        std::lock_guard lock(impl->mutex);
        impl->additions.push_back(f);
    }
    impl->wake.notify_all();
}
void LibraryService::rescan()
{
    {
        std::lock_guard lock(impl->mutex);
        impl->requestedRescan = true;
    }
    impl->wake.notify_all();
}
void LibraryService::pause(bool value)
{
    impl->pauseRequested = value;
    impl->wake.notify_all();
}
uint64_t LibraryService::search(const juce::String &q, const juce::String &root)
{
    std::lock_guard lock(impl->mutex);
    impl->text = q;
    impl->filter = root;
    impl->desiredOffset = 0;
    impl->desiredLimit = 20;
    impl->pagePending = false;
    impl->pinned = false;
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
bool LibraryService::ready() const
{
    return impl->initialized;
}
} // namespace sp
