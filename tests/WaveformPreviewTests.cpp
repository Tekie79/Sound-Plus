#include "WaveformPreviewTests.h"
#include "WaveformPreview.h"
#include "SourceCard.h"
#include "PerformanceState.h"
#include "CanvasEditing.h"
#include "LibraryView.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <thread>
#include <condition_variable>
namespace
{
int checks = 0;
void check(bool ok, const char *why)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
template <class F> void wait(F ready, const char *why)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
    while (!ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(ready(), why);
}
sp::Asset fixture(const juce::File &file, int channels, int samples, double rate,
                  const std::function<float(int, int)> &sample)
{
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), rate, channels, 24, {}, 0));
    juce::AudioBuffer<float> block(channels, 8192);
    for (int at = 0; at < samples; at += block.getNumSamples())
    {
        const int n = std::min(block.getNumSamples(), samples - at);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                block.setSample(c, i, sample(c, at + i));
        check(writer && writer->writeFromAudioSampleBuffer(block, 0, n),
              "Finalize generated waveform fixture");
    }
    writer.reset();
    return sp::inspectDesignSource(file);
}
float peak(const sp::PeakColumns &columns)
{
    float result = 0;
    for (auto [lo, hi] : columns)
        result = std::max({result, std::abs(lo), std::abs(hi)});
    return result;
}
} // namespace
namespace sp
{
struct LibraryPreviewTestAccess
{
    static void checkPaging(const juce::File &root, const Asset &asset)
    {
        LibraryView view(root.getChildFile("paging/library.sqlite"), {}, false);
        view.stopTimer();
        view.requested = 42;
        view.snapshot.request = 41;
        view.snapshot.total = 73;
        view.snapshot.results.assign(20, asset);
        view.snapshot.pageSerial = 1;
        view.requestPageForRow(60);
        check(view.requestedPage == -1, "Old visible rows cannot pin a pending query");
        view.snapshot.request = 42;
        view.snapshot.pageSerial = 2;
        view.requestPageForRow(60);
        check(view.requestedPage == 0 && view.requestedPageSerial == 2,
              "Missing visible rows request their page");
        // The fetched page can be superseded by a 20-row semantic/refresh prefix
        // of the very same query. Its missing viewport must be fetchable again.
        view.snapshot.results.assign(73, asset);
        view.snapshot.pageSerial = 3;
        view.requestPageForRow(60);
        view.snapshot.results.resize(20);
        view.snapshot.pageSerial = 4;
        view.requestPageForRow(60);
        check(view.requestedPageSerial == 4,
              "A shorter replacement result page re-fetches missing visible rows");
    }
};
} // namespace sp
void runWaveformPreviewTests(const juce::File &parent)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto silence = fixture(root.getChildFile("silence.wav"), 1, 960, 48000, [](int, int) { return 0.f; });
    auto stereo = fixture(root.getChildFile("opposite-polarity-transients.wav"), 2, 48000, 48000,
                          [](int c, int n) {
                              return n == 100 ? (c ? -.9f : .9f) : n == 47000 ? (c ? -.35f : .35f) : 0.f;
                          });
    sp::LibraryPreviewTestAccess::checkPaging(root, stereo);
    auto multi = fixture(root.getChildFile("four-channel.wav"), 4, 48000, 48000,
                         [](int c, int n) { return c == 3 && n == 12345 ? .8f : 0.f; });
    auto tiny = fixture(root.getChildFile("one-sample.wav"), 1, 1, 48000, [](int, int) { return .7f; });
    auto longFile = fixture(root.getChildFile("hour-overview.wav"), 1, 8000 * 3600, 8000,
                            [](int, int n) { return n == 8000 * 1800 + 19 ? .95f : 0.f; });
    double now = 0;
    std::atomic<bool> wrongThread = false;
    const auto ui = std::this_thread::get_id();
    auto pool = std::make_shared<sp::WaveformPool>(
        root.getChildFile("peaks"),
        [&]
        {
            if (std::this_thread::get_id() == ui)
                wrongThread = true;
        },
        [&] { return now; });
    pool->demand(1, {stereo, silence, stereo, multi, tiny, longFile});
    pool->demand(2, {multi, stereo, silence});
    wait(
        [&]
        {
            pool->tick();
            return pool->get(longFile) != nullptr;
        },
        "Serial deduplicated waveform demand finishes");
    check((int64_t)pool->metrics()["decodes"] == 5,
          "Five sources across repeated cards/rows decode exactly once");
    check(!wrongThread, "Decoder callbacks run outside the UI thread");
    auto a = pool->columns(stereo, 0, 24000, false, 17), b = pool->columns(stereo, 24000, 48000, false, 17);
    check(a && b && peak(*a) > .89f && peak(*b) > .34f && peak(*b) < .36f,
          "Different source ranges retain their own isolated transients, including opposite phase channels");
    auto reversed = pool->columns(stereo, 0, 24000, true, 17);
    auto flipped = *a;
    std::reverse(flipped.begin(), flipped.end());
    check(*reversed == flipped, "Reverse mirrors the selected range without decoding again");
    check(peak(*pool->columns(multi, 0, 48000, false, 9)) > .79f,
          "All-channel extrema include an isolated fourth-channel transient");
    check(peak(*pool->columns(silence, 0, 960, false, 100)) == 0,
          "Genuine silent audio is a real flat waveform");
    check(peak(*pool->columns(tiny, 0, 1, false, 100)) > .69f,
          "One-sample source has truthful nonempty geometry");
    check(pool->get(longFile)->peaks.size() == 65536 &&
              peak(*pool->columns(longFile, 0, longFile.samples, false, 61)) > .94f,
          "Long files stay capped at 65536 bins and reduction retains short transients");
    sp::WaveformData edge;
    edge.samples = 1024;
    edge.peaks = {{0, .9f}, {0, 0}, {0, .6f}, {0, 0}};
    check(peak(sp::compactWaveformColumns(edge, 256, 512, false, 20)) == 0,
          "Exact trimmed-bin boundaries exclude the preceding transient bin");
    check(peak(sp::compactWaveformColumns(edge, 255, 512, false, 20)) > .89f,
          "Partial edge bins are conservatively included at cache resolution");
    check(peak(sp::compactWaveformColumns(edge, 0, 1024, false, 1)) > .89f,
          "Every bin covered by a pixel contributes, not just one sampled bin");
    juce::Label title;
    juce::ComboBox chooser;
    sp::SourceCard card(title, chooser, pool);
    card.setSize(210, 70);
    auto layer = sp::makeDesignLayer(stereo, 0, 24000, true);
    layer.rate = 2;
    layer.placement = 12000;
    card.bind(&layer, layer.id, "Attack", true, .5);
    check(title.getText() == "Attack" && card.getTitle().contains("0.25 seconds") && card.displayedColumns(),
          "Card preserves custom alias and linked-rate duration with selected-range geometry");
    const auto before = pool->metrics();
    juce::Image image(juce::Image::ARGB, 210, 70, true);
    juce::Graphics g(image);
    for (int i = 0; i < 100; ++i)
    {
        card.paint(g);
        card.bind(&layer, layer.id, "Attack", false, i / 100.);
    }
    check((int64_t)pool->metrics()["decodes"] == (int64_t)before["decodes"] &&
              (int64_t)pool->metrics()["requests"] == (int64_t)before["requests"],
          "Painting, selection and contribution changes do not request or decode sources");
    card.transport(true, 12000, 48000);
    check(card.cursor() && *card.cursor() == 0, "Passive source head begins at placement");
    card.transport(true, 18000, 48000);
    check(card.cursor() && std::abs(*card.cursor() - .5) < 1e-4,
          "Head follows selected range and linked rate");
    card.transport(true, 24000, 48000);
    check(!card.cursor(), "Source head hides beyond source interval, including effect tails");
    card.transport(false, 18000, 48000);
    check(!card.cursor(), "Library-owned playback does not create a Designer card head");
    layer.reverse = true;
    card.bind(&layer, layer.id, "Attack", false, .5);
    card.transport(true, 12000, 48000);
    check(card.cursor() && *card.cursor() == 0,
          "Reverse source head agrees with mirrored overview at its start");
    bool edited = false;
    card.onEdit = [&](auto &id) { edited = id == layer.id; };
    card.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    check(edited, "Keyboard route opens the stable assigned layer in Edit");
    card.bind(nullptr, {}, "", false, 0);
    check(card.stateText() == "Drop sound" && !card.displayedColumns(),
          "Clear removes the old waveform immediately");
    card.bind(nullptr, "missing-id", "", false, 0);
    check(card.stateText() == "Missing layer", "Missing layer differs from unassigned");
    auto oneSample = sp::makeDesignLayer(tiny, 0, -1, true);
    card.bind(&oneSample, oneSample.id, "", false, 0);
    check(card.getTitle().contains("0.02 milliseconds"),
          "A one-sample source has a nonzero duration in visible/accessibility units");
    auto quiet = sp::makeDesignLayer(silence, 0, -1, true);
    card.bind(&quiet, quiet.id, "", false, 0);
    check(card.stateText() == "Silent source" && card.displayedColumns() &&
              card.getDescription().contains("Silent source"),
          "Silence differs from loading and missing");
    // Existing cache survives disconnect as an offline state, then revalidates on reconnect.
    const auto moved = root.getChildFile("disconnected.wav");
    check(juce::File(stereo.path).moveFileTo(moved), "Disconnect only the generated fixture");
    now = 6000;
    wait(
        [&]
        {
            pool->tick();
            auto d = pool->get(stereo);
            return d && d->status == sp::WaveformData::Status::offline;
        },
        "Background stamp validation detects an offline cached source");
    card.bind(&layer, layer.id, "Attack", false, 0);
    check(card.stateText() == "Offline source" && !card.displayedColumns(),
          "Offline card never retains an old ready image");
    check(moved.moveFileTo(juce::File(stereo.path)), "Reconnect the original fixture unchanged");
    now = 12000;
    wait(
        [&]
        {
            pool->tick();
            auto d = pool->get(stereo);
            return d && d->status == sp::WaveformData::Status::ready;
        },
        "Reconnect restores source from verified disk peaks");
    layer.source.error = "Offline or missing; relink source";
    card.bind(&layer, layer.id, "Attack", false, 0);
    check(card.stateText() == "Ready" && card.displayedColumns(),
          "Reconnection supersedes a file-offline observation retained when opening the document");
    layer.source.error = "Source revision changed; accept a replacement";
    card.bind(&layer, layer.id, "Attack", false, 0);
    check(!card.displayedColumns(), "Verified peaks cannot bypass a revision/replacement conflict");
    layer.source.error.clear();
    auto relink = stereo;
    relink.path = root.getChildFile("relinked.wav").getFullPathName();
    check(juce::File(stereo.path).copyFileTo(juce::File(relink.path)), "Create a relinked fixture copy");
    sp::FileStamp stamp;
    sp::fileStamp(juce::File(relink.path), stamp);
    relink.size = stamp.size;
    relink.modified = stamp.modified;
    relink.inode = stamp.inode;
    pool->demand(1, {relink});
    pool->demand(2, {});
    auto relinkLayer = layer;
    relinkLayer.source = relink;
    card.bind(&relinkLayer, relinkLayer.id, "Relink", false, 0);
    check(card.stateText() == "Loading" && !card.displayedColumns(),
          "Relink clears the old binding before asynchronous data arrives");
    wait(
        [&]
        {
            pool->tick();
            return pool->get(relink) != nullptr;
        },
        "Relink source gets its own validated binding");
    card.refreshPeaks();
    check(card.displayedColumns() != nullptr, "Relink shows only matching immutable peaks");
    auto corrupt = stereo;
    corrupt.id = "corrupt";
    corrupt.path = root.getChildFile("corrupt.wav").getFullPathName();
    juce::File(corrupt.path).replaceWithText("not audio");
    sp::fileStamp(juce::File(corrupt.path), stamp);
    corrupt.size = stamp.size;
    corrupt.modified = stamp.modified;
    corrupt.inode = stamp.inode;
    pool->demand(1, {corrupt});
    wait(
        [&]
        {
            pool->tick();
            return pool->get(corrupt) != nullptr;
        },
        "Decoder failure completes without hanging the scheduler");
    auto badLayer = layer;
    badLayer.source = corrupt;
    card.bind(&badLayer, badLayer.id, "Bad", false, 0);
    check(card.stateText() == "Decode error" && !card.displayedColumns(),
          "Decode failure is distinct from offline and silence");
    // Recycled bindings cannot display a late old result while the sole decoder is blocked.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        std::atomic<bool> entered = false;
    };
    auto gate = std::make_shared<Gate>();
    auto blocked = std::make_shared<sp::WaveformPool>(root.getChildFile("blocked"),
                                                      [gate]
                                                      {
                                                          gate->entered = true;
                                                          std::unique_lock lock(gate->mutex);
                                                          gate->cv.wait_for(lock, std::chrono::seconds(15),
                                                                            [&] { return gate->release; });
                                                      });
    blocked->demand(1, {multi});
    blocked->tick();
    wait([&] { return gate->entered.load(); }, "Controlled decoder starts on its worker");
    juce::Label t;
    juce::ComboBox c;
    sp::SourceCard recycled(t, c, blocked);
    recycled.setSize(180, 70);
    auto oldLayer = sp::makeDesignLayer(multi, 0, -1, true);
    recycled.bind(&oldLayer, oldLayer.id, "Old", false, 0);
    recycled.bind(&quiet, quiet.id, "New", false, 0);
    blocked->demand(1, {silence});
    check(!recycled.displayedColumns() && recycled.layerId() == quiet.id,
          "Rapid row/card reuse immediately clears stale geometry");
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    wait(
        [&]
        {
            blocked->tick();
            recycled.refreshPeaks();
            return recycled.stateText() == "Silent source";
        },
        "Late old completion cannot paint under the new binding");
    // Simulated catalog demand is bounded to the visible slice, not all files.
    const auto startRequests = (int64_t)pool->metrics()["requests"];
    std::vector<sp::Asset> visible;
    for (int i = 0; i < 5; ++i)
    {
        auto a = silence;
        a.id = "visible-" + juce::String(i);
        visible.push_back(a);
    }
    pool->demand(1, {});
    pool->demand(2, visible);
    wait(
        [&]
        {
            pool->tick();
            return pool->get(visible.back()) != nullptr;
        },
        "Visible five-source window completes serially");
    check((int64_t)pool->metrics()["requests"] - startRequests == 5,
          "Visible slice schedules five jobs, not the conceptual catalog");
    for (int i = 0; i < 55; ++i)
    {
        auto a = silence;
        a.id = "bounded-" + juce::String(i);
        pool->demand(2, {a});
        wait(
            [&]
            {
                pool->tick();
                return pool->get(a) != nullptr;
            },
            "Bounded cache turnover completes");
        pool->columns(a, 0, a.samples, false, 130 + i);
    }
    auto geometryAsset = silence;
    geometryAsset.id = "bounded-54";
    for (int width = 50; width < 170; ++width)
        pool->columns(geometryAsset, 0, silence.samples, false, width);
    check((int)pool->metrics()["retainedSources"] <= 48 && (int)pool->metrics()["retainedGeometries"] == 96 &&
              (int64_t)pool->metrics()["retainedPeakBytes"] <= 48 * 65536 * 8,
          "Retained peak sources and geometry memory remain bounded while scrolling");
    // A checksummed document for repeatable native Loading/Silence/Offline/Error
    // inspection. All media is owned by this isolated generated-fixture directory.
    auto offlineFile = root.getChildFile("offline-source.wav");
    check(juce::File(stereo.path).copyFileTo(offlineFile), "Create owned native offline fixture");
    auto offline = sp::inspectDesignSource(offlineFile);
    auto longNameFile =
        root.getChildFile("Recording-" + juce::String::repeatedString("long-texture-name-", 8) + ".wav");
    check(juce::File(stereo.path).copyFileTo(longNameFile), "Create owned long-filename fixture");
    auto named = sp::inspectDesignSource(longNameFile);
    sp::DesignDocument qa;
    qa.name = "Waveform source states QA";
    qa.original.layers = {sp::makeDesignLayer(silence, 0, -1, true), sp::makeDesignLayer(named, 0, -1, true),
                          sp::makeDesignLayer(offline, 0, -1, true),
                          sp::makeDesignLayer(corrupt, 0, -1, true)};
    for (int i = 0; i < 4; ++i)
        sp::assignBlendLayer(qa.original, i, qa.original.layers[(size_t)i].id);
    qa.selectedLayer = qa.original.layers.front().id;
    sp::DesignVariation ranges;
    ranges.name = "Ranges and reverse";
    ranges.mix.layers = {sp::makeDesignLayer(stereo, 0, 24000, true),
                         sp::makeDesignLayer(stereo, 24000, 48000, true),
                         sp::makeDesignLayer(multi, 0, -1, true), sp::makeDesignLayer(tiny, 0, -1, true)};
    ranges.mix.layers[0].name = "Attack";
    ranges.mix.layers[1].name = "Reverse tail";
    ranges.mix.layers[1].reverse = true;
    ranges.mix.layers[1].rate = 2;
    for (int i = 0; i < 4; ++i)
        sp::assignBlendLayer(ranges.mix, i, ranges.mix.layers[(size_t)i].id);
    qa.variations.push_back(ranges);
    sp::DesignVariation missing;
    missing.name = "Missing layer";
    missing.mix = qa.original;
    missing.mix.performance.assignments[0] = {"missing-stable-id", "Missing role"};
    qa.variations.push_back(missing);
    auto nativeDocument = root.getChildFile("native-source-cards.soundplus");
    sp::saveDesignAtomic(nativeDocument, qa);
    check(offlineFile.moveFileTo(root.getChildFile("offline-source.disconnected.wav")),
          "Native offline fixture is disconnected without touching user media");
    auto record = juce::var(new juce::DynamicObject());
    record.getDynamicObject()->setProperty("checks", checks);
    record.getDynamicObject()->setProperty("waveforms", pool->metrics());
    record.getDynamicObject()->setProperty("fixture", root.getFullPathName());
    record.getDynamicObject()->setProperty("nativeDocument", nativeDocument.getFullPathName());
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(record, true));
    std::cout << "PASS truthful compact waveform ranges, extrema, identity, worker scheduling and cards: "
              << checks << " checks\n";
}
