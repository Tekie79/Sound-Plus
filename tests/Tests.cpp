#include "Catalog.h"
#include "LibraryService.h"
#include "AudioStream.h"
#include "AudioIPC.h"
#include "Performance.h"
#include "PerformanceTests.h"
#include "SemanticTests.h"
#include "SimilarTests.h"
#include "DesignTests.h"
#include "DesignSessionTests.h"
#include "CanvasGestureTests.h"
#include "TransportInteractionTests.h"
#include "FadeTests.h"
#include "IconTests.h"
#include "ViewCommandTests.h"
#include "PerformanceStateTests.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <thread>
#include <cmath>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
namespace
{
int checks = 0;
void require(bool value, const char *reason)
{
    ++checks;
    if (!value)
        throw std::runtime_error(reason);
}
template <class F> void until(F f, const char *reason, int ms = 12000)
{
    auto end = juce::Time::getMillisecondCounter() + ms;
    while (!f())
    {
        if (juce::Time::getMillisecondCounter() >= end)
            throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ++checks;
}
void fixture(const juce::File &file, int channels = 1, int rate = 48000, double seconds = 0.25,
             double frequency = 330)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;
    juce::AudioFormat &format = file.hasFileExtension("aiff")   ? static_cast<juce::AudioFormat &>(aiff)
                                : file.hasFileExtension("flac") ? static_cast<juce::AudioFormat &>(flac)
                                                                : static_cast<juce::AudioFormat &>(wav);
    auto stream = file.createOutputStream();
    require(stream != nullptr, "Fixture stream");
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), rate, (unsigned)channels, 16, {}, 0));
    require(writer != nullptr, "Fixture writer");
    int samples = (int)(rate * seconds);
    juce::AudioBuffer<float> b(channels, samples);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < samples; ++i)
            b.setSample(c, i,
                        (float)(0.2 * std::sin(2 * juce::MathConstants<double>::pi * (frequency + c * 70) *
                                               i / rate)));
    require(writer->writeFromAudioSampleBuffer(b, 0, samples), "Write generated PCM");
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        if (argc >= 4 && juce::String(argv[1]) == "--performance-state")
        {
            runPerformanceStateTests(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--view-commands")
        {
            runViewCommandTests(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 3 && juce::String(argv[1]) == "--icon-system")
        {
            runIconTests(juce::File(argv[2]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--designer-fades")
        {
            runFadeTests(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--selection-audition")
        {
            auto parent = juce::File(argv[2]);
            auto root = parent.getChildFile(juce::Uuid().toString());
            root.createDirectory();
            for (const auto *folder : {"A", "B", "C"})
                fixture(root.getChildFile("sounds/" + juce::String(folder) + "/clip.wav"), 1, 48000, 4);
            auto plan = sp::message("test");
            plan.getDynamicObject()->setProperty("mode", "selection-audition");
            auto planFile = root.getChildFile("plan.json"), resultFile = root.getChildFile("result.json");
            require(planFile.replaceWithText(juce::JSON::toString(plan)), "Selection plan written");
            juce::ChildProcess app;
            require(app.start(juce::StringArray{
                        argv[3], "--data-dir", root.getChildFile("catalog").getFullPathName(), "--add-folder",
                        root.getChildFile("sounds").getFullPathName(), "--benchmark-plan",
                        planFile.getFullPathName(), "--benchmark-output", resultFile.getFullPathName()}),
                    "Native selection application launched");
            if (!app.waitForProcessToFinish(30000))
            {
                app.kill();
                throw std::runtime_error("Native selection application timed out");
            }
            auto result = juce::JSON::parse(resultFile);
            require(resultFile.copyFileTo(parent.getChildFile("latest.json")), "Selection evidence retained");
            require((bool)result["success"], result["error"].toString().toRawUTF8());
            require(app.getExitCode() == 0, "Native selection application exited normally");
            std::cout << "Native selection audition passed in full Library and Designer browser\n";
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--designer-transport")
        {
            runTransportInteractionTests(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 3 && juce::String(argv[1]) == "--canvas-gestures")
        {
            runCanvasGestureTests(juce::File(argv[2]));
            return 0;
        }
        if (argc >= 3 && juce::String(argv[1]) == "--designer-session")
        {
            runDesignSessionTests(juce::File(argv[2]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--designer")
        {
            runDesignTests(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 3 && juce::String(argv[1]) == "--prepare-designer-workload")
        {
            prepareDesignWorkload(juce::File(argv[2]));
            return 0;
        }
        if (argc >= 5 && (juce::String(argv[1]) == "--interrupt-designer-save" ||
                          juce::String(argv[1]) == "--interrupt-designer-render"))
        {
            interruptDesignOperation(juce::String(argv[1]).endsWith("render"), juce::File(argv[2]),
                                     juce::File(argv[3]), juce::File(argv[4]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--similar-integrity")
        {
            runSimilarIntegrity(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 6 && juce::String(argv[1]) == "--similar-workflow")
        {
            runSimilarWorkflow(juce::File(argv[2]), juce::File(argv[3]), juce::File(argv[4]),
                               juce::File(argv[5]));
            return 0;
        }
        if (argc >= 6 && juce::String(argv[1]) == "--capacity-accuracy")
        {
            runCapacityAccuracy(juce::File(argv[2]), juce::File(argv[3]), juce::File(argv[4]),
                                juce::File(argv[5]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--capacity-recovery")
        {
            runCapacityRecovery(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--semantic-integrity")
        {
            runSemanticIntegrity(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 6 && juce::String(argv[1]) == "--semantic-workflow")
        {
            runSemanticWorkflow(juce::File(argv[2]), juce::File(argv[3]), juce::File(argv[4]),
                                juce::File(argv[5]));
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--long-decode")
        {
            juce::Array<juce::var> rows;
            double totalSeconds = 0, start = sp::monotonicMs();
            for (int i = 0; i < 6; ++i)
            {
                auto file = juce::File(argv[2]).getChildFile("long-" + juce::String(i) + ".wav");
                sp::PcmStream stream(file, 48000);
                juce::AudioBuffer<float> output(2, 512);
                int64_t frames = 0;
                float gain = 1;
                double began = sp::monotonicMs(), energy = 0;
                while (!stream.ended())
                {
                    stream.fill();
                    output.clear();
                    int n = stream.read(output.getArrayOfWritePointers(), 2, 512, gain, 1);
                    frames += n;
                    for (int j = 0; j < n; ++j)
                        energy += std::abs(output.getSample(0, j));
                }
                require(energy > 0, "Long corpus non-silent decode");
                auto *o = new juce::DynamicObject();
                o->setProperty("file", file.getFileName());
                o->setProperty("bytes", (juce::int64)file.getSize());
                o->setProperty("outputFrames", (juce::int64)frames);
                o->setProperty("audioSeconds", (double)frames / 48000);
                o->setProperty("wallMs", sp::monotonicMs() - began);
                rows.add(juce::var(o));
                totalSeconds += (double)frames / 48000;
            }
            auto *o = new juce::DynamicObject();
            o->setProperty("mode", "offline full-file decoding; separate from UI and device latency");
            o->setProperty("clips", rows);
            o->setProperty("audioSeconds", totalSeconds);
            o->setProperty("wallMs", sp::monotonicMs() - start);
            juce::File(argv[3]).replaceWithText(juce::JSON::toString(juce::var(o)));
            std::cout << "Decoded " << totalSeconds << " source seconds\n";
            return 0;
        }
        if (argc >= 3 && juce::String(argv[1]) == "--open-catalog")
        {
            sp::Catalog db(juce::File{argv[2]});
            db.exec("INSERT INTO names(names,rank) VALUES('integrity-check',1)");
            std::cout << "Catalog and FTS integrity checked: " << db.count() << " records\n";
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--performance")
        {
            runPerformanceTests(juce::File(argv[2]), juce::File(argv[3]));
            return 0;
        }
        if (argc >= 3 && juce::String(argv[1]) == "--slow-source-report")
        {
            auto base = juce::File(argv[2]);
            base.createDirectory();
            auto slow = base.getChildFile("A-slow"), healthy = base.getChildFile("Z-healthy");
            fixture(slow.getChildFile("slow.wav"));
            fixture(healthy.getChildFile("healthy.wav"));
            sp::performance.delayedRoot = slow.getFullPathName();
            sp::performance.delayMs = 5000;
            auto start = sp::monotonicMs();
            {
                sp::LibraryService service(base.getChildFile("library.sqlite"));
                service.addFolder(slow);
                service.addFolder(healthy);
                service.search("healthy");
                until([&] { return !service.snapshot().results.empty(); }, "Healthy root progresses", 15000);
                auto *o = new juce::DynamicObject();
                o->setProperty("healthyVisibleMs", sp::monotonicMs() - start);
                o->setProperty("controlledSlowRootDelayMs", 5000);
                base.getChildFile("result.json").replaceWithText(juce::JSON::toString(juce::var(o)));
            }
            std::cout << base.getChildFile("result.json").loadFileAsString() << "\n";
            return 0;
        }
        if (argc >= 4 && juce::String(argv[1]) == "--crash")
        {
            sp::Catalog db{juce::File(argv[2])};
            auto root = db.roots().front();
            sp::FileStamp st{12, 34, 56};
            db.begin();
            db.discover(root, "uncommitted.wav", st, 99);
            _exit(0);
        }
        require(argc >= 3, "Arguments: app executable, output directory");
        auto executable = juce::File(argv[1]), base = juce::File(argv[2]);
        base.deleteRecursively();
        base.createDirectory();
        auto root = base.getChildFile("fixture"), dbFile = base.getChildFile("catalog/library.sqlite");
        dbFile.getParentDirectory().createDirectory();
        fixture(root.getChildFile("A/soft mechanical click.wav"), 1, 48000, 0.25);
        fixture(root.getChildFile("B/soft mechanical click.wav"), 2, 44100, 0.5, 440);
        fixture(root.getChildFile(juce::String::fromUTF8("Unicode/雨 café.wav")), 1, 96000, 0.1);
        fixture(root.getChildFile("formats/airy.aiff"), 2, 44100, 0.2);
        fixture(root.getChildFile("formats/field.flac"), 1, 96000, 0.15);
        root.getChildFile("broken.wav").replaceWithText("not an audio file");
        root.getChildFile("readme.txt").replaceWithText("ignored");
        fixture(root.getChildFile(".cache/ignored.wav"));
        // Stage contract: a committed name is searchable before any header job.
        juce::String firstId, secondId;
        {
            sp::Catalog db(dbFile);
            auto r = db.addRoot(root);
            sp::FileStamp stamp;
            require(sp::fileStamp(root.getChildFile("A/soft mechanical click.wav"), stamp),
                    "Read source stamp");
            db.begin();
            db.discover(r, "A/soft mechanical click.wav", stamp, 1);
            db.commit();
            sp::Catalog read(dbFile, true);
            auto rows = read.search("soft mech");
            require(rows.size() == 1 && rows[0].state == "pending" && rows[0].channels == 0,
                    "Progressive searchable names precede metadata");
            firstId = rows[0].id;
            require(read.search(firstId).size() == 1, "Exact asset ID lookup");
        }
        {
            sp::LibraryService service(dbFile);
            until(
                [&]
                {
                    auto s = service.snapshot();
                    return s.results.size() == 6 && s.roots[0].ready == 5 && s.roots[0].failed == 1;
                },
                "Initial recursive import; WAV AIFF FLAC");
            auto request = service.search("soft mechanical");
            until(
                [&]
                {
                    auto s = service.snapshot();
                    return s.request == request && s.results.size() == 2;
                },
                "FTS search");
            auto s = service.snapshot();
            require(s.results[0].id != s.results[1].id, "Same filename, distinct stable IDs");
            for (auto &a : s.results)
            {
                if (a.relative.startsWith("A/"))
                    require(a.channels == 1 && a.sampleRate == 48000 && std::abs(a.duration - 0.25) < 0.001,
                            "Mono header");
                else
                {
                    secondId = a.id;
                    require(a.channels == 2 && a.sampleRate == 44100 && std::abs(a.duration - 0.5) < 0.001,
                            "Stereo header");
                }
            }
            for (int i = 0; i < 30; ++i)
                service.search("nothing " + juce::String(i));
            request = service.search(juce::String::fromUTF8("café"));
            until(
                [&]
                {
                    auto q = service.snapshot();
                    return q.request == request && q.results.size() == 1;
                },
                "Newest generation wins; Unicode search");
            // Native FSEvents discovers additions without a manual rescan.
            fixture(root.getChildFile("newly created.wav"));
            request = service.search("newly");
            until(
                [&]
                {
                    auto q = service.snapshot();
                    return q.request == request && q.results.size() == 1 && q.results[0].state == "ready";
                },
                "Live file event");
            require(root.getChildFile("A/soft mechanical click.wav")
                        .moveFileTo(root.getChildFile("A/renamed click.wav")),
                    "Rename fixture");
            service.rescan();
            request = service.search("renamed");
            until(
                [&]
                {
                    auto q = service.snapshot();
                    return q.request == request && q.results.size() == 1 && q.results[0].id == firstId;
                },
                "Confirmed rename retains ID");
            require(service.snapshot().results[0].revision == 1, "Rename retains revision");
            fixture(root.getChildFile("A/renamed click.wav"), 2, 96000, 0.2);
            service.rescan();
            until(
                [&]
                {
                    auto q = service.snapshot();
                    return q.results.size() == 1 && q.results[0].revision == 2 &&
                           q.results[0].sampleRate == 96000;
                },
                "Replacement invalidates metadata and advances revision");
            auto offline = base.getChildFile("disconnected");
            require(root.moveFileTo(offline), "Simulated disconnect");
            until(
                [&]
                {
                    auto q = service.snapshot();
                    return !q.roots.empty() && !q.roots[0].online && !q.results.empty() &&
                           !q.results[0].online;
                },
                "Offline records remain searchable");
            require(offline.moveFileTo(root), "Simulated reconnect");
            until(
                [&]
                {
                    auto q = service.snapshot();
                    return q.roots[0].online && q.results[0].online && q.results[0].id == firstId;
                },
                "Reconnect restores access and ID");
        }
        {
            sp::LibraryService reopened(dbFile);
            auto request = reopened.search("soft mechanical");
            until(
                [&]
                {
                    auto q = reopened.snapshot();
                    return q.request == request && q.results.size() == 1 && q.results[0].id == secondId &&
                           !q.roots[0].scanning;
                },
                "Persistent searchable catalog after restart");
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            require(reopened.snapshot().headersRead == 0, "Unchanged restart schedules zero header jobs");
        }
        {
            // A killed writer must leave the previously committed WAL state intact.
            pid_t child = fork();
            if (child == 0)
            {
                execl(argv[0], argv[0], "--crash", dbFile.getFullPathName().toRawUTF8(), "unused", nullptr);
                _exit(2);
            }
            int code;
            waitpid(child, &code, 0);
            require(WIFEXITED(code) && WEXITSTATUS(code) == 0, "Crash fixture exit");
            sp::Catalog db(dbFile, true);
            require(db.search("uncommitted").empty() && db.count() == 7,
                    "Transaction rollback after abrupt writer exit");
            auto start = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 200; ++i)
                require(db.search("soft mech").size() == 1, "Repeated FTS search");
            std::cout << "200 warm FTS queries, mean ms: "
                      << (juce::Time::getMillisecondCounterHiRes() - start) / 200 << "\n";
            for (auto q : {"\"", "NEAR(foo)", "' OR 1=1 --", "click*", "雨"})
                db.search(juce::String::fromUTF8(q));
        }
        {
            auto bulk = base.getChildFile("bulk");
            auto bulkDb = base.getChildFile("bulk.sqlite");
            auto original = root.getChildFile("B/soft mechanical click.wav");
            for (int i = 0; i < 2400; ++i)
            {
                auto f = bulk.getChildFile("nested" + juce::String(i / 100) + "/bulk click " +
                                           juce::String(i) + ".wav");
                f.getParentDirectory().createDirectory();
                require(original.copyFileTo(f), "Bulk fixture copy");
            }
            {
                sp::LibraryService service(bulkDb);
                service.pause(true);
                service.addFolder(bulk);
                until([&] { return service.ready() && service.snapshot().progress.paused; },
                      "Import starts at a controlled pause boundary");
                sp::Catalog committed(bulkDb, true);
                service.pause(false);
                until([&] { return committed.count() > 0; }, "First durable import batch");
                service.pause(true);
                until([&] { return service.snapshot().progress.paused; }, "Interrupt at a safe boundary");
                auto request = service.search("bulk click");
                until(
                    [&]
                    {
                        auto q = service.snapshot();
                        return q.request == request && !q.results.empty() && !q.roots.empty() &&
                               q.roots[0].scanning && q.roots[0].discovered < 2400;
                    },
                    "Search during active enumeration");
                std::cout << "Progressive import visible at " << service.snapshot().roots[0].discovered
                          << " / 2400 files\n";
                // Tear down a parked partial enumeration, preserving only committed batches.
            }
            {
                sp::Catalog read(bulkDb, true);
                require(read.count() > 0 && read.count() < 2400,
                        "Interrupted import retains committed records");
            }
            {
                sp::LibraryService resumed(bulkDb);
                until(
                    [&]
                    {
                        auto q = resumed.snapshot();
                        return !q.roots.empty() && q.roots[0].ready == 2400 && !q.roots[0].scanning;
                    },
                    "Interrupted import resumes without duplicate IDs", 30000);
                sp::Catalog read(bulkDb, true);
                require(read.count() == 2400, "Restart completes every asset once");
            }
        }
        for (int rate : {44100, 48000, 96000})
            for (int channels : {1, 2})
            {
                auto file = base.getChildFile("rate.wav");
                fixture(file, channels, rate, 0.1);
                sp::PcmStream stream(file, 48000);
                stream.fill();
                int frames = 0;
                float gain = 1, energy = 0;
                juce::AudioBuffer<float> output(2, 256);
                while (!stream.ended())
                {
                    output.clear();
                    auto reads = stream.decodedReads();
                    int n = stream.read(output.getArrayOfWritePointers(), 2, 256, gain, 1);
                    require(stream.decodedReads() == reads, "Callback performs no decoder I/O");
                    frames += n;
                    for (int i = 0; i < n; ++i)
                        energy += std::abs(output.getSample(0, i));
                    stream.fill();
                }
                require(frames == 4800, "Exact resampled frame count");
                require(energy > 100, "Non-silent generated playback");
            }
        {
            auto file = base.getChildFile("long audition.wav");
            fixture(file, 2, 48000, 8);
            sp::AudioClient audio(executable);
            until(
                [&]
                {
                    auto s = audio.status();
                    return (double)s["sampleRate"] > 0;
                },
                "Core Audio output initialized", 15000);
            audio.play("test-asset", file);
            until([&] { return (bool)audio.status()["playing"]; }, "Audition helper playback");
            auto initial = audio.status();
            int64_t frames = (juce::int64)initial["frames"], underruns = (juce::int64)initial["underruns"];
            bool heard = false;
            sp::LibraryService service(dbFile);
            for (int i = 0; i < 100; ++i)
            {
                service.search(i % 2 ? "soft" : "renamed");
                auto state = audio.status();
                heard |= (double)state["peak"] > 0.01;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            auto final = audio.status();
            require((bool) final["playing"] && (juce::int64) final["frames"] > frames + 20000,
                    "Core Audio continues while searching");
            require(heard, "Core Audio callback receives non-silent PCM");
            require((juce::int64) final["underruns"] == underruns,
                    "No stream starvation during concurrent search");
            std::cout << "Core Audio device: " << final["device"].toString()
                      << ", sample rate: " << final["sampleRate"].toString()
                      << ", callback frames: " << final["frames"].toString() << "\n";
            audio.stop();
            until([&] { return !(bool)audio.status()["playing"]; }, "Stop command");
            auto generation = (juce::int64)audio.status()["processGeneration"];
            audio.restart();
            until(
                [&]
                {
                    auto state = audio.status();
                    return (double)state["sampleRate"] > 0 &&
                           (juce::int64)state["processGeneration"] > generation;
                },
                "Audio helper recovery");
        }
        std::cout << "PASS: " << checks << " checks. Original fixtures retained at " << root.getFullPathName()
                  << "\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
