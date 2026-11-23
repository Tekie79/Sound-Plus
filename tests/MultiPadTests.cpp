#include "PerformView.h"
#include "TrackModel.h"
#include "DesignLive.h"
#include "AudioStream.h"
#include "Performance.h"
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
namespace
{
using namespace sp;
using namespace juce;
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f, const char *why)
{
    bool failed = false;
    try
    {
        f();
    }
    catch (...)
    {
        failed = true;
    }
    check(failed, why);
}
AudioBuffer<float> pcm(const DesignDocument &d, int partition, int frames = 1024)
{
    DesignGraph graph(d);
    AudioBuffer<float> result(1, frames);
    result.clear();
    for (int n = 0; n < frames; n += partition)
        graph.render(result, n, std::min(partition, frames - n));
    return result;
}
double difference(const AudioBuffer<float> &a, const AudioBuffer<float> &b)
{
    double result = 0;
    for (int n = 0; n < a.getNumSamples(); ++n)
        result = std::max(result, (double)std::abs(a.getSample(0, n) - b.getSample(0, n)));
    return result;
}
DesignDocument fixture(const File &root)
{
    root.createDirectory();
    DesignDocument d;
    d.name = "Four-pad shared clock QA";
    d.original.layout = channelLayout("mono-v1");
    d.original.masterDb = 0;
    d.original.duration = 48000 * 8;
    auto owner = addTrack(d.original, TrackKind::audio);
    for (int i = 0; i < 4; ++i)
    {
        auto file = root.getChildFile("Pad " + String::charToString((juce_wchar)('A' + i)) + ".wav");
        {
            WavAudioFormat format;
            auto stream = file.createOutputStream();
            std::unique_ptr<AudioFormatWriter> writer(
                format.createWriterFor(stream.release(), 48000, 1, 32, {}, 0));
            AudioBuffer<float> block(1, 48000);
            for (int n = 0; n < 48000; ++n)
                block.setSample(0, n, .05f * (i + 1));
            for (int n = 0; n < 8; ++n)
                check(writer->writeFromAudioSampleBuffer(block, 0, 48000), "Finalize generated pad source");
        }
        auto layer = makeDesignLayer(inspectDesignSource(file), 0, -1, true);
        initialiseRouting(layer, d.original.layout);
        layer.trackId = owner;
        layer.fadeIn = layer.fadeOut = 0;
        d.original.layers.push_back(layer);
    }
    d.selectedLayer = d.original.layers[0].id;
    d.selectedTrack = owner;
    assignBlendLayer(d.original, 0, d.original.layers[0].id);
    d.original.performance.blendEnabled = true;
    return d;
}
} // namespace
namespace sp
{
struct MultiPadTestAccess
{
    static void level(PerformView &view, int phase, double value)
    {
        if (phase == 0)
            view.padLevel.onDragStart();
        view.padLevel.setValue(value, juce::sendNotificationSync);
        if (phase == 2)
            view.padLevel.onDragEnd();
    }
    static void tab(PerformView &view, int pad)
    {
        view.padTabs[(size_t)pad]->onClick();
    }
    static void view(DesignSession &session)
    {
        PerformView view(session);
        int forwarded = 0;
        view.onCommand = [&](const juce::KeyPress &key)
        {
            ++forwarded;
            return key.getModifiers().isCommandDown() || key.isKeyCode(juce::KeyPress::spaceKey);
        };
        check(view.keyPressed(juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0)) &&
                  view.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)) && forwarded == 2,
              "Perform commands and transport route to the shared Designer owner");
        view.setSize(1000, 500);
        session.onChange = [&] { view.refresh(); };
        view.refresh();
        auto initial = designStructure(session.document);
        auto a = session.document.original.performance.x;
        view.padTabs[1]->onClick();
        auto foreground = view.foregroundSources();
        check(foreground.size() == 1 && foreground[0].path == session.document.original.layers[1].source.path,
              "Waveform worker demand follows the edited Pad B rather than always Pad A");
        auto &mix = session.document.original;
        const AutomationTarget target{padIdentity(mix, 1), "level"};
        addAutomationPoint(mix, "master", target, 0, -3., session.document.sampleRate);
        auto curveBefore = *automationCurve(mix.masterAutomation, target);
        view.padAction(3, 1);
        check(!automationCurve(session.document.original.masterAutomation, target)->read,
              "Actual Read pad menu command disables its own retained curves");
        session.undo();
        check(automationCurve(session.document.original.masterAutomation, target)->read &&
                  automationCurve(session.document.original.masterAutomation, target)->points[0].id ==
                      curveBefore.points[0].id,
              "Read menu Undo retains original points and restores playback");
        view.padAction(3, 1);
        view.padAction(3, 1);
        check(automationCurve(session.document.original.masterAutomation, target)->read,
              "Read pad menu command reenables the same curves");
        session.document.original.masterAutomation.curves.clear();
        view.padLevel.onDragStart();
        view.padLevel.setValue(-9, sendNotificationSync);
        view.padLevel.onDragEnd();
        check(session.document.performPad == 1 &&
                  std::abs(padState(session.document.original, 1).levelDb + 9) < 1e-8 &&
                  session.document.original.performance.levelDb == 0,
              "Actual tab/level edits only the selected pad");
        session.undo();
        check(padState(session.document.original, 1).levelDb == 0, "One Undo restores pad level");
        session.redo();
        view.padTabs[0]->onClick();
        check(session.document.original.performance.x == a && designStructure(session.document) == initial,
              "Tabs preserve all pads and shared graph identity");
        view.setSize(690, 352);
        view.refresh();
        for (int p = 0; p < 4; ++p)
            check(view.blendBounds.contains(view.padTabs[(size_t)p]->getBounds()),
                  "All real pad tabs remain within minimum workspace");
        check(view.macroArea.contains(view.padLevel.getBounds()) &&
                  view.macroArea.contains(view.padActions.getBounds()),
              "Pad level/actions remain in compact macro sidebar");
        session.onChange = {};
    }
};
} // namespace sp
#include "MultiPadDeviceTests.inc"
void runMultiPadTests(const juce::File &base, const juce::File &app)
{
    using namespace sp;
    using namespace juce;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto d = fixture(root);
    auto legacy = JSON::toString(designToJson(d), true);
    auto legacyPCM = pcm(d, 512);
    check(!hasMultiPad(d) && !designToJson(d).hasProperty("performPad"),
          "Legacy Pad A omits all new policy/editor fields");
    addPerformancePad(d);
    check(d.performPad == 1 && d.original.performancePads.size() == 1,
          "Adjacent Add creates Pad B and selects editor only");
    check(difference(legacyPCM, pcm(d, 512)) < 1e-7, "Adding empty Pad B preserves original Pad A PCM");
    assignBlendLayer(d.original, 0, d.original.layers[1].id, 1);
    padState(d.original, 1).blendEnabled = true;
    addPerformancePad(d);
    assignBlendLayer(d.original, 0, d.original.layers[2].id, 2);
    padState(d.original, 2).blendEnabled = true;
    addPerformancePad(d);
    assignBlendLayer(d.original, 0, d.original.layers[3].id, 3);
    padState(d.original, 3).blendEnabled = true;
    rejects([&] { addPerformancePad(d); }, "A fifth pad cannot be created");
    rejects([&] { assignBlendLayer(d.original, 1, d.original.layers[1].id, 2); },
            "A source cannot acquire conflicting pad ownership");
    validateDesign(d);
    check((int)designToJson(d)["version"] == 18, "Multi-pad has an explicit manifest18 gate");
    auto raw = pcm(d, 512);
    for (int n = 0; n < 1024; ++n)
        check(std::abs(raw.getSample(0, n) - .5) < 1e-6, "Independent four-source sum oracle");
    auto copy = d;
    padState(copy.original, 1).levelDb = -6;
    raw = pcm(copy, 61);
    for (int n = 0; n < 1024; ++n)
        check(std::abs(raw.getSample(0, n) - (.4 + .1 * std::pow(10., -6. / 20))) < 1e-6,
              "Independent per-pad level oracle");
    padState(copy.original, 1).mute = true;
    check(std::abs(pcm(copy, 257).getSample(0, 1000) - .4) < 1e-6, "Pad mute affects only owned sources");
    padState(copy.original, 3).solo = true;
    check(std::abs(pcm(copy, 32).getSample(0, 1000) - .2) < 1e-6, "Pad solo is independent from editing tab");
    for (int p = 0; p < 4; ++p)
    {
        auto stem = padRenderDocument(d, p);
        auto signal = pcm(stem, 61);
        for (int n = 0; n < 1024; ++n)
            check(std::abs(signal.getSample(0, n) - .05 * (p + 1)) < 1e-6,
                  "Per-pad stem oracle excludes all other pad sources");
    }
    auto targets = automationTargets(d.original, "master");
    check(targets[0].target.parameter == "volume" && targets.size() >= 57,
          "One Master lane remains Volume-first and exposes all pad parameters");
    auto level = AutomationTarget{padIdentity(d.original, 1), "level"};
    for (auto [frame, value] :
         {std::pair{0, 0.}, std::pair{7, -6.}, std::pair{61, -12.}, std::pair{257, -3.}})
        addAutomationPoint(d.original, "master", level, frame, value, 48000);
    auto reference = pcm(d, 512);
    for (int partition : {1, 32, 61, 257})
        check(difference(reference, pcm(d, partition)) < 1e-7,
              "Hidden pad curves use partition-invariant anchored source clock plus exact keyframes");
    for (double rate : {44100., 96000.})
    {
        auto converted = d;
        converted.sampleRate = rate;
        auto expected = pcm(converted, 512);
        for (int partition : {1, 32, 61, 257})
            check(difference(expected, pcm(converted, partition)) < 1e-6,
                  "Multi-pad clock/grid remains partition invariant at44.1/96k");
    }
    auto file = root.getChildFile("Four pads.soundplus");
    saveDesignAtomic(file, d);
    check(difference(reference, pcm(loadDesign(file), 61)) == 0, "Save/Reopen retains all pad PCM");
    auto forged = designToJson(d);
    forged.getDynamicObject()->setProperty("version", 17);
    forged.getDynamicObject()->setProperty("dsp", "hosted-automation-v1");
    rejects([&] { designFromJson(forged); }, "Old manifests cannot disguise new pad processing");
    auto controls = mixControls(d.original);
    check(controlsFromJson(controlsToJson(controls)) == controls,
          "Bounded typed multi-pad controls round trip without cross-pad loss");
    auto source = d;
    disablePadAutomation(source.original, 0);
    auto first = freezeTake(source, "Pad A");
    first.generation = 7;
    hashTake(first);
    TakeCapture capture(first);
    auto moving = mixControls(source.original);
    moving.x = .1;
    check(capture.append(0, moving), "First pad event may begin at source frame zero");
    moving.x = .9;
    capture.append(128, moving);
    capture.finish(1024, true);
    first = capture.snapshot();
    check((int)takeToJson(first)["recipe"]["recipeVersion"] == 9 &&
              takeFromJson(takeToJson(first)).recipeHash == first.recipeHash,
          "Recipe9 freezes complete multi-pad identity/curves and reopens immutably");
    auto full = d.original;
    full.masterAutomation.curves.clear();
    for (int i = 0; i < 4; ++i)
    {
        AutomationCurve curve;
        curve.target = {"reserved:" + String(i), "future"};
        curve.read = false;
        for (int n = 0; n < 2048; ++n)
            curve.points.push_back({Uuid().toString(), n, 0, true});
        full.masterAutomation.curves.push_back(std::move(curve));
    }
    validateTracks(full, 48000);
    auto fullBefore = JSON::toString(automationLaneJson(full.masterAutomation), true);
    rejects([&] { retainPadTake(full, 0, first); },
            "Editable point capacity fails explicitly without discarding the complete frozen take");
    check(JSON::toString(automationLaneJson(full.masterAutomation), true) == fullBefore &&
              takeFromJson(takeToJson(first)).complete,
          "Capacity failure leaves old curves and complete take intact");
    retainPadTake(d.original, 0, first);
    auto preservedA = d.original;
    check(!automationCurve(d.original.masterAutomation, {padIdentity(d.original, 0), "mute"}),
          "Recording movement does not create unchanged mute/solo curves");
    auto secondSeed = d;
    disablePadAutomation(secondSeed.original, 1);
    auto second = freezeTake(secondSeed, "Pad B overdub");
    second.generation = 8;
    hashTake(second);
    auto cap = std::make_shared<TakeCapture>(second);
    cap->phase = TakeCapture::recording;
    secondSeed.takeProcessing = true;
    PcmStream stream(secondSeed, 48000, 0);
    stream.startTake(cap);
    moving = mixControls(secondSeed.original);
    moving.pads[0].levelDb = -9;
    stream.updatePerformance(moving);
    stream.fill(512);
    AudioBuffer<float> heard(1, 1024);
    float volume = 1;
    std::array<std::atomic<int>, 16> mapping;
    for (auto &v : mapping)
        v = -1;
    mapping[0] = 0;
    check(stream.read(heard.getArrayOfWritePointers(), 1, 512, volume, 1, &mapping) == 512,
          "Actual prepared stream consumes Pad A replay while recording B");
    moving.pads[0].x = .7;
    moving.pads[0].levelDb = -3;
    stream.updatePerformance(moving);
    stream.fill(512);
    AudioBuffer<float> rest(1, 512);
    check(stream.read(rest.getArrayOfWritePointers(), 1, 512, volume, 1, &mapping) == 512,
          "Second pad gesture shares the same advancing source clock");
    heard.copyFrom(0, 512, rest, 0, 0, 512);
    cap->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
    second = cap->snapshot();
    check(second.complete && second.events.size() == 2,
          "Overdub retains exactly the actually applied second-pad states");
    auto replay = secondSeed;
    replay.takes = {second};
    replay.activeTake = second.id;
    DesignReader reader(replay, 48000, 0, -1, 0);
    AudioBuffer<float> reproduced(1, 1024);
    check(reader.read(reproduced, 1024) == 1024 && difference(heard, reproduced) < 1e-7,
          "Consumed combined live PCM equals immutable overdub replay");
    retainPadTake(d.original, 1, second);
    auto retainedA = [&](const DesignMix &m)
    {
        auto result = m.masterAutomation;
        std::erase_if(result.curves, [&](auto &c) { return c.target.instance != padIdentity(m, 0); });
        return JSON::toString(automationLaneJson(result), true);
    };
    check(retainedA(preservedA) == retainedA(d.original),
          "Recording B preserves every A point, identity and Read flag");
    check(takeFromJson(takeToJson(first)).recipeHash == first.recipeHash,
          "Recording B never changes the retained A take");
    saveDesignAtomic(file, d);
    check(difference(pcm(d, 61), pcm(loadDesign(file), 257)) < 1e-7,
          "Both recorded pads remain independently editable and reopen under one clock");
    DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.submit = [](auto) { return true; };
    services.post = [](auto) {};
    services.resolve = [](auto &) {};
    services.status = [] { return var(new DynamicObject); };
    services.stop = [] {};
    DesignSession session(root.getChildFile("view.sqlite"), services);
    session.document = fixture(root.getChildFile("ui-sources"));
    for (int p = 0; p < 3; ++p)
    {
        addPerformancePad(session.document);
        assignBlendLayer(session.document.original, 0, session.document.original.layers[(size_t)p + 1].id,
                         p + 1);
    }
    session.document.performPad = 0;
    MultiPadTestAccess::view(session);
    auto settings = d.renderSettings;
    settings.sampleRate = 48000;
    settings.channels = 1;
    settings.layout = "same";
    settings.bits = 32;
    settings.tailSeconds = 0;
    settings.first = 0;
    settings.last = 1024;
    auto output =
        renderDesign(d, root.getChildFile("Renders"), settings, [] { return false; }, [](double) {});
    check(File(output.path).existsAsFile() && output.processing == "multi-pad-perform-v1",
          "Finalized combined render records the new processing policy");
    auto verifyWav = [&](const DesignOutput &output, int pad)
    {
        AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(File(output.path)));
        check(reader && reader->numChannels == 1 && reader->lengthInSamples == 1024 &&
                  reader->sampleRate == 48000,
              "Finalized WAV header matches requested mono range/rate");
        AudioBuffer<float> data(1, 1024);
        reader->read(&data, 0, 1024, 0, true, false);
        for (int frame : {500, 1023})
        {
            double b = .1 * std::pow(10., (frame < 512 ? -9. : -3.) / 20.);
            double expected = pad < 0 ? .4 + b : pad == 1 ? b : .05 * (pad + 1);
            check(std::abs(data.getSample(0, frame) - expected) < 1e-6,
                  "Independent finalized combined/pad PCM oracle after smoothing settles");
        }
    };
    verifyWav(output, -1);
    for (int p = 0; p < 4; ++p)
    {
        auto stem = renderDesign(
            padRenderDocument(d, p), root.getChildFile("Renders"), settings, [] { return false; },
            [](double) {});
        check(File(stem.path).existsAsFile(), "Durable per-pad export is finalized");
        verifyWav(stem, p);
    }
    if (SystemStats::getEnvironmentVariable("SOUNDPLUS_QA_PAD_DEVICE", {}).isNotEmpty())
        runMultiPadDevice(root, app);
    auto report = var(new DynamicObject);
    report.getDynamicObject()->setProperty("checks", checks);
    report.getDynamicObject()->setProperty("document", file.getFullPathName());
    report.getDynamicObject()->setProperty("liveFrozenMaxError", difference(heard, reproduced));
    base.getChildFile("latest.json").replaceWithText(JSON::toString(report, true));
    std::cout << "Multi-pad: " << checks
              << " independent PCM, shared-clock overdub, view and persistence checks passed\n";
}
