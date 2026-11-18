#include "PerformanceTests.h"
#include "LibraryService.h"
#include "AudioIPC.h"
#include "Performance.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <set>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace
{
void check(bool value, const char *reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
template <class F> void wait(F condition, const char *reason, int timeout = 20000)
{
    auto end = sp::monotonicMs() + timeout;
    while (!condition())
    {
        if (sp::monotonicMs() > end)
            throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void makeWav(const juce::File &file, int seconds = 1)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat format;
    auto out = file.createOutputStream();
    check(out != nullptr, "Fixture output");
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(out.release(), 48000, 1, 16, {}, 0));
    juce::AudioBuffer<float> block(1, seconds * 48000);
    for (int i = 0; i < block.getNumSamples(); ++i)
        block.setSample(0, i, 0.1f * std::sin((float)i * .05f));
    check(writer && writer->writeFromAudioSampleBuffer(block, 0, block.getNumSamples()), "Fixture PCM");
}
} // namespace
void runPerformanceTests(const juce::File &base, const juce::File &executable)
{
    check(base.getFileName() == "performance-test-output",
          "Use the dedicated performance-test-output directory");
    base.deleteRecursively();
    base.createDirectory();
    auto rootA = base.getChildFile("A"), rootB = base.getChildFile("B"),
         database = base.getChildFile("catalog/library.sqlite");
    auto seed = base.getChildFile("seed.wav");
    makeWav(seed);
    const int count = 4096;
    for (int i = 0; i < count; ++i)
    {
        auto file = rootA.getChildFile("chunk" + juce::String(i / 256) + "/bulk " +
                                       juce::String(i).paddedLeft('0', 5) + ".wav");
        file.getParentDirectory().createDirectory();
        check(seed.copyFileTo(file), "Copy fixture");
    }
    for (int i = 0; i < 40; ++i)
    {
        auto file = rootB.getChildFile("bulk " + juce::String(i).paddedLeft('0', 5) + ".wav");
        file.getParentDirectory().createDirectory();
        check(seed.copyFileTo(file), "Copy second root");
    }
    juce::String rootId;
    {
        sp::LibraryService service(database);
        service.pause(true);
        service.addFolder(rootA);
        service.addFolder(rootB);
        wait([&] { return service.ready() && service.snapshot().progress.paused; },
             "Initial pause reaches safe boundary");
        auto empty = service.search("bulk");
        wait([&] { return service.snapshot().request == empty; }, "Search while initially paused");
        check(service.snapshot().total == 0, "No enumeration while paused");
        service.pause(false);
        wait(
            [&]
            {
                auto s = service.snapshot();
                int total = 0;
                for (auto &r : s.roots)
                    total += r.discovered;
                return total > 0 && total < count + 40;
            },
            "Observe progressive commits");
        service.pause(true);
        wait([&] { return service.snapshot().progress.paused; }, "Pause active indexing at commit boundary");
        auto paused = service.snapshot();
        int committed = 0;
        for (auto &r : paused.roots)
            committed += r.discovered;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int later = 0;
        for (auto &r : service.snapshot().roots)
            later += r.discovered;
        check(later == committed, "Pause holds committed progress");
        auto q = service.search("bulk");
        wait([&] { return service.snapshot().request == q; }, "Search remains usable while paused");
        std::cout << "Pause acknowledged at " << committed << " committed files\n";
        service.pause(false);
        wait(
            [&]
            {
                auto s = service.snapshot();
                int ready = 0;
                for (auto &r : s.roots)
                    ready += r.ready;
                return ready == count + 40;
            },
            "Resume completes every header", 30000);
        for (auto &r : service.snapshot().roots)
            if (r.path == rootA.getFullPathName())
                rootId = r.id;
        check(rootId.isNotEmpty(), "Root identity");
        q = service.search("bulk", rootId);
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.request == q && s.total == count;
            },
            "Full result count exceeds old cap");
        std::set<juce::String> ids;
        for (int offset = 0; offset < count; offset += 256)
        {
            service.page(offset, 256);
            wait(
                [&]
                {
                    auto s = service.snapshot();
                    return s.request == q && s.offset == offset &&
                           (int)s.results.size() == std::min(256, count - offset);
                },
                "Paged rows loaded");
            for (auto &a : service.snapshot().results)
                check(ids.insert(a.id).second, "No duplicate IDs between pages");
            if (offset == 256)
            {
                check(seed.copyFileTo(rootA.getChildFile("chunk0/bulk new.wav")),
                      "Concurrent import during browsing");
                service.rescan();
            }
        }
        check((int)ids.size() == count, "Every matching recording accessible beyond 1000");
        wait(
            [&]
            {
                auto s = service.snapshot();
                for (auto &r : s.roots)
                    if (r.id == rootId)
                        return r.ready == count + 1;
                return false;
            },
            "Import progresses while result snapshot is pinned");
        check(service.snapshot().total == count && service.snapshot().newResultsAvailable,
              "Pinned result ordering survives new imports");
        q = service.search("bulk", rootId);
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.request == q && s.total == count + 1;
            },
            "Refresh includes newly imported ID");
        auto before = sp::performance.fileStats.load();
        check(seed.copyFileTo(rootA.getChildFile("chunk0/bulk live.wav")), "Live file addition");
        q = service.search("bulk live");
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.request == q && s.total == 1 && s.results[0].state == "ready";
            },
            "Changed-directory event reaches search");
        check(sp::performance.fileStats.load() - before < 1024,
              "Ordinary update does not stat the whole library");
        for (int i = 0; i < 500; ++i)
            service.search("discarded " + juce::String(i), rootId);
        juce::String idB;
        for (auto &r : service.snapshot().roots)
            if (r.path == rootB.getFullPathName())
                idB = r.id;
        q = service.search("bulk", idB);
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.request == q && s.total == 40;
            },
            "Rapid typing and source switch produce latest results");
        for (auto &a : service.snapshot().results)
            check(a.rootId == idB, "No stale-source row");
        q = service.search("bulk 00000.wav", idB);
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.request == q && s.total == 1;
            },
            "Flat-directory file selected");
        auto flatId = service.snapshot().results[0].id;
        check(rootB.getChildFile("bulk 00000.wav").deleteFile(), "Delete disposable flat fixture");
        wait(
            [&]
            {
                auto s = service.snapshot();
                return !s.results.empty() && !s.results[0].online;
            },
            "Missing flat-directory asset remains visible offline");
        check(seed.copyFileTo(rootB.getChildFile("bulk 00000.wav")), "Restore flat fixture");
        sp::FileStamp restored;
        check(sp::fileStamp(rootB.getChildFile("bulk 00000.wav"), restored), "Restored stamp");
        wait(
            [&]
            {
                auto s = service.snapshot();
                return !s.results.empty() && s.results[0].online && s.results[0].state == "ready" &&
                       s.results[0].modified == restored.modified && s.results[0].inode == restored.inode;
            },
            "Restored flat asset available");
        check(service.snapshot().results[0].id == flatId, "Restored file identity");
        auto alias = base.getChildFile("alias-A");
        check(symlink(rootA.getFullPathName().toRawUTF8(), alias.getFullPathName().toRawUTF8()) == 0,
              "Create root alias");
        service.addFolder(alias);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(service.snapshot().roots.size() == 2 && service.snapshot().error.isEmpty(),
              "Canonical root aliases do not duplicate or fail the writer");
        q = service.search("");
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.request == q && s.total == count + 42;
            },
            "Clearing query returns every source");
        service.page(2048);
        wait([&] { return service.snapshot().offset == 2048; }, "Deep page for viewport");
        sp::Catalog read(database, true);
        check(read.count() == count + 42, "No duplicate durable records");
    }
    auto filesBefore = sp::performance.fileStats.load(), entriesBefore = sp::performance.entries.load(),
         headersBefore = sp::performance.headers.load();
    {
        sp::LibraryService reopened(database);
        wait(
            [&]
            {
                return reopened.snapshot().progress.completedJobs >= 2 &&
                       reopened.snapshot().progress.activeWorkers == 0;
            },
            "Reopen reconciliation completes");
        check(sp::performance.fileStats == filesBefore && sp::performance.entries == entriesBefore,
              "Unchanged reopen uses directory fingerprints without file enumeration or file stat");
        check(sp::performance.headers == headersBefore && reopened.snapshot().headersRead == 0,
              "Zero unchanged header jobs");
        auto audioFile = base.getChildFile("long.wav");
        makeWav(audioFile, 3);
        sp::AudioClient audio(executable);
        wait([&] { return (double)audio.status()["sampleRate"] > 0; }, "Audio output ready");
        audio.play("paused-test", audioFile);
        wait([&] { return (bool)audio.status()["playing"]; }, "Audio playing");
        reopened.pause(true);
        wait([&] { return reopened.snapshot().progress.paused; }, "Idle pause");
        auto frames = (juce::int64)audio.status()["frames"];
        for (int i = 0; i < 40; ++i)
        {
            reopened.search(i % 2 ? "bulk" : "missing");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check((bool)audio.status()["playing"] && (juce::int64)audio.status()["frames"] > frames,
              "Audition and queries continue while indexing paused");
        audio.stop();
    }
    std::cout << "PASS paging, concurrent import identity, cancellation, source filters, pause/resume, "
                 "incremental scope, reopen and paused audition\n";
}
