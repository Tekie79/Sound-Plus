#include "LibraryService.h"
#include "MacSources.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <filesystem>

namespace sp
{
LibraryService::LibraryService(juce::File db) : database(std::move(db))
{
    indexer = std::thread([this] { indexLoop(); });
    reader = std::thread([this] { searchLoop(); });
}
LibraryService::~LibraryService()
{
    stopping = true;
    wake.notify_all();
    if (indexer.joinable())
        indexer.join();
    if (reader.joinable())
        reader.join();
}
void LibraryService::addFolder(const juce::File &f)
{
    {
        std::lock_guard lock(mutex);
        additions.push_back(f);
    }
    wake.notify_all();
}
void LibraryService::rescan()
{
    retryRequested = true;
    dirty = true;
    wake.notify_all();
}
uint64_t LibraryService::search(const juce::String &q, const juce::String &root)
{
    uint64_t id;
    {
        std::lock_guard lock(mutex);
        text = q;
        rootFilter = root;
        id = ++request;
    }
    wake.notify_all();
    return id;
}
LibrarySnapshot LibraryService::snapshot()
{
    std::lock_guard lock(mutex);
    return current;
}
void LibraryService::publish()
{
    ++version;
    wake.notify_all();
}
void LibraryService::scan(Catalog &db, Root root)
{
    auto resolved = resolveRoot(root.path, root.volume, root.volumeRelative);
    root.online = resolved.isNotEmpty();
    root.scanning = root.online;
    root.error.clear();
    if (root.online)
        root.path = resolved;
    db.rootState(root);
    publish();
    if (!root.online)
        return;
    const auto scanId = juce::Time::getHighResolutionTicks();
    bool transaction = false;
    try
    {
        namespace fs = std::filesystem;
        fs::recursive_directory_iterator it(fs::path(root.path.toStdString()), fs::directory_options::none),
            end;
        int batch = 0;
        bool first = true;
        auto last = juce::Time::getMillisecondCounter();
        db.begin();
        transaction = true;
        for (; it != end && !stopping; ++it)
        {
            const auto &entry = *it;
            auto filename = entry.path().filename().string();
            if (entry.is_symlink())
            {
                if (entry.is_directory())
                    it.disable_recursion_pending();
                continue;
            }
            if (entry.is_directory())
            {
                if (filename.starts_with('.') || filename == "__MACOSX" ||
                    entry.path().extension() == ".app" ||
                    juce::File(juce::String::fromUTF8(entry.path().c_str())) == database.getParentDirectory())
                    it.disable_recursion_pending();
                continue;
            }
            juce::File file(juce::String::fromUTF8(entry.path().c_str()));
            if (!supportedExtension(file))
                continue;
            FileStamp stamp;
            if (!fileStamp(file, stamp))
                continue;
            db.discover(root, file.getRelativePathFrom(juce::File(root.path)), stamp, scanId);
            ++batch;
            if (first || batch >= 32 || juce::Time::getMillisecondCounter() - last >= 100)
            {
                db.commit();
                transaction = false;
                publish();
                first = false;
                batch = 0;
                last = juce::Time::getMillisecondCounter();
                db.begin();
                transaction = true;
            }
        }
        db.commit();
        transaction = false;
        // Missing records become offline only after a complete, successful enumeration.
        if (!stopping && resolveRoot(root.path, root.volume, root.volumeRelative).isNotEmpty())
            db.finishScan(root, scanId);
    }
    catch (const std::exception &e)
    {
        if (transaction)
            db.exec("ROLLBACK");
        root.error = juce::String::fromUTF8(e.what());
    }
    root.online = resolveRoot(root.path, root.volume, root.volumeRelative).isNotEmpty();
    root.scanning = false;
    db.rootState(root);
    publish();
}
void LibraryService::indexLoop()
{
    try
    {
        if (!database.getParentDirectory().createDirectory())
            throw std::runtime_error("Cannot create the catalog directory");
        Catalog db(database);
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<SourceWatch> watch;
        auto rebuildWatch = [&]
        {
            std::vector<juce::String> paths;
            for (auto &r : db.roots())
                if (r.online)
                    paths.push_back(r.path);
            watch =
                std::make_unique<SourceWatch>(paths, dirty, database.getParentDirectory().getFullPathName());
        };
        rebuildWatch(); // Events are registered before enumeration, including first import below.
        initialized = true;
        publish();
        auto lastAvailability = juce::Time::getMillisecondCounter();
        auto lastReconcile = lastAvailability;
        while (!stopping)
        {
            std::vector<juce::File> added;
            {
                std::lock_guard lock(mutex);
                added.swap(additions);
                if (!added.empty())
                    current.error.clear();
            }
            for (auto &f : added)
            {
                if (f.isDirectory())
                    db.addRoot(f);
                else
                {
                    std::lock_guard lock(mutex);
                    current.error = "Folder is unavailable: " + f.getFullPathName();
                }
            }
            bool needsScan = dirty.exchange(false) || !added.empty();
            if (!added.empty())
                rebuildWatch();
            auto now = juce::Time::getMillisecondCounter();
            if (now - lastAvailability >= 2000)
            {
                bool availabilityChanged = false;
                for (auto root : db.roots())
                {
                    auto path = resolveRoot(root.path, root.volume, root.volumeRelative);
                    bool online = path.isNotEmpty();
                    if (online != root.online || (online && path != root.path))
                    {
                        root.online = online;
                        if (online)
                            root.path = path;
                        root.scanning = false;
                        db.rootState(root);
                        availabilityChanged = true;
                    }
                }
                if (availabilityChanged)
                {
                    publish();
                    rebuildWatch();
                    needsScan = true;
                }
                lastAvailability = now;
            }
            // Reconciliation also covers shares, dropped FSEvents, wake and in-place file edits.
            if (now - lastReconcile >= 30000)
            {
                needsScan = true;
                lastReconcile = now;
            }
            if (needsScan)
                for (auto root : db.roots())
                {
                    if (stopping)
                        break;
                    scan(db, root);
                }
            if (retryRequested.exchange(false))
                db.exec("UPDATE assets SET state='pending',error='' WHERE state='failed' AND online=1");
            auto pending = db.pending();
            for (auto &a : pending)
            {
                if (stopping)
                    break;
                FileStamp before;
                if (!fileStamp(juce::File(a.path), before))
                {
                    dirty = true;
                    continue;
                }
                if (before.modified != a.modified || before.size != a.size || before.inode != a.inode)
                {
                    dirty = true;
                    continue;
                }
                auto age = juce::Time::currentTimeMillis() - before.modified / 1000000;
                if (age < 750)
                    continue; // Do not read a recording that is still being written.
                ++headers;
                std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(juce::File(a.path)));
                FileStamp after;
                if (!fileStamp(juce::File(a.path), after) || after.modified != before.modified ||
                    after.size != before.size || after.inode != before.inode)
                {
                    dirty = true;
                    continue;
                }
                if (r && r->sampleRate > 0 && r->numChannels > 0 && r->lengthInSamples > 0)
                    db.metadata(a, (double)r->lengthInSamples / r->sampleRate, (int)r->numChannels,
                                r->sampleRate, {});
                else
                    db.metadata(a, 0, 0, 0, "Unsupported codec, empty or unreadable audio. Rescan to retry.");
            }
            if (!pending.empty())
                publish();
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(pending.empty() ? 200 : 30),
                          [&] { return stopping.load() || !additions.empty(); });
        }
    }
    catch (const std::exception &e)
    {
        std::lock_guard lock(mutex);
        current.error = juce::String::fromUTF8(e.what());
        initialized = true;
        wake.notify_all();
    }
}
void LibraryService::searchLoop()
{
    while (!initialized && !stopping)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stopping)
        return;
    try
    {
        Catalog db(database, true);
        uint64_t lastVersion = 0, lastRequest = 0;
        while (!stopping)
        {
            juce::String q, filter;
            uint64_t id, v = version.load();
            {
                std::unique_lock lock(mutex);
                if (v == lastVersion && request == lastRequest)
                {
                    wake.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                q = text;
                filter = rootFilter;
                id = request;
            }
            auto results = db.search(q, filter);
            auto roots = db.roots();
            {
                std::lock_guard lock(mutex);
                if (id == request)
                {
                    current.results = std::move(results);
                    current.roots = std::move(roots);
                    current.request = id;
                    current.version = v;
                    current.headersRead = headers.load();
                }
            }
            lastVersion = v;
            lastRequest = id;
        }
    }
    catch (const std::exception &e)
    {
        std::lock_guard lock(mutex);
        current.error = juce::String::fromUTF8(e.what());
    }
}
} // namespace sp
