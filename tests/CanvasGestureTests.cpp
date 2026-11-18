#include "CanvasGestureTests.h"
#include "DesignerCanvas.h"
#include "UIComponents.h"
#include "LibraryLayout.h"
#include "DesignLive.h"
#include <iostream>
#include <future>
namespace
{
int checks = 0;
void check(bool value, const char *message)
{
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
sp::Asset source(int rate)
{
    sp::Asset asset;
    asset.id = juce::Uuid().toString();
    asset.path = "/gesture-fixture.wav";
    asset.name = "fixture.wav";
    asset.sampleRate = rate;
    asset.samples = rate * 10;
    asset.duration = 10;
    asset.channels = 4;
    asset.revision = 1;
    return asset;
}
juce::MouseEvent pointer(sp::DesignerCanvas &canvas, juce::Point<float> position, juce::Point<float> down,
                         juce::ModifierKeys modifiers = {})
{
    return {juce::Desktop::getInstance().getMainMouseSource(),
            position,
            modifiers,
            1,
            0,
            0,
            0,
            0,
            &canvas,
            &canvas,
            juce::Time::getCurrentTime(),
            down,
            juce::Time::getCurrentTime(),
            1,
            position != down};
}
void fixture(const juce::File &file)
{
    juce::WavAudioFormat format;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), 48000, 1, 16, {}, 0));
    juce::AudioBuffer<float> buffer(1, 480);
    buffer.clear();
    check(writer && writer->writeFromAudioSampleBuffer(buffer, 0, 480), "Drop fixture written");
}
} // namespace
void runCanvasGestureTests(const juce::File &parent)
{
    juce::ScopedJuceInitialiser_GUI init;
    auto directory = parent.getChildFile(juce::Uuid().toString());
    directory.createDirectory();
    for (int sourceRate : {44100, 48000, 96000})
        for (double linked : {.25, 1., 4.})
            for (bool reverse : {false, true})
            {
                auto layer =
                    sp::makeDesignLayer(source(sourceRate), sourceRate / 4, sourceRate * 7 / 4, true);
                layer.rate = linked;
                layer.reverse = reverse;
                layer.placement = 240000;
                layer.sync = sourceRate;
                for (int64_t delta : {-100000000LL, -480000LL, -1LL, 0LL, 1LL, 48000LL, 100000000LL})
                    for (bool left : {false, true})
                    {
                        auto changed = layer;
                        sp::trimLayer(changed, layer, left, delta, 48000);
                        check(changed.region.startSample >= 0 &&
                                  changed.region.endSample <= layer.source.samples &&
                                  changed.region.endSample > changed.region.startSample,
                              "Trim retains nonempty bounded original samples");
                        check(changed.placement >= 0 && changed.sync >= changed.region.startSample &&
                                  changed.sync < changed.region.endSample,
                              "Trim respects zero timeline and valid sync");
                        if (left)
                            check(changed.placement + sp::layerDuration(changed, 48000) ==
                                      layer.placement + sp::layerDuration(layer, 48000),
                                  "Left trim keeps opposite edge exactly on design clock");
                        else
                            check(changed.placement == layer.placement,
                                  "Right trim keeps opposite timeline edge");
                        sp::DesignDocument document;
                        document.original.layers = {changed};
                        sp::validateDesign(document);
                        auto slipped = layer;
                        sp::slipLayer(slipped, layer, delta, 48000);
                        check(slipped.placement == layer.placement &&
                                  sp::layerDuration(slipped, 48000) == sp::layerDuration(layer, 48000),
                              "Slip preserves placement and duration forward/reverse/rate");
                        check(slipped.region.endSample - slipped.region.startSample ==
                                      layer.region.endSample - layer.region.startSample &&
                                  slipped.region.startSample >= 0 &&
                                  slipped.region.endSample <= layer.source.samples,
                              "Slip retains interval length and source bounds");
                    }
                auto full = sp::makeDesignLayer(layer.source, 0, layer.source.samples, true);
                full.placement = 240000;
                full.rate = linked;
                full.reverse = reverse;
                auto before = full;
                sp::trimLayer(full, before, true, -96000, 48000);
                check(full.placement == before.placement,
                      "Full-source left trim cannot move whole sound past source bounds");
                auto faded = layer;
                sp::fadeLayer(faded, layer, true, INT32_MAX, 48000);
                sp::fadeLayer(faded, faded, false, -INT32_MAX, 48000);
                check(faded.fadeIn == sp::layerDuration(layer, 48000) && faded.fadeOut == faded.fadeIn,
                      "Overlapping linear fades each clamp to duration");
                auto sync = layer;
                sp::syncLayer(sync, layer, INT32_MAX, 48000);
                check(sync.sync >= sync.region.startSample && sync.sync < sync.region.endSample &&
                          sync.placement == layer.placement,
                      "Sync drag cannot move audio or escape source interval");
            }
    sp::DesignDocument document;
    auto a = sp::makeDesignLayer(source(48000), 0, 96000, true), b = a, c = a;
    b.id = juce::Uuid().toString();
    c.id = juce::Uuid().toString();
    a.placement = 240000;
    b.placement = 144000;
    c.placement = 48000;
    document.original.layers = {a, b, c};
    document.selectedLayer = a.id;
    auto original = document;
    sp::moveLayers(document, original, {a.id, b.id}, -480000);
    check(document.original.layers[0].placement == 96000 && document.original.layers[1].placement == 0 &&
              document.original.layers[2].placement == c.placement,
          "Group move uses one common clamped delta");
    auto beforeReorder = document;
    sp::reorderLayer(document, a.id, 3);
    check(document.original.layers.back().id == a.id &&
              document.original.layers.back().placement == beforeReorder.original.layers[0].placement,
          "Header reorder preserves placement");
    sp::TimelineViewport viewport;
    viewport.width = 1000;
    viewport.pixelsPerSecond = 70;
    viewport.startSeconds = 1;
    auto x = viewport.x(240000, 48000);
    viewport.zoom(x, 2);
    check(std::abs(viewport.frame(x, 48000) - 240000) <= 1, "Zoom retains pointer-centered time");
    auto scale = viewport.pixelsPerSecond;
    sp::moveLayers(document, document, {a.id}, 480000);
    check(viewport.pixelsPerSecond == scale, "Last-layer edit cannot alter persistent viewport scale");
    auto shape = sp::clipGeometry(document.original.layers[0], 0, viewport, 48000);
    check(sp::canvasHitTest(document, viewport, viewport.header + 10, shape.top + 50).target ==
              sp::CanvasTarget::empty,
          "Empty row timeline cannot drag a clip");
    document = original;
    viewport.startSeconds = 0;
    viewport.pixelsPerSecond = 50;
    shape = sp::clipGeometry(a, 0, viewport, 48000);
    check(sp::canvasHitTest(document, viewport, shape.left, shape.top + 10).target ==
              sp::CanvasTarget::fadeIn,
          "Zero-fade corner wins over trim/body");
    check(sp::canvasHitTest(document, viewport, shape.left, shape.top + 30).target ==
              sp::CanvasTarget::trimIn,
          "Trim has a distinct edge hit target");
    check(sp::canvasHitTest(document, viewport, shape.sync, shape.top + 69).target == sp::CanvasTarget::sync,
          "Sync triangle is a separate target");
    check(sp::canvasHitTest(document, viewport, shape.left + 25, shape.gain).target == sp::CanvasTarget::gain,
          "Gain line is a distinct constant-gain target");
    auto narrow = document;
    narrow.original.layers[0].region.endSample = 1;
    narrow.original.layers[0].sync = 0;
    auto narrowShape = sp::clipGeometry(narrow.original.layers[0], 0, viewport, 48000);
    check(sp::canvasHitTest(narrow, viewport, narrowShape.right, narrowShape.fadeOutY).target ==
              sp::CanvasTarget::fadeOut,
          "Narrow clips retain an independently reachable OUT grip");
    for (auto fades : {std::pair<int64_t, int64_t>{48000, 48000}, {96000, 0}, {0, 96000}})
    {
        auto overlap = original;
        overlap.original.layers[0].fadeIn = fades.first;
        overlap.original.layers[0].fadeOut = fades.second;
        auto g = sp::clipGeometry(overlap.original.layers[0], 0, viewport, 48000);
        check(sp::canvasHitTest(overlap, viewport, g.fadeIn, g.fadeInY).target == sp::CanvasTarget::fadeIn &&
                  sp::canvasHitTest(overlap, viewport, g.fadeOut, g.fadeOutY).target ==
                      sp::CanvasTarget::fadeOut,
              "Both coincident fade handles have separate painted and hit-test positions");
    }
    for (float width : {280.f, 700.f, 2200.f})
    {
        sp::TimelineViewport fit;
        fit.width = width;
        fit.fit(0, 48000LL * 3601, 48000);
        check(fit.x(48000LL * 3601, 48000) <= width - 18 && fit.startSeconds == 0,
              "Fit Design fits a full valid hour plus tail at narrow and wide widths");
    }
    // Exercise the real component's pointer handlers and session history, not a second edit implementation.
    std::deque<std::function<void()>> jobs, posts;
    sp::DesignSessionServices services;
    services.timers = false;
    services.automaticRecovery = false;
    services.submit = [&](auto job)
    {
        jobs.push_back(std::move(job));
        return true;
    };
    services.post = [&](auto job) { posts.push_back(std::move(job)); };
    services.status = [] { return sp::message("status"); };
    services.stop = [] {};
    services.resolve = [](auto &) {};
    services.play = [](const auto &, const auto &, int64_t, const auto &) { return uint64_t(1); };
    services.parameters = [](const auto &, uint64_t, double) {};
    sp::DesignSession session(directory.getChildFile("library.sqlite"), services);
    session.document = original;
    session.selectedIds = {a.id};
    juce::PropertiesFile preferences(directory.getChildFile("interface.settings"),
                                     sp::ui::preferenceOptions());
    sp::DesignerCanvas canvas(
        session, [](const auto &) -> std::shared_ptr<const sp::WaveformData> { return {}; }, preferences);
    canvas.setBounds(0, 0, 1000, 400);
    canvas.syncSelectors();
    canvas.viewport.pixelsPerSecond = 50;
    canvas.viewport.startSeconds = 0;
    canvas.snapping.enabled = false;
    // View-only heights and selection must not alter the audio recipe/history.
    auto image = [&]
    {
        juce::Image result(juce::Image::ARGB, 1000, 600, true);
        juce::Graphics g(result);
        canvas.paint(g);
        return result;
    };
    shape = sp::clipGeometry(a, 0, canvas.viewport, 48000);
    juce::Point<float> clipClick{shape.left + 50, shape.top + 52};
    canvas.mouseDown(pointer(canvas, clipClick, clipClick));
    canvas.mouseUp(pointer(canvas, clipClick, clipClick));
    auto clipImage = image();
    auto lanePixel = clipImage.getPixelAt(950, (int)shape.top + 40);
    auto clipPixel = clipImage.getPixelAt((int)shape.left + 20, (int)shape.top + 40);
    juce::Point<float> headerClick{20, shape.top + 20};
    canvas.mouseDown(pointer(canvas, headerClick, headerClick));
    canvas.mouseUp(pointer(canvas, headerClick, headerClick));
    auto trackImage = image();
    check(lanePixel == sp::ui::surface && trackImage.getPixelAt(950, (int)shape.top + 40) != lanePixel &&
              trackImage.getPixelAt((int)shape.left + 20, (int)shape.top + 40) != clipPixel,
          "Clip selection paints only the clip; track selection has a distinct lane and clip state");
    auto beforeHeight = session.document.revision;
    juce::Point<float> edge{20, canvas.viewport.rowTop(1) - 3}, taller{20, edge.y + 90};
    canvas.mouseDown(pointer(canvas, edge, edge));
    canvas.mouseDrag(pointer(canvas, taller, edge));
    canvas.mouseUp(pointer(canvas, taller, edge));
    check(canvas.viewport.rowHeight(0) == 168 && canvas.viewport.rowTop(1) == 200 &&
              session.document.revision == beforeHeight && !session.canUndo(),
          "Real lower-edge drag changes only this track's height without audio edits/history");
    auto resizedShape = sp::clipGeometry(b, 1, canvas.viewport, 48000);
    check(sp::canvasHitTest(session.document, canvas.viewport, resizedShape.left, resizedShape.fadeInY).row ==
              1,
          "Gesture hit mapping follows resized rows");
    canvas.zoomVertical(1.25);
    check(canvas.viewport.rowHeight(0) == 210 && canvas.viewport.rowHeight(1) == 97.5,
          "Vertical zoom scales all rows independently of horizontal zoom");
    {
        sp::DesignerCanvas reopened(
            session, [](const auto &) -> std::shared_ptr<const sp::WaveformData> { return {}; }, preferences);
        reopened.setBounds(0, 0, 1000, 400);
        check(reopened.viewport.rowHeight(0) == 210 && reopened.viewport.rowHeight(1) == 97.5,
              "A reopened canvas restores independent heights and vertical zoom from preferences");
    }
    canvas.zoomVertical(1 / 1.25);
    edge = {20, canvas.viewport.rowTop(1) - 3};
    canvas.mouseDoubleClick(pointer(canvas, edge, edge));
    check(canvas.viewport.rowHeight(0) == 78, "Double-click track edge restores default height");
    auto structure = sp::designStructure(session.document);
    session.edit("Colour", [](auto &d) { d.original.layers[0].colour = 0xff72b8a2; });
    auto coloured = sp::designToJson(session.document);
    check(sp::designFromJson(coloured).original.layers[0].colour == 0xff72b8a2 &&
              sp::designStructure(session.document) == structure,
          "Track colour survives serialization without invalidating the audio graph");
    session.undo();
    check(session.document.original.layers[0].colour == 0, "Track colour is undoable");
    session.redo();
    check(session.document.original.layers[0].colour == 0xff72b8a2, "Track colour is redoable");
    session.undo();
    (*coloured["original"]["layers"].getArray())[0].getDynamicObject()->removeProperty("colour");
    check(sp::designFromJson(coloured).original.layers[0].colour == 0,
          "Existing version-one documents keep the default colour");
    session.edit("Coincident fades",
                 [](auto &d) { d.original.layers[0].fadeIn = d.original.layers[0].fadeOut = 48000; });
    auto coincident = sp::clipGeometry(session.document.original.layers[0], 0, canvas.viewport, 48000);
    for (bool in : {false, true})
    {
        juce::Point<float> from{in ? coincident.fadeIn : coincident.fadeOut,
                                in ? coincident.fadeInY : coincident.fadeOutY};
        auto to = from + juce::Point<float>(10, 0);
        canvas.mouseDown(pointer(canvas, from, from));
        canvas.mouseDrag(pointer(canvas, to, from));
        canvas.mouseUp(pointer(canvas, to, from));
        auto &layer = session.document.original.layers[0];
        check((in ? layer.fadeIn == 57600 && layer.fadeOut == 48000
                  : layer.fadeOut == 38400 && layer.fadeIn == 48000),
              "Real pointer handlers independently edit both coincident fade handles");
        session.undo();
    }
    session.undo();
    shape = sp::clipGeometry(a, 0, canvas.viewport, 48000);
    juce::Point<float> start{shape.fadeIn, shape.top + 10}, end{shape.fadeIn + 50, shape.top + 10};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseDrag(pointer(canvas, end, start));
    canvas.mouseUp(pointer(canvas, end, start));
    check(session.document.original.layers[0].fadeIn == 48000,
          "Real fade handle edits duration at captured scale");
    session.undo();
    check(session.document.original.layers[0].fadeIn == 0 && !session.canUndo(),
          "One undo restores complete fade gesture");
    start = {shape.left + 80, shape.top + 52};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseUp(pointer(canvas, start, start));
    check(!session.canUndo(), "No-op body click creates no edit/history");
    auto prior = session.document;
    start = {shape.left + 80, shape.top + 52};
    end = {start.x + 100, start.y};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseDrag(pointer(canvas, end, start));
    canvas.cancelDrag();
    check(session.document.original.layers[0].placement == prior.original.layers[0].placement &&
              !session.canUndo(),
          "Escape restores model and leaves no gesture history");
    start = {shape.left + 80, shape.top + 52};
    end = {-1000, start.y};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseDrag(pointer(canvas, end, start));
    canvas.mouseUp(pointer(canvas, end, start));
    check(session.document.original.layers[0].placement == 0,
          "Release outside component commits bounded move");
    session.undo();
    // Modifier routing must reach the same bounded edit commands from real pointer events.
    shape = sp::clipGeometry(session.document.original.layers[0], 0, canvas.viewport, 48000);
    start = {shape.left + 50, shape.top + 52};
    end = {start.x + 50, start.y};
    juce::ModifierKeys option(juce::ModifierKeys::altModifier);
    canvas.mouseDown(pointer(canvas, start, start, option));
    canvas.mouseDrag(pointer(canvas, end, start, option));
    canvas.mouseUp(pointer(canvas, end, start, option));
    check(session.document.original.layers[0].region.startSample == 48000 &&
              session.document.original.layers[0].region.endSample == 144000 &&
              session.document.original.layers[0].placement == a.placement,
          "Real Option-body drag slips source without moving audio");
    session.undo();
    start = {shape.left + 50, shape.gain};
    end = {start.x, start.y + 20};
    juce::ModifierKeys fine(juce::ModifierKeys::ctrlModifier);
    canvas.mouseDown(pointer(canvas, start, start, fine));
    canvas.mouseDrag(pointer(canvas, end, start, fine));
    canvas.mouseUp(pointer(canvas, end, start, fine));
    check(std::abs(session.document.original.layers[0].gainDb + .5) < 1e-9,
          "Real Control-gain drag applies fine dB adjustment");
    auto gained = sp::clipGeometry(session.document.original.layers[0], 0, canvas.viewport, 48000);
    start = {gained.left + 50, gained.gain};
    canvas.mouseDoubleClick(pointer(canvas, start, start));
    check(session.document.original.layers[0].gainDb == 0, "Gain line double-click resets unity");
    session.undo();
    session.undo();
    start = {20, 32 + 2 * 78 + 20};
    end = {20, 32};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseDrag(pointer(canvas, end, start));
    canvas.mouseUp(pointer(canvas, end, start));
    check(session.document.original.layers[0].id == c.id &&
              session.document.original.layers[0].placement == c.placement,
          "Real header drag reorders without changing placement");
    session.undo();
    session.selectLayer(a.id);
    auto second = sp::clipGeometry(b, 1, canvas.viewport, 48000);
    start = {second.left + 50, second.top + 52};
    juce::ModifierKeys additive(juce::ModifierKeys::shiftModifier);
    canvas.mouseDown(pointer(canvas, start, start, additive));
    canvas.mouseUp(pointer(canvas, start, start, additive));
    check(session.selectedIds == std::set<juce::String>{a.id, b.id},
          "Real Shift-click creates transient multi-selection");
    start = {shape.left + 50, shape.top + 52};
    end = {-1000, start.y};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseDrag(pointer(canvas, end, start));
    canvas.mouseUp(pointer(canvas, end, start));
    check(session.document.original.layers[0].placement == 96000 &&
              session.document.original.layers[1].placement == 0,
          "Real group drag uses one common clamped delta");
    session.undo();
    auto priorSelection = session.selectedIds;
    auto third = sp::clipGeometry(c, 2, canvas.viewport, 48000);
    start = {third.left + 50, third.top + 52};
    end = {start.x + 30, start.y};
    canvas.mouseDown(pointer(canvas, start, start));
    canvas.mouseDrag(pointer(canvas, end, start));
    canvas.cancelDrag();
    check(session.selectedIds == priorSelection && session.document.selectedLayer == b.id &&
              session.document.original.layers[2].placement == c.placement,
          "Escape restores pre-pointer-down model and multi-selection");
    session.selectLayer(a.id);
    canvas.snapping.enabled = true;
    start = {shape.left + 50, shape.top + 52};
    end = {start.x + 7, start.y};
    juce::ModifierKeys bypass(juce::ModifierKeys::commandModifier);
    canvas.mouseDown(pointer(canvas, start, start, bypass));
    canvas.mouseDrag(pointer(canvas, end, start, bypass));
    canvas.mouseUp(pointer(canvas, end, start, bypass));
    check(session.document.original.layers[0].placement == a.placement + 6720,
          "Command bypass preserves unsnapped pointer delta");
    session.undo();
    canvas.snapping.enabled = false;
    auto asset = source(48000);
    session.document.playhead = 48000;
    session.add(asset, 100, 1000, false, 1);
    auto added = session.document.activeMix().layers.back();
    check(added.placement == 48000 && added.source.id == asset.id && added.region.startSample == 100 &&
              added.region.endSample == 1000 && added.region.auditionPair == 1 && !added.external,
          "Add captures source identity/range/mapping at parked playhead");
    auto one = directory.getChildFile("one.wav"), two = directory.getChildFile("two.wav");
    fixture(one);
    fixture(two);
    {
        // A blocked optional decoder must not hold the view or application alive.
        auto entered = std::make_shared<std::promise<void>>();
        auto enteredFuture = entered->get_future();
        auto release = std::make_shared<std::promise<void>>();
        auto releaseFuture = release->get_future().share();
        auto returned = std::make_shared<std::promise<void>>();
        auto returnedFuture = returned->get_future();
        auto cache = std::make_unique<sp::WaveformCache>(directory.getChildFile("blocked-optional-cache"),
                                                         [entered, releaseFuture, returned]
                                                         {
                                                             entered->set_value();
                                                             releaseFuture.wait();
                                                             returned->set_value();
                                                         });
        auto recording = source(48000);
        recording.path = one.getFullPathName();
        recording.samples = 480;
        recording.channels = 1;
        sp::FileStamp stamp;
        check(sp::fileStamp(one, stamp), "Blocked waveform fixture is a regular file");
        recording.size = stamp.size;
        recording.modified = stamp.modified;
        recording.inode = stamp.inode;
        cache->request(recording);
        auto started = enteredFuture.wait_for(std::chrono::seconds(2));
        auto before = juce::Time::getMillisecondCounterHiRes();
        cache.reset();
        auto elapsed = juce::Time::getMillisecondCounterHiRes() - before;
        release->set_value();
        check(started == std::future_status::ready && elapsed < 500,
              "Optional cache shutdown is bounded while a decoder is blocked");
        check(returnedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
              "Blocked worker safely resumes after the cache and view are destroyed");
    }
    session.addExternals({one.getFullPathName(), two.getFullPathName()}, 120000);
    while (!jobs.empty())
    {
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
    }
    while (!posts.empty())
    {
        auto job = std::move(posts.front());
        posts.pop_front();
        job();
    }
    auto &layers = session.document.activeMix().layers;
    check(layers[layers.size() - 2].name == "one.wav" && layers.back().name == "two.wav" &&
              layers.back().placement == 120000 && layers[layers.size() - 2].placement == 120000,
          "Multi-file drop retains incoming order at common pointer time");
    session.undo();
    check(session.document.activeMix().layers.size() == 4, "One undo removes entire multi-file drop");
    for (int height : {651, 680, 861, 1100})
        for (bool reference : {false, true})
        {
            auto layout = sp::libraryLayout(1040, height, false, reference);
            check(layout.results.getBottom() <= layout.selection.getY() - 8,
                  "Reference/results/selected regions cannot overlap at minimum height");
            check(layout.results.getHeight() >= 90 && layout.waveform.getHeight() >= 60,
                  "Minimum Library retains usable results and waveform");
            if (reference)
                check(layout.referenceWave.getBottom() < layout.results.getY(),
                      "Reference waveform stays above result rows");
        }
    for (int width : {260, 300, 440})
    {
        auto layout = sp::libraryLayout(width, 651, true, false);
        check(layout.results.getX() >= 0 && layout.results.getRight() <= width &&
                  layout.results.getWidth() > 0,
              "Compact empty/loading/error area stays inside actual result bounds");
    }
    std::cout << "PASS canvas timing/targets, trim/rate boundaries, real pointer gestures/history and drops: "
              << checks << " checks\n";
}
