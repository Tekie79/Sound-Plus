#include "LibraryView.h"
#include "AudioStream.h"
#include "DesignModel.h"
#include "Performance.h"
#include <iostream>
#include <limits>
#include <set>
namespace
{
int checks = 0;
void check(bool ok, const char *why)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
template <class F> void until(F predicate, const char *why, int ms = 12000)
{
    auto end = sp::monotonicMs() + ms;
    while (!predicate() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(predicate(), why);
}
void fixture(const juce::File &file, int samples = 5760)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat format;
    juce::StringPairArray tags;
    tags.set(juce::WavAudioFormat::riffInfoGenre, "Field recordings");
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(file.createOutputStream().release(), 48000, 1, 24, tags, 0));
    juce::AudioBuffer<float> block(1, samples);
    for (int n = 0; n < samples; ++n)
        block.setSample(0, n, .1f * std::sin(n * .031));
    check(writer && writer->writeFromAudioSampleBuffer(block, 0, samples), "Finalized generated fixture");
    writer.reset();
    auto stream = file.createOutputStream();
    const juce::String xml = "<BWFXML><USER><Microphone>Test microphone</Microphone></USER></BWFXML>";
    const auto length = xml.getNumBytesAsUTF8();
    stream->write("iXML", 4);
    stream->writeInt((int)length);
    stream->write(xml.toRawUTF8(), length);
    if (length & 1)
        stream->writeByte(0);
    auto riffSize = (int)(stream->getPosition() - 8);
    stream->setPosition(4);
    stream->writeInt(riffSize);
}
void pcm(const juce::File &file)
{
    // Compare every consumed frame, including non-integral resampling, wraps
    // and a seek into a marked range. Producer read-ahead must not move telemetry.
    for (double rate : {48000., 44100.})
    {
        constexpr int64_t first = 137, last = 4198, seek = 347;
        sp::PcmStream stream(file, rate, first, last, 0, {}, true, seek);
        auto ratio = 48000. / rate;
        auto length = (uint64_t)std::ceil((last - first) / ratio);
        auto offset = (uint64_t)((seek - first) / ratio);
        uint64_t consumed = 0;
        float gain = 1;
        juce::AudioBuffer<float> out(2, 511);
        auto reads = stream.decodedReads();
        for (int pass = 0; pass < 50; ++pass)
        {
            stream.fill(8192);
            auto before = stream.decodedReads();
            check(stream.read(out.getArrayOfWritePointers(), 2, 511, gain, 1) == 511,
                  "Loop fills across boundaries without an EOF gap");
            check(before == stream.decodedReads(), "Audio read does not perform decoder I/O");
            for (int n = 0; n < 511; ++n)
            {
                double pos = first + ((consumed + n + offset) % length) * ratio;
                auto i = (int64_t)pos;
                float a = .1f * std::sin(i * .031), b = .1f * std::sin((i + 1 < last ? i + 1 : i) * .031);
                check(std::abs(out.getSample(0, n) - (a + (b - a) * (float)(pos - i))) < .000001,
                      "Loop PCM equals expected original range at every frame");
            }
            consumed += 511;
            check(stream.sourceSample() ==
                      std::min<int64_t>(last - 1, first + (int64_t)(((consumed + offset) % length) * ratio)),
                  "Loop cursor follows consumed frames, not queued producer frames");
            check(stream.loopCount() == (consumed + offset) / length && !stream.ended(),
                  "Loop telemetry counts consumed crossings and never completes naturally");
        }
        check(stream.decodedReads() > reads, "Decoding occurs in producer fill");
    }
    sp::AuditionCore core;
    auto stream = std::make_unique<sp::PcmStream>(file, 48000, 0, 256);
    stream->fill(512);
    core.adopt(std::move(stream));
    check(!core.completed(), "Prepared non-looping stream has not completed");
    juce::AudioBuffer<float> out(2, 256);
    core.render(out.getArrayOfWritePointers(), 2, 256);
    check(core.completed() && !core.playing(), "Natural EOF is distinguished from Stop");
    core.stop();
    check(!core.completed(), "Manual Stop cannot advertise natural completion");
}
void waveform()
{
    sp::WaveformView view;
    view.setSize(500, 200);
    auto data = std::make_shared<sp::WaveformData>();
    data->samples = 48000;
    data->rate = 48000;
    data->assetId = "silence";
    data->peaks.assign(500, {0, 0});
    view.setData(data);
    view.setRange(0, 48000, false, false);
    juce::Image whole(juce::Image::ARGB, 500, 200, true);
    juce::Graphics wholePainter(whole);
    view.paint(wholePainter);
    check(!view.isRangeSelected() && whole.getPixelAt(250, 60) == sp::ui::surface,
          "Whole-recording bounds do not imply an end-to-end visual selection");
    view.setRange(12000, 36000);
    juce::Image image(juce::Image::ARGB, 500, 200, true);
    juce::Graphics g(image);
    view.paint(g);
    auto background = sp::ui::surface;
    auto highlight = image.getPixelAt(250, 60);
    check(highlight != background, "Marked waveform range retains its visible highlight");
    check(image.getPixelAt(50, 60) == background, "Unselected waveform remains unshaded");
    for (int y = 34; y < 82; ++y)
        for (int x = 128; x < 371; ++x)
            check(image.getPixelAt(x, y) == highlight,
                  "Marked range keeps uniform highlight without drawn bracket glyphs");
    for (int scale : {1, 2})
    {
        auto render = [&](int64_t cursor)
        {
            view.setCursor(cursor);
            juce::Image result(juce::Image::ARGB, 500 * scale, 200 * scale, true);
            juce::Graphics painter(result);
            painter.addTransform(juce::AffineTransform::scale((float)scale));
            view.paint(painter);
            return result;
        };
        auto baseline = render(-1);
        for (int64_t cursor : {19200, 19229, 19267})
        {
            auto marked = render(cursor);
            auto centre = [&](int y)
            {
                double sum = 0, weighted = 0;
                for (int x = 190 * scale; x < 210 * scale; ++x)
                {
                    auto a = marked.getPixelAt(x, y * scale), b = baseline.getPixelAt(x, y * scale);
                    auto delta = std::abs((int)a.getRed() - b.getRed()) +
                                 std::abs((int)a.getGreen() - b.getGreen()) +
                                 std::abs((int)a.getBlue() - b.getBlue());
                    sum += delta;
                    weighted += delta * (x + .5) / scale;
                }
                check(sum > 0, "Playhead is visible on range highlight");
                return weighted / sum;
            };
            check(std::abs(centre(27) - centre(60)) < .35,
                  "Playhead pin and vertical line share the same centre at 1x/2x and fractional positions");
        }
    }
    view.setCursor(18000);
    view.markInButton().onClick();
    check(view.startSample() == 18000 && view.endSample() == 36000, "Transport IN retains marking");
    view.setCursor(30000);
    view.markOutButton().onClick();
    check(view.endSample() == 30000, "Transport OUT retains marking");
    auto pointer = [&](juce::Point<float> position, juce::Point<float> down, int clicks = 1)
    {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position,
                                juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, &view, &view,
                                juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), clicks,
                                position != down);
    };
    view.setRange(0, 48000, false, false);
    view.mouseDown(pointer({50, 100}, {50, 100}));
    view.mouseDrag(pointer({250, 100}, {50, 100}));
    view.mouseUp(pointer({250, 100}, {50, 100}));
    check(view.isRangeSelected() && view.startSample() == 4800 && view.endSample() == 24000,
          "Actual waveform drag creates a highlighted range from an unselected recording");
    view.zoomRange();
    view.mouseDown(pointer({250, 100}, {250, 100}));
    view.mouseUp(pointer({250, 100}, {250, 100}));
    check(view.cursorSample() == 14400 && view.startSample() == 4800 && view.endSample() == 24000,
          "Zoom Range maps the waveform midpoint to the exact marked range midpoint");
    int clears = 0;
    view.onClearRange = [&] { ++clears; };
    const auto doubleClick = pointer({125, 100}, {125, 100}, 2);
    view.mouseDown(doubleClick);
    view.mouseUp(doubleClick);
    view.mouseDoubleClick(doubleClick);
    check(!view.isRangeSelected() && view.startSample() == 0 && view.endSample() == 48000 && clears == 1,
          "Double-click clears the dragged range and restores whole-recording bounds");
    view.mouseDown(pointer({250, 100}, {250, 100}));
    view.mouseUp(pointer({250, 100}, {250, 100}));
    check(!view.isRangeSelected() && view.cursorSample() == 24000,
          "A waveform seek does not create an end-to-end range selection");
}
} // namespace
namespace sp
{
struct PlaybackSearchTestAccess
{
    static void run(const juce::File &root, const juce::File &app)
    {
        auto sounds = root.getChildFile("sounds");
        for (int i = 0; i < 23; ++i)
            fixture(sounds.getChildFile("sound-" + juce::String(i).paddedLeft('0', 2) + ".wav"));
        LibraryView v(root.getChildFile("catalog/library.sqlite"), app, false, {}, true);
        v.stopTimer();
        v.setSize(1040, 680);
        v.library.addFolder(sounds);
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.total == 23 && v.snapshot.headersRead == 23 &&
                       v.snapshot.progress.metadataBacklog == 0 && v.snapshot.results.size() == 20 &&
                       std::all_of(v.snapshot.results.begin(), v.snapshot.results.end(),
                                   [](auto &a) { return a.state == "ready"; });
            },
            "Indexed generated fixtures through real Library service");
        v.audio.gain(.02f);
        until([&] { return (double)v.audio.status()["sampleRate"] > 0; }, "Native audio helper available");
        v.table.selectRow(19);
        v.selectedRowsChanged(19);
        check(!v.regionSelected, "Selecting a recording does not mark a range");
        until([&]
        {
            v.updateRecordingHeaders({});
            return v.recordingDetails.getText().contains("Category: Field recordings");
        }, "Recording details read embedded genre from a generated WAV");
        check(v.recordingDetails.getText().contains("48000 Hz") &&
                  v.recordingDetails.getText().contains("24-bit integer") &&
                  v.recordingDetails.getText().contains(v.selectedAsset->path) &&
                  v.recordingDetails.getText().contains("Microphone: Test microphone"),
              "Recording details include catalog and custom WAV iXML metadata");
        auto microphoneColumn = v.addEmbeddedColumn("Microphone");
        v.toggleMetadataColumn(microphoneColumn);
        check(v.table.getHeader().isColumnVisible(microphoneColumn) &&
                  v.table.getHeader().getColumnName(microphoneColumn) == "Microphone",
              "A custom embedded tag can become a visible result column");
        v.setRegion(20, 500);
        v.matchMode = true;
        v.waveform.onClearRange();
        check(!v.regionSelected && !v.waveform.isRangeSelected() && !v.matchMode && v.regionStart == 0 &&
                  v.regionEnd == v.selectedAsset->samples && v.moments.getSelectedId() == 0 &&
                  v.moments.getTextWhenNothingSelected() == "Whole recording",
              "Clearing the range updates Library playback bounds and the range selector");
        v.playMatch.onClick();
        check(!v.regionSelected, "Play Match alone does not create a visual range selection");
        v.audio.stop();
        v.repeatAll.onClick();
        v.playWhole.onClick();
        check(!v.regionSelected, "Play Whole does not select the entire recording");
        std::set<int> heard;
        until(
            [&]
            {
                v.timerCallback();
                auto s = v.audio.status();
                if ((bool)s["playing"] && !(bool)s["pendingPlayback"] && s["id"].toString() == v.playingId)
                    heard.insert(v.playingRow);
                return heard.contains(19) && heard.contains(20) && heard.contains(21) && heard.contains(22) &&
                       heard.contains(0);
            },
            "Repeat All crosses the materialized page and wraps last to first");
        v.toggle();
        auto stoppedGeneration = v.audio.status()["intentGeneration"];
        auto end = monotonicMs() + 400;
        while (monotonicMs() < end)
        {
            v.timerCallback();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!(bool)v.audio.status()["playing"] && v.audio.status()["intentGeneration"] == stoppedGeneration,
              "Stop cannot restart Repeat All");
        v.repeatOne.onClick();
        check(v.repeatOne.getToggleState() && !v.repeatAll.getToggleState(), "Loop modes are exclusive");
        v.waveform.onRange(960, 4800);
        check(v.regionSelected && v.waveform.isRangeSelected(),
              "Explicitly dragging a range enables its highlight");
        v.playMatch.onClick();
        auto generation = v.playingGeneration;
        until(
            [&]
            {
                v.timerCallback();
                auto s = v.audio.status();
                return (uint64_t)(int64_t)s["loopCount"] >= 3 &&
                       (uint64_t)(int64_t)s["playGeneration"] == generation && (bool)s["playing"];
            },
            "Real helper repeats one without replacing its playback generation");
        check((int64_t)v.audio.status()["startSample"] == 960 &&
                  (int64_t)v.audio.status()["endSample"] == 4800,
              "Repeat One retains the selected original range across wraps");
        v.repeatOne.onClick();
        check(!v.repeatOne.getToggleState() && !v.repeatAll.getToggleState(), "Second click restores Off");
        until(
            [&]
            {
                v.timerCallback();
                return (bool)v.audio.status()["completed"] && !(bool)v.audio.status()["pendingPlayback"];
            },
            "Disabling Repeat One finishes the remaining range");
        // Search changes must not queue audio from an unrelated result set.
        v.repeatAll.onClick();
        v.playWhole.onClick();
        v.benchmarkSearch("sound-22");
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == v.requested && v.snapshot.total == 1;
            },
            "Changed query adopted");
        until(
            [&]
            {
                v.timerCallback();
                return !(bool)v.audio.status()["playing"] && !(bool)v.audio.status()["pendingPlayback"];
            },
            "New search cancels old Repeat All queue");
        check(v.allArmedGeneration == 0, "Old query cannot remain armed");
        v.repeatAll.onClick();
        v.benchmarkSearch("");
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == v.requested && v.snapshot.total == 23;
            },
            "All rows restored");
        auto savedPage = v.snapshot;
        v.table.selectRow(0);
        v.selectedRowsChanged(0);
        v.repeatAll.onClick();
        v.playWhole.onClick();
        auto ownedGeneration = v.playingGeneration;
        auto completion = v.audio.status();
        auto *event = completion.getDynamicObject();
        event->setProperty("id", v.playingId);
        event->setProperty("intentId", v.playingId);
        event->setProperty("intentGeneration", (juce::int64)ownedGeneration);
        event->setProperty("playGeneration", (juce::int64)(ownedGeneration - 1));
        event->setProperty("completed", true);
        event->setProperty("playing", false);
        event->setProperty("pendingPlayback", false);
        event->setProperty("error", "");
        v.updateRepeat(completion);
        check(v.playingGeneration == ownedGeneration, "A stale completion cannot advance Repeat All");
        event->setProperty("playGeneration", (juce::int64)ownedGeneration);
        event->setProperty("error", "decode failed");
        v.updateRepeat(completion);
        check(v.playingGeneration == ownedGeneration, "Playback failure cannot masquerade as natural EOF");
        v.snapshot.results[1].online = false;
        v.snapshot.results[2].state = "failed";
        event->setProperty("error", "");
        v.updateRepeat(completion);
        check(v.playingRow == 3 && v.playingGeneration != ownedGeneration,
              "Repeat All skips known offline and unreadable rows");
        v.audio.stop();
        v.cancelNavigation();
        v.repeatAll.onClick();
        v.snapshot = savedPage;
        auto candidates = v.snapshot.results;
        // Scores deliberately arrive in reverse order, with NaN and weak moments.
        for (size_t i = 0; i < candidates.size(); ++i)
            candidates[i].matches = {{0, 100, .30f + (float)i * .03f},
                                     {100, 200, .10f},
                                     {200, 300, std::numeric_limits<float>::quiet_NaN()}};
        AudioReference ref;
        ref.source = candidates.front();
        ref.endSample = 1000;
        v.submitReference(ref);
        auto request = v.requested;
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.request == request && v.snapshot.total == 0;
            },
            "Example search starts without a lexical tail");
        v.library.publishControlledSemantic(request, candidates);
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.semanticCompletedRequest == request;
            },
            "Controlled scores pass through actual fusion");
        check(v.snapshot.total == 11, "Only scores meeting default .55 cutoff are published");
        float previous = 2;
        for (auto &a : v.snapshot.results)
        {
            check(a.matches.size() == 1 && a.matches.front().similarity >= .55f &&
                      a.matches.front().similarity <= previous,
                  "Only qualifying finite moments remain, strictly score ordered");
            previous = a.matches.front().similarity;
        }
        v.similarityCutoff.setSelectedId(85, juce::dontSendNotification);
        v.similarityCutoff.onChange();
        auto stricter = v.requested;
        check(stricter != request, "Cutoff changes invalidate older result generations");
        v.library.publishControlledSemantic(request, candidates);
        v.library.publishControlledSemantic(stricter, candidates);
        until(
            [&]
            {
                v.timerCallback();
                return v.snapshot.semanticCompletedRequest == stricter;
            },
            "Stricter cutoff adopted");
        check(v.snapshot.total == 1 && v.snapshot.results.front().id == candidates.back().id,
              "Tight cutoff has no filler results");
        // Visibility and bounds in both real layouts, including minimum Browser width.
        v.setCompact(true);
        v.setSize(260, 680);
        check(v.repeatOne.isVisible() && v.repeatAll.isVisible() && !v.similarityCutoff.isVisible(),
              "Compact browser exposes loop modes");
        for (auto *button : {&v.play, &v.repeatOne, &v.repeatAll, &v.addDesigner})
            check(v.getLocalBounds().contains(button->getBounds()),
                  "Compact playback controls remain inside browser");
        check(!v.play.getBounds().intersects(v.repeatOne.getBounds()) &&
                  !v.repeatAll.getBounds().intersects(v.addDesigner.getBounds()),
              "Compact transport controls do not overlap");
        v.setCompact(false);
        v.setSize(1040, 680);
        v.snapshot.reference.present = true;
        v.resized();
        check(v.similarityCutoff.isVisible() &&
                  !v.similarityCutoff.getBounds().intersects(v.referenceDesigner.getBounds()),
              "Minimum full Library fits the cutoff");
        juce::PropertiesFile prefs(root.getChildFile("prefs.settings"), ui::preferenceOptions());
        v.bindPreferences(prefs);
        v.repeatOne.onClick();
        check(prefs.getIntValue("library.repeatMode", 0) == 1,
              "Loop preference persists without document edits");
        v.audio.stop();
    }
};
} // namespace sp
void runPlaybackSearchTests(const juce::File &parent, const juce::File &app)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto file = root.getChildFile("pcm.wav");
    fixture(file);
    pcm(file);
    waveform();
    sp::PlaybackSearchTestAccess::run(root, app);
    std::cout << "Library playback/search checks: " << checks << " passed\n";
}
