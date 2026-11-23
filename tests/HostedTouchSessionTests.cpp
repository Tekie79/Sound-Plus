#include "DesignSession.h"
#include "HostedRackView.h"
#include "AutomationEditor.h"
#include "HostedTouchCapture.h"
#include "Performance.h"
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include "TrackModel.h"
#include <deque>
#include <iostream>
namespace sp
{
struct HostedAutomationTestAccess
{
    static void playing(DesignSession &s)
    {
        s.active = true;
        s.previewWaiting = false;
        s.expectedGeneration = 73;
    }
    static void receive(DesignSession &s, LiveInsertSession::Capture c)
    {
        s.receiveHostedTouches(c);
    }
    static void ack(DesignSession &s, uint64_t id)
    {
        s.acknowledgeRetainedTouch("test", "other-generation", id);
    }
    static size_t history(const DesignSession &s)
    {
        return s.undoHistory.size();
    }
};
} // namespace sp
namespace
{
using namespace sp;
using namespace juce;
int checks = 0;
void check(bool b, const char *why)
{
    ++checks;
    if (!b)
        throw std::runtime_error(why);
}
var obj()
{
    return var(new DynamicObject);
}
void put(const var &v, const char *key, var x)
{
    v.getDynamicObject()->setProperty(key, x);
}
Slider *gainSlider(Component &c)
{
    if (auto *s = dynamic_cast<Slider *>(&c); s && s->getTitle().startsWith("External insert / Gain"))
        return s;
    for (auto *child : c.getChildren())
        if (auto *s = gainSlider(*child))
            return s;
    return nullptr;
}
} // namespace
#include "HostedTouchDeviceTests.inc"
void runHostedTouchSessionTests(const juce::File &base, const juce::File &app, const juce::File &fixture)
{
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    auto scan = obj();
    put(scan, "mode", "scan");
    put(scan, "path", fixture.getFullPathName());
    auto plugin = worker.call(scan, root, 20000)["plugins"][0];
    auto settings = obj();
    put(settings, "mode", "describe");
    put(settings, "plugin", plugin);
    put(settings, "rate", 48000);
    put(settings, "block", 512);
    put(settings, "layout", layoutJson(channelLayout("mono-v1")));
    auto described = worker.call(settings, root, 20000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
        put(settings, key, described[key]);
    var gain;
    for (auto &p : *settings["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            gain = p;
    std::deque<std::function<void()>> work, posts;
    std::vector<DesignDocument> retained;
    bool failRetention = true;
    int acknowledgements = 0;
    std::vector<std::tuple<String, String, int, float>> commands;
    DesignSession *current = nullptr;
    DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.submit = [&](auto job)
    {
        work.push_back(std::move(job));
        return true;
    };
    services.post = [&](auto job) { posts.push_back(std::move(job)); };
    services.now = [] { return 1000.; };
    services.resolve = [](auto &) {};
    services.status = [&]
    {
        auto state = obj();
        put(state, "id", current ? "design:" + current->document.id : String());
        put(state, "playing", true);
        put(state, "playGeneration", 73);
        put(state, "sourceSample", 200);
        return state;
    };
    services.parameters = [](auto &, auto, auto) {};
    services.stop = [] {};
    services.retain = [&](auto &d, auto &, bool)
    {
        if (failRetention)
            throw std::runtime_error("Injected retention failure");
        retained.push_back(d);
    };
    services.hostedGesture = [&](auto &slot, auto &parameter, auto &owner, int kind, float value)
    {
        commands.push_back({parameter, owner, kind, value});
        return true;
    };
    services.hostedTouchAck = [&](auto &, auto &, uint64_t) { ++acknowledgements; };
    auto drain = [&]
    {
        while (!work.empty() || !posts.empty())
        {
            if (!work.empty())
            {
                auto f = std::move(work.front());
                work.pop_front();
                f();
            }
            if (!posts.empty())
            {
                auto f = std::move(posts.front());
                posts.pop_front();
                f();
            }
        }
    };
    DesignSession session(root.getChildFile("catalog.sqlite"), services);
    current = &session;
    auto &d = session.document;
    d.original.layout = channelLayout("mono-v1");
    d.original.duration = 10000;
    auto owner = addTrack(d.original, TrackKind::audio);
    auto *track = findTrack(d.original, owner);
    track->layout = d.original.layout;
    HostedSlot slot{Uuid().toString(), JSON::toString(settings, true), false};
    track->inserts.hosted.slots = {slot};
    AutomationTarget target{slot.id, gain["stableId"].toString()};
    addAutomationPoint(d.original, owner, target, 0, .25, 48000);
    addAutomationPoint(d.original, owner, {{}, "volume"}, 0, -6, 48000);
    auto untouched = *automationCurve(track->automation, {{}, "volume"});
    track->automation.touch = true;
    track->automation.armed = target;
    track->automation.shown = target;
    d.selectedTrack = owner;
    HostedAutomationTestAccess::playing(session);
    // Durable generated document for separate native pointer/keyboard inspection.
    auto native = d;
    native.name = "Hosted automation QA";
    native.original.duration = 48000 * 120;
    native.original.masterDb = 0;
    auto source = root.getChildFile("Hosted source.wav");
    {
        WavAudioFormat format;
        auto stream = source.createOutputStream();
        std::unique_ptr<AudioFormatWriter> writer(
            format.createWriterFor(stream.release(), 48000, 1, 32, {}, 0));
        juce::AudioBuffer<float> buffer(1, 48000);
        for (int n = 0; n < 48000; ++n)
            buffer.setSample(0, n, .08f * std::sin(n * .031f));
        for (int n = 0; n < 120; ++n)
            check(writer->writeFromAudioSampleBuffer(buffer, 0, 48000), "Generate finalized native fixture");
    }
    auto layer = makeDesignLayer(inspectDesignSource(source), 0, -1, true);
    initialiseRouting(layer, native.original.layout);
    layer.trackId = owner;
    native.original.layers = {layer};
    native.selectedLayer = layer.id;
    native.selectedTrack = owner;
    findTrack(native.original, owner)->automation.expanded = true;
    addTrack(native.original, TrackKind::groupBus);
    addTrack(native.original, TrackKind::midiInstrument);
    auto nativeFile = root.getChildFile("Native hosted automation.soundplus");
    saveDesignAtomic(nativeFile, native);
    auto evidence = obj();
    put(evidence, "file", nativeFile.getFullPathName());
    put(evidence, "root", root.getFullPathName());
    base.getChildFile("latest-native.json").replaceWithText(JSON::toString(evidence, true));
    {
        HostedRackView view(session, false, root.getChildFile("registry.json"), true, {}, {}, false);
        view.bind(owner);
        view.selectSlot(slot.id);
        view.refresh();
        auto *slider = gainSlider(view);
        check(slider != nullptr, "Actual hosted view creates Gain control");
        slider->onDragStart();
        track->automation.shown = {{}, "volume"};
        slider->setValue(.7, sendNotificationSync);
        slider->onDragEnd();
        check(commands.size() == 3 && std::get<2>(commands[0]) == 0 && std::get<2>(commands[1]) == 1 &&
                  std::get<2>(commands[2]) == 2,
              "Actual inline view emits one captured Begin/value/End sequence");
        for (auto &command : commands)
            check(std::get<0>(command) == target.parameter && std::get<1>(command) == owner,
                  "Displayed-target change cannot redirect inline Touch");
    }
    commands.clear();
    TimelineViewport timeline;
    SnapSettings snap;
    AutomationEditor editor(session, owner, timeline, snap);
    editor.beginTouch();
    editor.touchValue(.6);
    editor.endTouch();
    check(commands.size() == 3 && std::get<0>(commands[1]) == target.parameter,
          "Existing one-lane Touch value control uses acknowledged hosted recorder");
    auto capture = LiveInsertSession::Capture();
    capture.document = d.id;
    capture.variation = d.activeVariation;
    capture.incarnation = session.documentGeneration();
    capture.owner = owner;
    capture.slot = slot.id;
    capture.generation = "owned-generation";
    capture.settings = slot.settings;
    capture.expected = slot.settings;
    capture.evidence = obj();
    auto binding = obj();
    put(binding, "source", "");
    put(binding, "instrument", "");
    put(capture.evidence, "binding", binding);
    auto pass = obj();
    put(pass, "id", 1);
    put(pass, "owner", owner);
    put(pass, "index", gain["index"]);
    put(pass, "complete", false);
    put(pass, "end", 240);
    put(pass, "returnEnd", 5040);
    put(pass, "returnValue", .25);
    Array<var> points;
    for (auto [frame, value] : {std::pair{100, .7}, std::pair{200, .3}})
    {
        auto p = obj();
        put(p, "frame", frame);
        put(p, "value", value);
        points.add(p);
    }
    put(pass, "points", points);
    put(capture.evidence, "touchPasses", Array<var>{pass});
    auto history = HostedAutomationTestAccess::history(session);
    HostedAutomationTestAccess::receive(session, capture);
    check(HostedAutomationTestAccess::history(session) == history && acknowledgements == 0,
          "Active native pass cannot claim completed retention");
    put(capture.evidence["touchPasses"][0], "complete", true);
    HostedAutomationTestAccess::receive(session, capture);
    check(HostedAutomationTestAccess::history(session) == history + 1,
          "Completed native pass is one Undo transaction");
    check(acknowledgements == 0, "Model mutation alone does not acknowledge durable retention");
    HostedAutomationTestAccess::receive(session, capture);
    check(HostedAutomationTestAccess::history(session) == history + 1,
          "Duplicate publication cannot duplicate native pass or Undo");
    drain();
    check(acknowledgements == 0 && retained.empty(),
          "Injected storage failure retains helper checkpoint instead of claiming success");
    failRetention = false;
    HostedAutomationTestAccess::receive(session, capture);
    drain();
    check(acknowledgements == 1 && retained.size() == 1,
          "Successful recovery write acknowledges actual durable retention exactly once");
    auto *curve = automationCurve(*automationLane(d.original, owner), target);
    check(curve && curve->hostedBinding && curve->points.size() >= 5,
          "Native target and complete100ms return curve retained");
    auto before = JSON::toString(automationLaneJson(*automationLane(d.original, owner)), true);
    check(automationCurve(*automationLane(d.original, owner), {{}, "volume"})->points[0].value ==
              untouched.points[0].value,
          "Unrelated hidden curve remains unchanged");
    HostedAutomationTestAccess::receive(session, capture);
    drain();
    check(acknowledgements == 1, "Late duplicate completion remains ignored after acknowledgement");
    session.undo();
    check(HostedAutomationTestAccess::history(session) == history, "Undo removes one complete pass");
    session.redo();
    check(JSON::toString(automationLaneJson(*automationLane(d.original, owner)), true) == before,
          "Redo restores complete native pass and independent display state");
    auto interrupted = capture;
    interrupted.evidence = capture.evidence.clone();
    put(interrupted.evidence["touchPasses"][0], "id", 2);
    put(interrupted.evidence["touchPasses"][0], "complete", false);
    put(interrupted.evidence["touchPasses"][0]["points"][0], "value", .9);
    put(interrupted.evidence, "touchFrame", (juce::int64)512);
    interrupted.evidence =
        HostedTouchCapture::interruptedCheckpoint(interrupted.evidence, "Owned helper failure");
    interrupted.touchOnly = true;
    HostedAutomationTestAccess::receive(session, interrupted);
    drain();
    check(automationCurve(*automationLane(d.original, owner), target)->points[1].value == .9 &&
              acknowledgements == 2,
          "Acknowledged crash checkpoint is actually retained before pruning");
    // Removing an insert mid-gesture must preserve an unavailable curve, never another processor.
    put(capture.evidence["touchPasses"][0], "id", 3);
    put(capture.evidence["touchPasses"][0], "complete", false);
    HostedAutomationTestAccess::receive(session, capture);
    session.edit("Remove external insert",
                 [&](auto &document) { findTrack(document.original, owner)->inserts.hosted.slots.clear(); });
    put(capture.evidence["touchPasses"][0], "complete", true);
    HostedAutomationTestAccess::receive(session, capture);
    drain();
    curve = automationCurve(*automationLane(d.original, owner), target);
    check(curve && curve->hostedBinding && !curve->read && curve->hostedBinding->slot == slot.id,
          "Removed plugin's pass remains unavailable with its original identity");
    check(designFromJson(designToJson(d)).id == d.id,
          "Unavailable recorded curve remains Save/Reopen compatible");
    auto replacement = slot;
    replacement.id = Uuid().toString();
    session.edit("Add replacement", [&](auto &document)
                 { findTrack(document.original, owner)->inserts.hosted.slots = {replacement}; });
    auto *lane = automationLane(d.original, owner);
    lane->shown = target;
    editor.refresh();
    AutomationTarget destination{replacement.id, target.parameter};
    auto oldBinding = automationCurve(*lane, target)->hostedBinding;
    auto original = JSON::toString(automationLaneJson(*lane), true);
    check(!editor.repairHostedTarget(target, destination, false, oldBinding) &&
              JSON::toString(automationLaneJson(*lane), true) == original,
          "Repair never retargets without explicit confirmation");
    check(editor.repairHostedTarget(target, destination, true, oldBinding),
          "Actual view repair command adopts explicitly selected exact binding");
    auto *repaired = automationCurve(*automationLane(d.original, owner), destination);
    check(repaired && repaired->hostedBinding && repaired->hostedBinding->slot == replacement.id &&
              !automationCurve(*automationLane(d.original, owner), target),
          "Confirmed repair preserves points under new stable UUID without leaving a second lane");
    session.undo();
    check(automationCurve(*automationLane(d.original, owner), target) != nullptr,
          "Undo restores unresolved original binding after repair");
    auto ackBefore = acknowledgements;
    HostedAutomationTestAccess::ack(session, 2);
    check(acknowledgements == ackBefore, "Later retention cannot prune an earlier failed checkpoint");
    HostedAutomationTestAccess::ack(session, 1);
    check(acknowledgements == ackBefore + 1, "Only contiguous durable passes advance cumulative ACK");
    auto generation = session.documentGeneration();
    session.newDesign();
    drain();
    check(session.documentGeneration() != generation,
          "Actual document transition invalidates prior callback lifetime");
    auto next = JSON::toString(designToJson(session.document), true);
    HostedAutomationTestAccess::receive(session, capture);
    drain();
    check(JSON::toString(designToJson(session.document), true) == next,
          "Stale native callback cannot modify replacement document");
    if (SystemStats::getEnvironmentVariable("SOUNDPLUS_QA_TOUCH_DEVICE", {}).isNotEmpty())
        runHostedTouchDeviceTests(native, root, app, owner, target);
    std::cout << "Hosted Touch session: " << checks
              << " view, retention, target, Undo and stale-callback checks passed\n";
}
