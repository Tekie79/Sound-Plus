#include "PerformanceStateTests.h"
#include "PerformanceState.h"
#include "DesignLive.h"
#include "DesignSession.h"
#include "PerformView.h"
#include "Performance.h"
#include <iostream>
#include <cmath>
#include <deque>
#include <thread>
namespace
{
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f, const char *why)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (...)
    {
        rejected = true;
    }
    check(rejected, why);
}
juce::String mixText(const sp::DesignMix &mix)
{
    sp::DesignDocument d;
    d.original = mix;
    return juce::JSON::toString(sp::designToJson(d)["original"], true);
}
double pcmDifference(const sp::DesignDocument &a, const sp::DesignDocument &b)
{
    sp::DesignReader ar(a, 48000, 0, -1, .1), br(b, 48000, 0, -1, .1);
    juce::AudioBuffer<float> x(2, 512), y(2, 512);
    double error = 0;
    for (;;)
    {
        int n = ar.read(x, 512);
        check(n == br.read(y, 512), "Effective-state PCM lengths match");
        if (!n)
            break;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                error = std::max(error, (double)std::abs(x.getSample(c, i) - y.getSample(c, i)));
    }
    return error;
}
} // namespace
namespace sp
{
struct PerformanceViewTestAccess
{
    static juce::MouseEvent pointer(juce::Component &c, juce::Point<float> p, juce::Point<float> start)
    {
        return {juce::Desktop::getInstance().getMainMouseSource(),
                p,
                {},
                1,
                0,
                0,
                0,
                0,
                &c,
                &c,
                juce::Time::getCurrentTime(),
                start,
                juce::Time::getCurrentTime(),
                1,
                p != start};
    }
    static void padBoundary(PerformView &view)
    {
        auto &session = view.session;
        auto saved = session.document.activeMix().performance;
        auto &pad = view.pad;
        for (auto size :
             {juce::Point<int>(664, 308), juce::Point<int>(752, 360), juce::Point<int>(1400, 1000)})
        {
            view.setSize(size.x, size.y);
            juce::Path frame;
            frame.addRoundedRectangle(pad.surfaceBounds(), 12);
            auto render = [&](double x, double y)
            {
                auto &state = session.document.activeMix().performance;
                state.x = x;
                state.y = y;
                juce::Image image(juce::Image::ARGB, pad.getWidth(), pad.getHeight(), true);
                juce::Graphics g(image);
                pad.paint(g);
                return image;
            };
            for (bool enabled : {false, true})
            {
                session.document.activeMix().performance.blendEnabled = enabled;
                auto centre = render(.5, .5);
                for (double x : {0., .5, 1.})
                    for (double y : {0., .5, 1.})
                    {
                        auto pixels = render(x, y);
                        for (int py = 0; py < pixels.getHeight(); ++py)
                            for (int px = 0; px < pixels.getWidth(); ++px)
                                if (!frame.contains(px + .5f, py + .5f))
                                    check(pixels.getPixelAt(px, py) == centre.getPixelAt(px, py),
                                          "Pad marker/glow never paint outside the rounded frame");
                    }
            }
            auto start = pad.movementBounds().getCentre();
            for (auto end : {juce::Point<float>(-100, -100), juce::Point<float>(pad.getWidth() + 100.f, -100),
                             juce::Point<float>(-100, pad.getHeight() + 100.f),
                             juce::Point<float>(pad.getWidth() + 100.f, pad.getHeight() + 100.f)})
            {
                pad.mouseDown(pointer(pad, start, start));
                pad.mouseDrag(pointer(pad, end, start));
                pad.mouseUp(pointer(pad, end, start));
                auto &state = session.document.activeMix().performance;
                check(state.x == (end.x < 0 ? 0. : 1.) && state.y == (end.y < 0 ? 0. : 1.),
                      "Dragging beyond every pad corner still reaches exact 0/1 model endpoints");
                session.undo();
            }
            auto corner = pad.movementBounds().getBottomRight();
            pad.mouseDown(pointer(pad, corner, corner));
            pad.mouseUp(pointer(pad, corner, corner));
            check(session.document.activeMix().performance.x == 1 &&
                      session.document.activeMix().performance.y == 1,
                  "Visible inset marker endpoint and mouse mapping agree");
            session.undo();
        }
        session.document.activeMix().performance = saved;
        view.setSize(752, 360);
        view.refresh();
    }
    static juce::Point<int> dropPoint(PerformView &view, int slot)
    {
        auto b = view.slotBounds[(size_t)slot];
        return view.getLocalPoint(&view.blendContent, b.getCentre());
    }
    static void workspaceLayout(PerformView &view)
    {
        const auto before = mixText(view.session.document.activeMix());
        const auto revision = view.session.document.revision;
        view.setSize(1280, 700);
        check(view.macroArea.getWidth() == ui::workspace::sidebarWidth && view.pad.getWidth() >= 240,
              "Macros use the same fixed logical width as the Library sidebar");
        check(view.xLabel.getText() == "X" && view.yLabel.getText() == "Y" &&
                  view.yValue.getSliderStyle() == juce::Slider::LinearVertical &&
                  view.yValue.getRight() <= view.pad.getX() && view.xValue.getY() >= view.pad.getBottom(),
              "Single-letter axes sit vertically left and horizontally below the pad");
        for (auto size :
             {juce::Point<int>(690, 352), juce::Point<int>(950, 540), juce::Point<int>(1500, 900),
              juce::Point<int>(690, 1800), juce::Point<int>(2500, 352), juce::Point<int>(2500, 1800)})
        {
            view.setSize(size.x, size.y);
            {
                check(view.blendBounds.getWidth() >= PerformView::minimumBlendWidth &&
                          view.macroArea.getWidth() == ui::workspace::sidebarWidth,
                      "Macro width stays constant at minimum, wide and tall sizes");
                check(view.blendBounds.contains(view.blendContent.getBounds()) &&
                          view.blendContent.getLocalBounds().contains(view.pad.getBounds()) &&
                          view.pad.getWidth() >= 280 && view.pad.getHeight() >= 208,
                      "Resizing keeps the pad and all controls inside their panel");
                const auto padInView = view.getLocalArea(&view.blendContent, view.pad.getBounds());
                check(view.pad.getWidth() <= 640 && view.pad.getHeight() <= 640 &&
                          view.pad.getWidth() * 2 <= view.pad.getHeight() * 3 &&
                          view.pad.getHeight() * 2 <= view.pad.getWidth() * 3,
                      "Pad never stretches beyond 640 points or a 3:2 aspect ratio in either direction");
                check(std::abs(padInView.getCentreX() - view.blendBounds.getCentreX()) <= 1 &&
                          std::abs(padInView.getCentreY() - view.blendBounds.getCentreY()) <= 1,
                      "Bounded pad remains centred as screen shape changes");
                for (auto *axis : std::initializer_list<juce::Component *>{&view.xValue, &view.yValue,
                                                                           &view.xLabel, &view.yLabel})
                    check(view.blendContent.getLocalBounds().contains(axis->getBounds()),
                          "Both axes and labels fit completely without scrolling or clipping");
                for (int i = 0; i < 4; ++i)
                {
                    auto b = view.slotBounds[(size_t)i];
                    check(view.blendContent.getLocalBounds().contains(b) &&
                              !b.intersects(view.pad.getBounds()) && b.getHeight() >= 32 &&
                              b.getHeight() <= 64 &&
                              (i < 2 ? b.getBottom() <= view.pad.getY() : b.getY() > view.xValue.getBottom()),
                          "Compact source rows remain above and below the pad, without side gutters");
                    auto point = dropPoint(view, i);
                    check(view.blendContent.getBounds().contains(point) && view.slotAt(point) == i,
                          "Every source assignment stays fully visible and accepts drops without scrolling");
                    auto chooser = view.assignments[(size_t)i].getBounds();
                    check(chooser.getWidth() == chooser.getHeight() &&
                              chooser.getCentreY() ==
                                  view.assignmentNames[(size_t)i].getBounds().getCentreY(),
                          "Source dropdown border is square and aligned with its name row");
                }
                check(view.slotBounds[0].getX() == view.pad.getX() + 8 &&
                          std::abs(view.slotBounds[1].getRight() - (view.pad.getRight() - 8)) <= 1,
                      "Source row edges align with the visible pad frame");
                for (auto &slider : view.macros)
                    check(view.macroArea.contains(slider.getBounds()),
                          "Macro sliders stay inside compact sidebar");
                view.openMapping(0);
                for (auto *field : {&view.minimum, &view.maximum})
                    check(field->isVisible() && view.macroArea.contains(field->getBounds()),
                          "Both mapping range fields remain usable at minimum width");
                view.back.onClick();
            }
        }
        view.setSize(2500, 1800);
        const juce::Point<int> cappedPad{view.pad.getWidth(), view.pad.getHeight()};
        view.setSize(3000, 2000);
        check(juce::Point<int>(view.pad.getWidth(), view.pad.getHeight()) == cappedPad &&
                  cappedPad == juce::Point<int>(640, 640),
              "Further window growth cannot stretch the pad after its maximum size");
        view.setSize(690, 352);
        check(view.pad.getWidth() >= 280 && view.pad.getHeight() >= 208,
              "Supported minimum window protects a usable pad instead of squeezing it further");
        check(view.yValue.valueToProportionOfLength(0) == 1 &&
                  view.yValue.valueToProportionOfLength(1) == 0 &&
                  view.yValue.proportionOfLengthToValue(1) == 0,
              "Vertical Y keeps the existing zero-at-top, one-at-bottom source orientation");
        check(view.macroArea.getWidth() == ui::workspace::sidebarWidth &&
                  view.blendBounds.getWidth() > view.macroArea.getWidth(),
              "The equal-width macro sidebar cannot squeeze the primary Blend workspace");
        check(mixText(view.session.document.activeMix()) == before &&
                  view.session.document.revision == revision,
              "Workspace resizing never changes the design, controls, or audio revision");
        view.setSize(752, 360);
    }
    static void run(DesignSession &session)
    {
        PerformView view(session);
        view.setSize(752, 360);
        session.onChange = [&] { view.refresh(); };
        workspaceLayout(view);
        padBoundary(view);
        const auto axisY = session.document.activeMix().performance.y;
        view.yValue.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        check(session.document.activeMix().performance.y > axisY && view.yValue.getWantsKeyboardFocus(),
              "Down on the focusable vertical Y axis moves toward the lower source row");
        session.undo();
        view.yValue.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
        check(session.document.activeMix().performance.y < axisY,
              "Up on the vertical Y axis moves toward the upper source row");
        session.undo();
        session.document.activeMix().performance.blendLaw = 1;
        view.refresh();
        check(view.updateBlend.isVisible(), "Legacy blend exposes an explicit upgrade action");
        view.updateBlend.onClick();
        check(session.document.activeMix().performance.blendLaw == 2,
              "Actual upgrade action selects continuous law");
        session.undo();
        check(session.document.activeMix().performance.blendLaw == 1 && !session.canUndo(),
              "Upgrade is recoverable in one Undo");
        session.document.activeMix().performance.blendLaw = 2;
        view.refresh();
        check(view.blendEnable.getTitle() == view.blendEnable.getButtonText(),
              "Blend accessible action matches its visible on/off state");
        auto initial = session.document.activeMix().performance.x;
        auto &pad = view.pad;
        juce::Point<float> start{20, 20}, end{90, 90};
        pad.mouseDown(pointer(pad, start, start));
        pad.mouseDrag(pointer(pad, end, start));
        pad.mouseUp(pointer(pad, end, start));
        check(session.canUndo() && session.document.activeMix().performance.x != initial,
              "Actual pad drag edits the control state");
        session.undo();
        check(session.document.activeMix().performance.x == initial && !session.canUndo(),
              "One pad drag has one complete undo step");
        pad.mouseDown(pointer(pad, start, start));
        pad.mouseDrag(pointer(pad, end, start));
        pad.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        pad.mouseUp(pointer(pad, end, start));
        check(session.document.activeMix().performance.x == initial && !session.canUndo(),
              "Escape restores the full pad gesture without an undo entry");
        for (auto *slider :
             std::initializer_list<juce::Slider *>{&view.macros[0], &view.macros[1], &view.macros[2],
                                                   &view.macros[3], &view.xValue, &view.yValue})
        {
            const auto original = mixText(session.document.activeMix());
            const auto value = slider->getValue();
            slider->onDragStart();
            slider->setValue(value < .5 ? .85 : .15, juce::sendNotificationSync);
            view.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            check(mixText(session.document.activeMix()) == original,
                  "Slider Escape restores the complete original state immediately");
            slider->setValue(.95, juce::sendNotificationSync);
            slider->setValue(.05, juce::sendNotificationSync);
            slider->onDragEnd();
            check(mixText(session.document.activeMix()) == original && !session.canUndo(),
                  "Remaining held-slider events and release cannot resurrect a cancelled gesture");
            // JUCE normally repaints/refreshed fields after release; drive the next real gesture.
            view.refresh();
            slider->onDragStart();
            slider->setValue(.4, juce::sendNotificationSync);
            slider->setValue(value < .5 ? .8 : .2, juce::sendNotificationSync);
            slider->onDragEnd();
            const auto changed = mixText(session.document.activeMix());
            check(changed != original && session.canUndo(), "Fresh slider drag works after cancellation");
            session.undo();
            check(mixText(session.document.activeMix()) == original && !session.canUndo(),
                  "Fresh slider drag has one undo step");
            session.redo();
            check(mixText(session.document.activeMix()) == changed, "Fresh slider drag can be redone");
            session.undo();
            auto baseDocument = session.document;
            slider->onDragStart();
            slider->setValue(.37, juce::sendNotificationSync);
            session.cancelGesture();
            session.document = baseDocument;
            session.document.id = juce::Uuid().toString();
            view.refresh();
            const auto replacement = mixText(session.document.activeMix());
            slider->setValue(.91, juce::sendNotificationSync);
            slider->onDragEnd();
            check(mixText(session.document.activeMix()) == replacement && !session.canUndo(),
                  "Late held-slider values cannot edit a replacement document");
            session.document = baseDocument;
            DesignVariation variation;
            variation.name = "Held gesture target";
            variation.mix = baseDocument.activeMix();
            session.document.variations.push_back(variation);
            view.refresh();
            slider->onDragStart();
            slider->setValue(.37, juce::sendNotificationSync);
            session.selectVariation(variation.id);
            const auto candidate = mixText(session.document.activeMix());
            slider->setValue(.91, juce::sendNotificationSync);
            slider->onDragEnd();
            check(mixText(session.document.activeMix()) == candidate,
                  "Actual variation switching rejects the old held slider's values");
            session.undo();
            session.undo();
            session.document = baseDocument;
            view.refresh();
        }
        const auto beforeDrop = mixText(session.document.activeMix());
        const auto layerCount = session.document.activeMix().layers.size();
        auto payload = view.cards[0]->dragPayload.clone();
        const auto sourceId = session.document.activeMix().performance.assignments[0].layer;
        view.itemDropped({payload, view.cards[0].get(), dropPoint(view, 3)});
        check(session.document.activeMix().performance.assignments[3].layer == sourceId &&
                  session.document.activeMix().performance.assignments[0].layer.isEmpty() &&
                  session.document.activeMix().layers.size() == layerCount,
              "Actual card drop moves an existing assignment without duplicate voices");
        session.undo();
        check(mixText(session.document.activeMix()) == beforeDrop && !session.canUndo(),
              "Card move is one complete undo step");
        session.redo();
        check(session.document.activeMix().performance.assignments[3].layer == sourceId,
              "Card move can be redone");
        session.undo();
        view.itemDropped(
            {"SoundPlus layer:" + session.document.activeMix().layers[2].id, &view, dropPoint(view, 1)});
        check(session.document.activeMix().performance.assignments[1].layer ==
                  session.document.activeMix().layers[2].id,
              "Existing stable-layer drag payload remains supported");
        auto swapPayload = view.cards[0]->dragPayload.clone();
        const auto otherId = session.document.activeMix().performance.assignments[1].layer;
        view.itemDropped({swapPayload, view.cards[0].get(), dropPoint(view, 1)});
        check(session.document.activeMix().performance.assignments[1].layer == sourceId &&
                  session.document.activeMix().performance.assignments[0].layer == otherId,
              "Dropping onto an occupied card swaps stable assignments instead of creating a conflict");
        session.undo();
        session.undo();
        DesignDocument captured;
        captured.original.layers = {session.document.activeMix().layers[2]};
        view.itemDropped({juce::JSON::toString(designToJson(captured), true), &view, dropPoint(view, 1)});
        check(session.document.activeMix().layers.size() == layerCount + 1,
              "Actual Library payload creates one ordinary design layer through the existing path");
        session.undo();
        check(mixText(session.document.activeMix()) == beforeDrop && !session.canUndo(),
              "Library card drop and assignment undo together");
        auto documentId = session.document.id;
        session.document.id = juce::Uuid().toString();
        view.refresh();
        view.itemDropped({payload, &view, dropPoint(view, 3)});
        check(mixText(session.document.activeMix()) == beforeDrop && !session.canUndo(),
              "A stale cross-document assignment payload cannot mutate the new design");
        session.document.id = documentId;
        view.refresh();
        auto macroInitial = session.document.activeMix().performance.macros[0].value;
        view.macros[0].onDragStart();
        view.macros[0].setValue(.1, juce::sendNotificationSync);
        view.macros[0].setValue(.9, juce::sendNotificationSync);
        view.macros[0].onDragEnd();
        session.undo();
        check(session.document.activeMix().performance.macros[0].value == macroInitial && !session.canUndo(),
              "Actual macro slider callbacks form one undo step");
        pad.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        check(session.document.activeMix().performance.x > initial,
              "Keyboard alternative reaches the pad model");
        session.undo();
        pad.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0));
        check(std::abs(session.document.activeMix().performance.x - initial - .005) < 1e-10,
              "Shift-arrow performs the advertised fine pad movement");
        session.undo();
        pad.mouseDown(pointer(pad, start, start));
        auto another = session.document;
        another.id = juce::Uuid().toString();
        session.cancelGesture();
        session.document = another;
        view.refresh();
        auto x = session.document.activeMix().performance.x;
        pad.mouseDrag(pointer(pad, end, start));
        pad.mouseUp(pointer(pad, end, start));
        check(session.document.activeMix().performance.x == x,
              "Late pad events cannot edit a replacement document");
        auto oldName = session.document.activeMix().performance.macros[0].name;
        view.macroNames[0].onEditorShow();
        session.document.id = juce::Uuid().toString();
        view.refresh();
        view.macroNames[0].setText("Stale rename", juce::sendNotificationSync);
        check(session.document.activeMix().performance.macros[0].name == oldName,
              "A stale macro-name edit cannot rename a different document");
        session.edit("Delete mapped layer",
                     [](auto &d) { d.activeMix().layers.erase(d.activeMix().layers.begin() + 1); });
        check(view.macroFeedback[0].getText().contains("unresolved"),
              "Deleted targets are visibly unresolved in the actual macro view");
        session.undo();
        for (auto size : {juce::Point<int>(664, 308), juce::Point<int>(752, 360)})
        {
            view.setSize(size.x, size.y);
            auto bounds = view.getLocalBounds();
            check(bounds.contains(view.blendContent.getBounds()) &&
                      bounds.contains(view.macros[3].getBounds()),
                  "Minimum Perform layout contains Blend and macro controls without overflow");
            view.openMapping(0);
            for (auto *c : std::initializer_list<juce::Component *>{&view.mappingChoice, &view.targetChoice,
                                                                    &view.minimum, &view.maximum, &view.curve,
                                                                    &view.addMapping})
                check(bounds.contains(c->getBounds()),
                      "Mapping targets and bounds remain accessible at minimum size");
        }
        auto effective = resolvePerformance(session.document);
        session.saveSnapshot("Static QA snapshot");
        check(session.document.activeVariation.isNotEmpty() && session.document.variations.back().snapshot &&
                  session.document.variations.back().kept,
              "Save Snapshot creates a named kept candidate through the actual session command");
        check(pcmDifference(session.document, effective) == 0,
              "Actual snapshot command recalls its effective sound once");
        session.undo();
        check(session.document.activeVariation.isEmpty(), "Snapshot creation is undoable");
        session.redo();
        check(session.document.variations.back().name == "Static QA snapshot",
              "Snapshot redo retains its name and state");
        session.onChange = {};
    }
};
} // namespace sp
void runPerformanceStateTests(const juce::File &parent, const juce::File &app)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto file = root.getChildFile("tone.wav");
    {
        juce::WavAudioFormat fmt;
        auto stream = file.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> w(
            fmt.createWriterFor(stream.release(), 48000, 2, 32, {}, 0));
        juce::AudioBuffer<float> b(2, 48000);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 48000; ++i)
                b.setSample(c, i, (float)(.1 * std::sin((c + 1) * i * .03)));
        check(w && w->writeFromAudioSampleBuffer(b, 0, 48000), "Finalize generated performance fixture");
    }
    sp::DesignDocument d;
    auto source = sp::inspectDesignSource(file);
    for (int i = 0; i < 5; ++i)
    {
        auto l = sp::makeDesignLayer(source, 0, -1, true);
        l.name = "Layer " + juce::String(i + 1);
        l.inserts.stock = {sp::makeStockEffect("filter"), sp::makeStockEffect("delay"),
                           sp::makeStockEffect("reverb")};
        d.original.layers.push_back(l);
    }
    d.selectedLayer = d.original.layers[0].id;
    auto legacy = sp::designToJson(d);
    check((int)legacy["version"] == 9, "Live insert documents use the authoritative ordered-chain manifest");
    legacy.getDynamicObject()->setProperty("version", 1);
    legacy.getDynamicObject()->setProperty("dsp", "stock-sinc32-v1");
    legacy.getDynamicObject()->setProperty("original", sp::designMixToJson(d.original, true));
    auto old = sp::designFromJson(legacy);
    check(mixText(sp::resolvePerformance(old.original)) == mixText(d.original),
          "Old documents resolve neutrally");
    for (int count = 0; count <= 4; ++count)
    {
        auto mix = d.original;
        for (int i = 0; i < count; ++i)
            sp::assignBlendLayer(mix, i, mix.layers[(size_t)i].id);
        mix.performance.blendEnabled = true;
        for (int x = 0; x <= 10; ++x)
            for (int y = 0; y <= 10; ++y)
            {
                mix.performance.x = x / 10.;
                mix.performance.y = y / 10.;
                auto weights = sp::blendWeights(mix.performance, mix);
                double energy = 0;
                for (auto w : weights)
                {
                    check(std::isfinite(w) && w >= 0 && w <= 1.000000001,
                          "Blend weights are finite bounded amplitudes over the whole pad");
                    energy += w * w;
                }
                check(std::abs(energy - (count ? 1 : 0)) < 1e-10,
                      "Zero to four occupied slots retain normalized power, including missing corners");
                auto resolved = sp::resolvePerformance(mix);
                check(resolved.layers[4].controlGain == 1, "Unassigned layers are unchanged");
                check(mixText(sp::resolvePerformance(resolved)) == mixText(resolved),
                      "Repeated resolution is idempotent");
            }
        auto bypass = mix;
        bypass.performance.blendEnabled = false;
        check(sp::resolvePerformance(bypass).layers[0].controlGain == 1, "Blend bypass returns base levels");
    }
    // Endpoint approaches, every occupancy arrangement, not just bounds on first-N slots.
    for (int mask = 0; mask < 16; ++mask)
    {
        auto mix = d.original;
        for (int i = 0; i < 4; ++i)
            if (mask & (1 << i))
                sp::assignBlendLayer(mix, i, mix.layers[(size_t)i].id);
        mix.performance.blendEnabled = true;
        for (double x : {0., .1, .5, .9, 1.})
            for (double y : {0., .1, .5, .9, 1.})
            {
                mix.performance.x = x;
                mix.performance.y = y;
                const auto at = sp::blendWeights(mix.performance, mix);
                if (mask == 15)
                {
                    auto legacy = mix.performance;
                    legacy.blendLaw = 1;
                    check(at == sp::blendWeights(legacy, mix),
                          "Full four-slot law preserves bilinear sound exactly");
                }
                double power = 0;
                for (int i = 0; i < 4; ++i)
                {
                    check(std::isfinite(at[i]) && at[i] >= 0 && at[i] <= 1 &&
                              ((mask & (1 << i)) || at[i] == 0),
                          "Every occupancy mask has bounded nonnegative occupied weights");
                    power += at[i] * at[i];
                }
                check(std::abs(power - (mask ? 1 : 0)) < 1e-10,
                      "All occupancy masks preserve equal-power normalization");
                for (double distance : {1e-2, 1e-3, 1e-5, 1e-7})
                    for (int direction = 0; direction < 8; ++direction)
                    {
                        const auto angle = direction * juce::MathConstants<double>::pi / 4;
                        auto near = mix.performance;
                        near.x = std::clamp(x + distance * std::cos(angle), 0., 1.);
                        near.y = std::clamp(y + distance * std::sin(angle), 0., 1.);
                        const auto approaching = sp::blendWeights(near, mix);
                        double error = 0;
                        for (int i = 0; i < 4; ++i)
                            error += std::pow(at[i] - approaching[i], 2);
                        const auto step = std::hypot(near.x - x, near.y - y);
                        check(std::sqrt(error) <= 8 * step + 1e-10,
                              "Sparse blend stays continuous from every endpoint/edge approach direction");
                    }
            }
    }
    auto legacyDesign = d;
    legacyDesign.original.performance.blendLaw = 1;
    for (int i = 0; i < 3; ++i)
        sp::assignBlendLayer(legacyDesign.original, i, legacyDesign.original.layers[(size_t)i].id);
    auto &legacyState = legacyDesign.original.performance;
    legacyState.blendEnabled = true;
    legacyState.x = legacyState.y = 1;
    auto legacyWeights = sp::blendWeights(legacyState, legacyDesign.original);
    check(std::abs(legacyWeights[0] - 1 / std::sqrt(3.)) < 1e-12,
          "Legacy saved law retains its exact endpoint behavior, without silent migration");
    auto oldFormat = sp::designToJson(legacyDesign);
    check((int)oldFormat["version"] == 9 && (int)oldFormat["original"]["performance"]["version"] == 1,
          "Unmodified legacy performance state stays compatible with the original manifest");
    auto oldReopened = sp::designFromJson(oldFormat);
    check(oldReopened.original.performance.blendLaw == 1 && pcmDifference(legacyDesign, oldReopened) == 0,
          "Reopening legacy designs preserves the old law and PCM");
    auto upgraded = oldReopened;
    upgraded.original.performance.blendLaw = 2;
    auto newWeights = sp::blendWeights(upgraded.original.performance, upgraded.original);
    check(newWeights[0] == 0 && std::abs(newWeights[1] - 1 / std::sqrt(2.)) < 1e-12 &&
              std::abs(newWeights[2] - newWeights[1]) < 1e-12,
          "New triangular law projects the empty corner onto the occupied opposite edge");
    auto newFormat = sp::designToJson(upgraded);
    check((int)newFormat["version"] == 9 && newFormat["dsp"].toString() == "ordered-inserts-v1",
          "New law has an explicit document/DSP compatibility gate");
    auto newReopened = sp::designFromJson(newFormat);
    check(newReopened.original.performance.blendLaw == 2 && pcmDifference(upgraded, newReopened) == 0,
          "New-law state is deterministic across save/reopen and rendering");
    newFormat.getDynamicObject()->setProperty("version", 2);
    newFormat.getDynamicObject()->setProperty("dsp", "stock-perform-v1");
    rejects([&] { sp::designFromJson(newFormat); }, "Old manifest cannot disguise new-law audio state");
    for (auto &l : legacyDesign.original.layers)
        l.mute = false;
    upgraded.original.performance.blendEnabled = false;
    check(sp::resolvePerformance(upgraded.original).layers[0].controlGain == 1,
          "Updated sparse law bypass restores unchanged base amplitudes");
    auto aliases = d.original;
    sp::assignBlendLayer(aliases, 0, aliases.layers[0].id);
    aliases.performance.assignments[0].name = "Custom role";
    sp::assignBlendLayer(aliases, 0, aliases.layers[2].id);
    check(aliases.performance.assignments[0].name == "Custom role",
          "Replacing the source retains its custom role alias");
    sp::assignBlendLayer(aliases, 0, {});
    sp::assignBlendLayer(aliases, 0, aliases.layers[3].id);
    check(aliases.performance.assignments[0].name == "Custom role",
          "Clearing and choosing again retain a custom alias");
    auto &mix = d.original;
    sp::assignBlendLayer(mix, 0, mix.layers[0].id);
    rejects([&] { sp::assignBlendLayer(mix, 1, mix.layers[0].id); },
            "Duplicate blend assignments are rejected");
    sp::PerformanceMapping gain{{mix.layers[1].id, {}, "gain"}, -30, 0, "linear", false};
    sp::addMacroMapping(mix, 0, gain);
    rejects([&] { sp::addMacroMapping(mix, 1, gain); }, "One macro writer per target");
    rejects([&] { sp::assignBlendLayer(mix, 1, mix.layers[1].id); },
            "Blend cannot take an owned gain target");
    auto &macro = mix.performance.macros[0];
    auto invalid = mix;
    invalid.performance.macros[0].value = 1.01;
    rejects([&] { sp::validatePerformance(invalid); }, "Macro values outside zero to one are rejected");
    auto invalidMap = gain;
    invalidMap.maximum = 25;
    auto isolated = d.original;
    isolated.performance.macros[0].mappings.clear();
    rejects([&] { sp::addMacroMapping(isolated, 0, invalidMap); },
            "Target ranges obey physical parameter bounds");
    check(isolated.performance.macros[0].mappings.empty(), "Rejected mappings leave ownership unchanged");
    check(sp::resolvePerformance(mix).layers[1].gainDb == 0, "Adding a bypassed mapping preserves sound");
    macro.enabled = true;
    macro.value = .25;
    check(sp::resolvePerformance(mix).layers[1].gainDb == -22.5, "Absolute macro range");
    macro.mappings[0].inverted = true;
    check(sp::resolvePerformance(mix).layers[1].gainDb == -7.5, "Macro inversion");
    macro.mappings[0].inverted = false;
    auto cutoff = sp::PerformanceMapping{
        {mix.layers[1].id, mix.layers[1].inserts.stock[0].id, "filterLow"}, 100, 10000, "log", false};
    sp::addMacroMapping(mix, 0, cutoff);
    macro.value = .5;
    check(std::abs(sp::resolvePerformance(mix).layers[1].inserts.stock[0].parameters[1] - 1000) < 1e-8,
          "Frequency interpolation is logarithmic");
    auto structure = sp::designStructure(d);
    mix.performance.x = .8;
    mix.performance.blendEnabled = true;
    macro.value = .8;
    check(sp::designStructure(d) == structure, "Pad/macro values and activation keep the graph signature");
    auto before = d;
    auto effective = sp::resolvePerformance(d);
    check(pcmDifference(d, effective) == 0,
          "Playback initialization uses the exact same effective static state");
    auto live = d;
    sp::applyDesignParameters(live, sp::designParameters(d));
    check(pcmDifference(d, live) == 0, "Live parameter packet resolves exactly once");
    std::reverse(mix.layers.begin(), mix.layers.end());
    auto reordered = sp::resolvePerformance(mix);
    check(std::abs(*sp::performanceTargetValue(reordered, gain.target) + 6) < 1e-10,
          "Reordered layers keep stable target identity");
    std::reverse(mix.layers.begin(), mix.layers.end());
    auto removed = mix;
    removed.layers.erase(removed.layers.begin() + 1);
    check(sp::performanceTargetValue(removed, gain.target) == nullptr,
          "Deleted layer stays unresolved instead of retargeting");
    removed = mix;
    removed.layers[1].inserts.stock.clear();
    check(sp::performanceTargetValue(removed, cutoff.target) == nullptr, "Removed effect stays unresolved");
    removed = mix;
    removed.layers[1].inserts.stock[0].type = "gain";
    check(sp::performanceTargetValue(removed, cutoff.target) == nullptr,
          "An incompatible effect with the same ID cannot acquire a cutoff mapping");
    auto manual = mix;
    manual.layers[1].gainDb = -9;
    sp::unlinkManuallyEditedTargets(mix, manual);
    check(!sp::performanceOwns(manual, gain.target) && sp::performanceOwns(manual, cutoff.target),
          "Manual parameter edit unlinks only its own mapping");
    auto saved = root.getChildFile("performance.soundplus");
    sp::saveDesignAtomic(saved, d);
    auto recall = sp::loadDesign(saved);
    check((int)sp::designToJson(recall)["version"] == 9,
          "Performance documents use the explicit new compatibility gate");
    check(pcmDifference(d, recall) == 0, "Save/reopen retains effective sound");
    auto forged = sp::designToJson(d);
    forged.getDynamicObject()->setProperty("version", 1);
    rejects([&] { sp::designFromJson(forged); }, "Older version cannot silently accept performance state");
    sp::DesignVariation snapshot;
    snapshot.name = "Snapshot";
    snapshot.snapshot = true;
    snapshot.kept = true;
    snapshot.mix = d.activeMix();
    d.variations.push_back(snapshot);
    d.activeVariation = snapshot.id;
    check(pcmDifference(d, before) == 0, "Snapshot base-plus-controls recall does not double-apply");
    auto smooth = d;
    smooth.activeMix().performance.macros[0].enabled = false;
    sp::DesignGraph graph(smooth);
    juce::AudioBuffer<float> block(2, 512);
    graph.render(block, 0, 512);
    auto packet = sp::designParameters(d);
    graph.updateParameters(packet);
    graph.render(block, 0, 512);
    check(block.getMagnitude(0, 512) < 1 && std::isfinite(block.getMagnitude(0, 512)),
          "Continuous smoothed gain/filter updates remain finite and bounded");
    sp::DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.submit = [](auto) { return true; };
    std::mutex completedMutex;
    std::deque<std::function<void()>> completed;
    services.post = [&](auto callback)
    {
        std::lock_guard lock(completedMutex);
        completed.push_back(std::move(callback));
    };
    services.resolve = [](auto &) {};
    services.status = [] { return sp::message("status"); };
    services.stop = [] {};
    {
        sp::DesignSession session(root.getChildFile("gesture.sqlite"), services);
        session.document = before;
        session.selectedIds = {before.selectedLayer};
        sp::PerformanceViewTestAccess::run(session);
        session.document = before;
        session.edit("Inspector gain", [](auto &d) { d.activeMix().layers[1].gainDb = -9; });
        check(!sp::performanceOwns(session.document.activeMix(), gain.target),
              "Actual inspector-style session edits unlink the owning macro target");
        session.undo();
        check(sp::performanceOwns(session.document.activeMix(), gain.target),
              "Undo restores the linked target and its prior base value");
        auto selectedFixture = before;
        for (int i = 1; i < 4; ++i)
            sp::assignBlendLayer(selectedFixture.original, i, selectedFixture.original.layers[i + 1].id);
        session.document = selectedFixture;
        session.document.outputFolder = root.getChildFile("Selected renders").getFullPathName();
        session.document.renderSettings.bits = 32;
        session.document.renderSettings.tailSeconds = .1;
        session.render(true);
        auto deadline = sp::monotonicMs() + 10000;
        while (session.rendering && sp::monotonicMs() < deadline)
        {
            std::function<void()> callback;
            {
                std::lock_guard lock(completedMutex);
                if (!completed.empty())
                {
                    callback = std::move(completed.front());
                    completed.pop_front();
                }
            }
            if (callback)
                callback();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(!session.rendering && session.lastOutput.existsAsFile(),
              "Actual Render selected layer command finalizes a performance output");
        auto reference = selectedFixture;
        reference.variations.clear();
        reference.activeVariation.clear();
        reference.original.performance = {};
        auto primary = selectedFixture.original.layers.front();
        primary.placement = 0;
        primary.mute = primary.solo = false;
        auto x = selectedFixture.original.performance.x, y = selectedFixture.original.performance.y;
        auto a = (1 - x) * (1 - y), b = x * (1 - y), c = (1 - x) * y, e = x * y;
        primary.controlGain = a / std::sqrt(a * a + b * b + c * c + e * e);
        reference.original.layers = {primary};
        sp::DesignReader expected(reference, 48000, 0, -1, .1);
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rendered(formats.createReaderFor(session.lastOutput));
        check(rendered && rendered->lengthInSamples == expected.length(),
              "Selection contains only the primary layer and its explicit tail");
        juce::AudioBuffer<float> actual(2, (int)rendered->lengthInSamples), block(2, 512);
        rendered->read(&actual, 0, actual.getNumSamples(), 0, true, true);
        double error = 0;
        int offset = 0;
        while (int n = expected.read(block, 512))
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    error = std::max(
                        error, (double)std::abs(actual.getSample(ch, offset + i) - block.getSample(ch, i)));
            offset += n;
        }
        check(error < 1e-7, "Primary-layer printing keeps its full-design blend weight instead of "
                            "renormalizing a single slot");
    }
    // Native/offline parity also exercises the new empty-corner triangle law.
    auto &nativeMix = d.activeMix();
    sp::assignBlendLayer(nativeMix, 1, nativeMix.layers[2].id);
    sp::assignBlendLayer(nativeMix, 2, nativeMix.layers[3].id);
    nativeMix.performance.blendEnabled = true;
    nativeMix.performance.x = nativeMix.performance.y = 1;
    // Actual permanent export and supervised native producer, from the same initial history.
    d.renderSettings.tailSeconds = .1;
    d.renderSettings.bits = 32;
    d.renderSettings.normalize = false;
    d.renderSettings.dither = false;
    auto output = sp::renderDesign(
        d, root.getChildFile("Renders"), d.renderSettings, [] { return false; }, [](double) {});
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> wav(formats.createReaderFor(juce::File(output.path)));
    check(wav != nullptr, "Finalized performance render exists");
    juce::AudioBuffer<float> offline(2, (int)wav->lengthInSamples);
    wav->read(&offline, 0, offline.getNumSamples(), 0, true, true);
    auto snapshotFile = root.getChildFile("native-performance.soundplus");
    sp::saveDesignAtomic(snapshotFile, d);
    auto capture = root.getChildFile("native-pre-monitor.f32");
    sp::AudioClient audio(app);
    auto wait = [&](auto condition, const char *why)
    {
        double deadline = sp::monotonicMs() + 15000;
        while (!condition())
        {
            if (sp::monotonicMs() > deadline)
                throw std::runtime_error(why);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ++checks;
    };
    wait([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native performance helper device ready");
    audio.deviceRate(48000);
    wait([&] { return (double)audio.status()["sampleRate"] == 48000; }, "Native performance device at 48k");
    audio.gain(.13f);
    audio.playDesign("design:" + d.id, snapshotFile, 0, capture);
    wait([&]
         { return audio.status()["id"].toString() == "design:" + d.id && (bool)audio.status()["playing"]; },
         "Version-3 performance design plays through the real helper");
    wait([&] { return !(bool)audio.status()["playing"] && !(bool)audio.status()["pendingPlayback"]; },
         "Native performance reaches its finalized tail");
    juce::MemoryBlock bytes;
    check(capture.loadFileAsData(bytes) && bytes.getSize() == (size_t)offline.getNumSamples() * 8,
          "Native performance capture has the exact render length");
    double nativeError = 0;
    auto *samples = (const float *)bytes.getData();
    for (int i = 0; i < offline.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c)
            nativeError =
                std::max(nativeError, (double)std::abs(samples[i * 2 + c] - offline.getSample(c, i)));
    check(nativeError < 1e-7, "Performance live/offline PCM agrees before monitor gain");
    auto rampDoc = before;
    rampDoc.activeVariation.clear();
    rampDoc.variations.clear();
    rampDoc.original.performance = {};
    rampDoc.original.layers.resize(1);
    rampDoc.original.layers[0].inserts.stock.clear();
    rampDoc.original.masterDb = 0;
    sp::DesignGraph ramp(rampDoc), unchanged(rampDoc);
    juce::AudioBuffer<float> moving(2, 512), steady(2, 512);
    ramp.render(moving, 0, 128);
    unchanged.render(steady, 0, 128);
    rampDoc.original.layers[0].gainDb = -18;
    ramp.updateParameters(sp::designParameters(rampDoc));
    ramp.render(moving, 0, 512);
    unchanged.render(steady, 0, 512);
    check(moving.getSample(0, 0) / steady.getSample(0, 0) > .9 &&
              std::abs(moving.getSample(0, 511) / steady.getSample(0, 511) - std::pow(10., -18. / 20)) < 1e-6,
          "Continuous gain motion ramps from the old value and settles instead of stepping");
    auto report = sp::message("performance-test");
    report.getDynamicObject()->setProperty("checks", checks);
    report.getDynamicObject()->setProperty("nativePreMonitorError", nativeError);
    report.getDynamicObject()->setProperty("monitorGain", .13);
    report.getDynamicObject()->setProperty("nativeStatus", audio.status());
    report.getDynamicObject()->setProperty("directory", root.getFullPathName());
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(report, true));
    audio.stop();
    std::cout
        << "PASS performance neutral compatibility, 0-4 blend, stable mappings, idempotence and PCM recall: "
        << checks << " checks\n";
}
