#include "ReliabilityTests.h"
#include "LibraryView.h"
#include "DesignModel.h"
#include "Performance.h"
#include "LibraryLayout.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <set>
namespace
{
int checks = 0;
void check(bool value, const char *why)
{
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
template <class F> void until(F predicate, const char *why, int ms = 10000)
{
    auto end = sp::monotonicMs() + ms;
    while (!predicate() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(predicate(), why);
}
sp::Asset fixture(const juce::File &p, int samples = 48000)
{
    p.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    auto out = p.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(out.release(), 48000, 1, 16, {}, 0));
    juce::AudioBuffer<float> b(1, 8192);
    for (int at = 0; at < samples; at += 8192)
    {
        int n = std::min(8192, samples - at);
        for (int i = 0; i < n; ++i)
            b.setSample(0, i, .1f * std::sin((at + i) * .05));
        check(w->writeFromAudioSampleBuffer(b, 0, n), "Fixture finalized");
    }
    w.reset();
    return sp::inspectDesignSource(p);
}
} // namespace
namespace sp
{
struct LibraryReliabilityTestAccess
{
    static void run(const juce::File &root, const juce::File &app)
    {
        auto source = fixture(root.getChildFile("seed.wav"), 48000 * 10);
        auto folder = root.getChildFile("sounds");
        folder.createDirectory();
        for (int i = 0; i < 40; ++i)
            check(juce::File(source.path)
                      .copyFileTo(
                          folder.getChildFile("z-sound-" + juce::String(i).paddedLeft('0', 3) + ".wav")),
                  "Create actual sources");
        LibraryView v(root.getChildFile("catalog/library.sqlite"), app, false);
        v.stopTimer();
        v.setSize(1040, 648);
        v.library.addFolder(folder);
        until([&] { return v.library.snapshot().headersRead >= 40; }, "Index real initial files");
        v.benchmarkSearch("");
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == v.requested && v.snapshot.results.size() == 20;
            },
            "Initial 20-row prefix");
        v.table.selectRow(5);
        v.selectedRowsChanged(5);
        check((bool)v.library.queryDiagnostics()["pinned"],
              "Selecting a loaded row pins its current service query");
        auto id = v.selectedId;
        v.setRegion(1500, 240000, false);
        auto first = v.regionStart, last = v.regionEnd;
        v.playSelected(true);
        auto generation = v.audio.status()["intentGeneration"];
        for (int i = 0; i < 30; ++i)
            check(juce::File(source.path)
                      .copyFileTo(folder.getChildFile("a-earlier-" + juce::String(i) + ".wav")),
                  "Earlier indexing matches");
        v.library.rescan();
        until(
            [&]
            {
                v.timerCallback();
                return v.library.snapshot().headersRead >= 70;
            },
            "Concurrent indexing completes");
        check(v.selectedId == id && v.regionStart == first && v.regionEnd == last,
              "Background indexing preserves stable selection and range");
        check(v.audio.status()["intentId"].toString() == id &&
                  v.audio.status()["intentGeneration"] == generation,
              "Background indexing does not retarget pending/playing audition");
        until(
            [&]
            {
                v.timerCallback();
                return (bool)v.audio.status()["playing"];
            },
            "Pending audition is adopted by native helper");
        check(v.audio.status()["playGeneration"] == generation,
              "Native adopted generation matches unchanged play intent");
        // A controlled reordered/shorter publication must never reuse the old
        // row index when its stable ID is outside the loaded page.
        auto published = v.snapshot;
        v.snapshot.results.erase(v.snapshot.results.begin() + 5);
        v.updating = true;
        auto restored = v.restoreSelection(true);
        v.updating = false;
        check(restored == -1 && v.table.getSelectedRow() == -1 && v.selectedId == id &&
                  v.selected()->id == id,
              "Missing selected ID retains its cached identity, never the replacement row index");
        check(v.audio.status()["intentId"].toString() == id &&
                  v.audio.status()["intentGeneration"] == generation,
              "Controlled reordered prefix cannot switch audition");
        v.snapshot = published;
        auto oldRow = 5;
        v.benchmarkSearch("no-such-recording");
        v.selectedRowsChanged(oldRow);
        check(!(bool)v.library.queryDiagnostics()["pinned"],
              "Old displayed selection cannot pin a newer query");
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == v.requested;
            },
            "Empty new query applies");
        check(v.selectedId.isEmpty() && v.table.getSelectedRow() == -1 &&
                  v.audio.status()["intentId"].toString() == id,
              "New query clears absent UI selection without taking over or stopping audition");
        v.refresh.onClick();
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == v.requested;
            },
            "Explicit Refresh Results applies");
        v.benchmarkSearch("");
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == v.requested;
            },
            "Fresh results apply");
        v.table.selectRow(2);
        v.selectedRowsChanged(2);
        auto next = v.selectedId;
        check(next != id && v.audio.status()["intentId"].toString() == next &&
                  (int64_t)v.audio.status()["intentGeneration"] == (int64_t)generation + 1,
              "Intentional loaded selection after searching follows active/pending Library audition once");
        v.audio.stop();
        v.table.selectRow(3);
        v.selectedRowsChanged(3);
        check(v.audio.status()["intentId"].toString().isEmpty(), "Stopped selection remains silent");
        check((bool)v.library.queryDiagnostics()["pinned"], "Stopped loaded-row selection also pins");
        v.benchmarkScroll(60);
        until(
            [&]
            {
                v.timerCallback();
                return v.rowAsset(60) != nullptr;
            },
            "Missing-page fetch works independently of selection pin");
        // Actual catalog identity removal, leaving its original source file intact.
        v.table.selectRow(3);
        v.selectedRowsChanged(3);
        auto removed = v.selectedId;
        v.playSelected(true);
        {
            Catalog db(root.getChildFile("catalog/library.sqlite"));
            db.exec(("DELETE FROM assets WHERE id='" + removed + "'").toRawUTF8());
        }
        v.library.rescan();
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.selectedAssetRemoved;
            },
            "Service explicitly reports removed selected identity");
        check(v.selectedId.isEmpty() && v.audio.status()["intentId"].toString().isEmpty(),
              "Removed catalog identity clears selection and stops only its own audition");
        auto *record = new juce::DynamicObject();
        record->setProperty("checks", checks);
        record->setProperty("catalog", root.getFullPathName());
        record->setProperty("actualIndexing", true);
        record->setProperty("controlledReorderedPrefix", true);
        record->setProperty("pendingPlayingStopped", true);
        root.getParentDirectory()
            .getChildFile("latest.json")
            .replaceWithText(juce::JSON::toString(juce::var(record)));
    }
};
} // namespace sp
void runLibraryStabilityTests(const juce::File &parent, const juce::File &app)
{
    check(sp::visibleLibraryRows({0, 0, 250, 522}, 58, 10000) == juce::Range<int>(0, 9),
          "An exact-height viewport has nine rows, not an offscreen tenth");
    check(sp::visibleLibraryRows({0, 1, 250, 522}, 58, 10000) == juce::Range<int>(0, 10),
          "Partially visible first/last rows are counted");
    check(sp::visibleLibraryRows({0, 580, 250, 522}, 58, 15) == juce::Range<int>(10, 15),
          "Viewport range is bounded by actual result count");
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    sp::LibraryReliabilityTestAccess::run(root, app);
    std::cout << "PASS Library stability: " << checks << " checks\n";
}
void runWaveformSchedulingTests(const juce::File &parent)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto longFile = fixture(root.getChildFile("long.wav"), 48000 * 20),
         shortFile = fixture(root.getChildFile("short.wav")), other = fixture(root.getChildFile("other.wav"));
    struct Gate
    {
        std::atomic<bool> entered = false, cancelled = false, release = false;
    };
    auto makePool = [&](const char *name, std::shared_ptr<Gate> gate)
    {
        return std::make_shared<sp::WaveformPool>(
            root.getChildFile(name), std::function<void()>{}, std::function<double()>{},
            [gate, id = longFile.id](const sp::Asset &a, int64_t at, const std::function<bool()> &stop)
            {
                if (a.id == id && at == 65536 && !gate->entered.exchange(true))
                {
                    auto end = sp::monotonicMs() + 5000;
                    while (!stop() && !gate->release && sp::monotonicMs() < end)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    gate->cancelled = stop();
                }
            });
    };
    auto abandoned = std::make_shared<Gate>();
    auto pool = makePool("abandoned", abandoned);
    pool->demand(2, {longFile});
    pool->tick();
    until([&] { return abandoned->entered.load(); }, "Long source held between blocks");
    pool->demand(2, {});
    pool->demand(0, {shortFile});
    pool->tick();
    pool->demand(0, {other});
    pool->tick();
    until(
        [&]
        {
            pool->tick();
            return abandoned->cancelled && pool->get(other) != nullptr;
        },
        "Abandoned decode cancels and rapid final selection advances without release", 1000);
    check(!pool->get(longFile), "Obsolete late result is not adopted");
    auto useful = std::make_shared<Gate>();
    auto priority = makePool("priority", useful);
    priority->demand(2, {longFile});
    priority->tick();
    until([&] { return useful->entered.load(); }, "Useful background decode held");
    for (int i = 0; i < 30; ++i)
    {
        priority->demand(2, {longFile});
        priority->tick();
    }
    check((int)priority->metrics()["cancellations"] == 0 && !useful->cancelled,
          "Repeated identical demands do not churn useful work");
    priority->demand(0, {shortFile});
    priority->tick();
    until(
        [&]
        {
            priority->tick();
            return useful->cancelled && priority->get(shortFile) != nullptr;
        },
        "Selected source preempts lower-priority still-demanded work", 1000);
    until(
        [&]
        {
            priority->tick();
            return priority->get(longFile) != nullptr;
        },
        "Still-demanded background source eventually completes");
    check((int)priority->metrics()["priorityPreemptions"] == 1,
          "One intentional priority preemption recorded");
    // Foreground demand continues changing: a reserved background admission must
    // still complete. Use a separate blocked worker, not a wait-and-release test.
    auto busy = std::make_shared<Gate>();
    auto fair = makePool("fair", busy);
    fair->demand(2, {longFile});
    fair->tick();
    until([&] { return busy->entered.load(); }, "Fairness source held");
    int serial = 0;
    until(
        [&]
        {
            auto a = shortFile;
            a.id = "foreground-" + juce::String(serial++);
            fair->demand(0, {a});
            fair->tick();
            return fair->get(longFile) != nullptr;
        },
        "Background completes despite continuous foreground replacement");
    check((int)fair->metrics()["fairAdmissions"] > 0, "Fair background reservation is exercised");
    auto pressure = std::make_shared<sp::WaveformPool>(root.getChildFile("pressure"));
    std::vector<sp::Asset> rows, layers;
    for (int i = 0; i < 80; ++i)
    {
        auto a = shortFile;
        a.id = "simultaneous-" + juce::String(i);
        (i < 40 ? rows : layers).push_back(a);
    }
    pressure->demand(0, {other});
    pressure->demand(1, {shortFile});
    pressure->demand(2, rows);
    pressure->demand(3, layers);
    std::set<juce::String> completed;
    until(
        [&]
        {
            pressure->tick();
            for (auto &a : rows)
                if (pressure->get(a))
                    completed.insert(a.id);
            for (auto &a : layers)
                if (pressure->get(a))
                    completed.insert(a.id);
            return completed.size() == 80;
        },
        "All 80 simultaneous visible sources make progress beyond the 48-source retention boundary", 15000);
    check((int)pressure->metrics()["retainedSources"] <= 48 &&
              (int)pressure->metrics()["retainedGeometries"] <= 96,
          "Cache retention remains bounded under simultaneous-surface pressure");
    auto record = pressure->metrics();
    record.getDynamicObject()->setProperty("checks", checks);
    record.getDynamicObject()->setProperty("completedBeyondBoundary", (int)completed.size());
    record.getDynamicObject()->setProperty("abandoned", pool->metrics());
    record.getDynamicObject()->setProperty("priority", priority->metrics());
    record.getDynamicObject()->setProperty("continuousForeground", fair->metrics());
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(record));
    std::cout << "PASS cooperative waveform cancellation, priority and bounded fairness: " << checks
              << " checks\n";
}
void prepareReliabilityWorkload(const juce::File &folder)
{
    check(!folder.exists(), "Use a fresh reliability design directory");
    folder.createDirectory();
    auto source = fixture(folder.getChildFile("forty-minute-source.wav"), 48000 * 2400);
    sp::DesignDocument d;
    d.name = "Sustained Performance QA";
    d.original.masterDb = -6;
    d.renderSettings.tailSeconds = 1;
    for (int i = 0; i < 8; ++i)
    {
        auto l = sp::makeDesignLayer(source, 0, -1, true);
        l.gainDb = -24;
        l.rate = i % 2 ? .875 : 1;
        l.reverse = i % 3 == 1;
        l.fadeIn = 480;
        l.fadeOut = 960;
        l.effects = {sp::makeStockEffect("filter"), sp::makeStockEffect("delay"),
                     sp::makeStockEffect("reverb")};
        d.original.layers.push_back(l);
    }
    d.original.masterEffects = {sp::makeStockEffect("filter"), sp::makeStockEffect("reverb")};
    d.selectedLayer = d.original.layers[0].id;
    sp::saveDesignAtomic(folder.getChildFile("workload.soundplus"), d);
}
