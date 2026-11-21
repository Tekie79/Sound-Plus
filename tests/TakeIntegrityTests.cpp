#include "TakeIntegrityTests.h"
#include "DesignerView.h"
#include "DesignLive.h"
#include "AudioStream.h"
#include "Performance.h"
#include <juce_cryptography/juce_cryptography.h>
#include <deque>
#include <iostream>
namespace
{
int checks = 0;
void check(bool value, const char *why)
{
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
template <class F> void until(F f, const char *why, int ms = 15000)
{
    auto end = sp::monotonicMs() + ms;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    check(f(), why);
}
struct Queue
{
    std::mutex mutex;
    std::deque<std::function<void()>> jobs;
    void post(std::function<void()> f)
    {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(f));
    }
    bool pending()
    {
        std::lock_guard lock(mutex);
        return !jobs.empty();
    }
    void drain()
    {
        for (;;)
        {
            std::function<void()> f;
            {
                std::lock_guard lock(mutex);
                if (jobs.empty())
                    break;
                f = std::move(jobs.front());
                jobs.pop_front();
            }
            f();
        }
    }
};
void source(const juce::File &file)
{
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream.release(), 48000, 2, 24, {}, 0));
    juce::AudioBuffer<float> b(2, 4096);
    for (int at = 0; at < 48000 * 8; at += 4096)
    {
        int n = std::min(4096, 48000 * 8 - at);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c)
                b.setSample(c, i, .16f * std::sin((at + i) * (.03 + c * .007)) + (at + i == 0 ? .3f : 0));
        check(w->writeFromAudioSampleBuffer(b, 0, n), "Generated finalized sustained stereo fixture");
    }
}
sp::DesignDocument fixture(const juce::File &root)
{
    auto f = root.getChildFile("source.wav");
    source(f);
    sp::DesignDocument d;
    d.name = "Take integrity QA";
    auto l = sp::makeDesignLayer(sp::inspectDesignSource(f), 0, -1, true);
    l.inserts.stock = {sp::makeStockEffect("delay")};
    d.original.layers = {l};
    d.original.inserts.stock = {sp::makeStockEffect("reverb")};
    auto &p = d.original.performance;
    p.macros[0].enabled = true;
    p.macros[0].value = .15;
    sp::addMacroMapping(d.original, 0, {{l.id, {}, "gain"}, -25., -3.});
    d.renderSettings.bits = 32;
    d.renderSettings.tailSeconds = .4;
    d.outputFolder = root.getChildFile("renders").getFullPathName();
    return d;
}
sp::PerformTake capture(const sp::DesignDocument &d, const juce::String &name, double value)
{
    auto t = sp::freezeTake(d, name);
    t.generation = 17;
    t.captureRate = 48000;
    sp::hashTake(t);
    auto c = std::make_shared<sp::TakeCapture>(t);
    auto base = d;
    base.takeProcessing = true;
    base.renderSettings.tailSeconds = 0;
    sp::PcmStream producer(base, 48000, 0);
    producer.startTake(c);
    juce::AudioBuffer<float> b(2, 512);
    float g = 1;
    for (int i = 0; i < 240; ++i)
    {
        if (i == 1)
        {
            auto controls = sp::performControls(t.recipe->performance);
            controls.macros[0] = value;
            producer.updatePerformance(controls);
        }
        producer.fill(512);
        producer.read(b.getArrayOfWritePointers(), 2, 512, g, 1);
    }
    c->finish(producer.acceptedFrame(), true, producer.consumedOutputFrames());
    t = c->snapshot();
    check(t.complete && t.events.size() == 1,
          "Real producer records sparse event followed by sustained state");
    return t;
}
juce::AudioBuffer<float> read(const sp::DesignDocument &d)
{
    sp::DesignReader r(d, 48000, 0, -1, .4);
    juce::AudioBuffer<float> b(2, (int)r.length()), block(2, 4096);
    int at = 0;
    while (auto n = r.read(block, 4096))
    {
        for (int c = 0; c < 2; ++c)
            b.copyFrom(c, at, block, c, 0, n);
        at += n;
    }
    return b;
}
double difference(const juce::AudioBuffer<float> &a, const float *raw)
{
    double e = 0;
    for (int n = 0; n < a.getNumSamples(); ++n)
        for (int c = 0; c < 2; ++c)
            e = std::max(e, (double)std::abs(a.getSample(c, n) - raw[n * 2 + c]));
    return e;
}
juce::String hash(const juce::File &f)
{
    auto in = f.createInputStream();
    return juce::SHA256(*in).toHexString();
}
} // namespace
namespace sp
{
struct TakeIntegrityTestAccess
{
    static void bind(DesignerView &v, Queue &q, DesignDocument d)
    {
        auto &s = v.session;
        s.stopTimer();
        s.protectStartupEdits();
        s.services.post = [&q](auto job) { q.post(std::move(job)); };
        s.services.resolve = [](auto &) {};
        s.document = std::move(d);
        s.restoreDerivedState();
        v.refresh();
    }
    static void ready(DesignerView &v, const juce::File &expected)
    {
        v.refresh();
        check(v.dragOutput.file == expected && v.dragOutput.resolveFile() == expected,
              "Actual drag file binding resolves selected context");
        check(v.dragOutput.isEnabled() == expected.existsAsFile() &&
                  v.addPrint.isEnabled() == expected.existsAsFile(),
              "Actual delivery controls reflect finalized existing current output");
    }
    static void print(DesignerView &v)
    {
        v.addPrint.onClick();
    }
    static void settings(DesignerView &v)
    {
        auto before = juce::JSON::toString(designMixToJson(v.session.document.activeMix()));
        auto context = v.session.document.activeTake;
        auto panel = v.makeRenderPanel();
        int callbacks = 0;
        for (auto *child : panel->getChildren())
        {
            if (auto *editor = dynamic_cast<juce::TextEditor *>(child))
            {
                if (editor->getTitle().startsWith("Fixed tail"))
                {
                    editor->setText("0.75");
                    editor->onReturnKey();
                    ++callbacks;
                }
                else
                    check(!editor->isEnabled(), "Full take range controls are honestly unavailable");
            }
            if (auto *toggle = dynamic_cast<juce::ToggleButton *>(child);
                toggle && (toggle->getButtonText().startsWith("Explicit peak") ||
                           toggle->getButtonText().startsWith("TPDF")))
            {
                toggle->setToggleState(true, juce::dontSendNotification);
                toggle->onClick();
                ++callbacks;
            }
            if (auto *box = dynamic_cast<juce::ComboBox *>(child); box && box->getTitle() == "WAV depth")
            {
                box->setSelectedId(2, juce::dontSendNotification);
                box->onChange();
                ++callbacks;
            }
        }
        auto &s = v.session.document.renderSettings;
        check(callbacks == 4 && s.tailSeconds == .75 && s.normalize && s.dither && s.bits == 24,
              "Actual settings panel callbacks apply tail, normalization, dither and format in take context");
        check(v.session.document.activeTake == context &&
                  juce::JSON::toString(designMixToJson(v.session.document.activeMix())) == before,
              "Settings callbacks retain take context and unchanged Live recipe");
    }
};
} // namespace sp
void runTakeIntegrityTests(const juce::File &parent, const juce::File &application)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto d = fixture(root);
    auto variationContext = d;
    variationContext.activeVariation = "alternate-live-variation";
    check(sp::designPlaybackContext(variationContext) == sp::designPlaybackContext(d),
          "Static variation changes remain in the legitimate mutable Live transport context");
    auto a = capture(d, "Integrity A", .9), b = capture(d, "Integrity B", .3);
    d.takes = {a, b};
    d.activeTake = a.id;
    const auto recipe = a.recipeHash, events = a.eventHash;
    auto baseline = read(d);
    // Reader itself rejects live controls even if a stale caller bypasses session/service guards.
    auto altered = d;
    altered.original.layers[0].gainDb = -65;
    altered.original.inserts.stock.clear();
    auto extra = altered.original.layers[0];
    extra.id = juce::Uuid().toString();
    altered.original.layers.push_back(extra);
    sp::DesignReader guarded(d, 48000, 0, -1, .4);
    guarded.updateParameters(sp::designParameters(altered));
    juce::AudioBuffer<float> guardedPcm(2, baseline.getNumSamples()), block(2, 4096);
    int at = 0;
    while (auto n = guarded.read(block, 4096))
    {
        for (int c = 0; c < 2; ++c)
            guardedPcm.copyFrom(c, at, block, c, 0, n);
        at += n;
    }
    double readerError = 0;
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < baseline.getNumSamples(); ++n)
            readerError = std::max(readerError,
                                   (double)std::abs(baseline.getSample(c, n) - guardedPcm.getSample(c, n)));
    check(readerError < 1e-7, "Reader rejects Live Design parameters throughout immutable replay");
    Queue q;
    sp::AudioClient audio(application);
    until([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native output available");
    audio.deviceRate(48000);
    audio.bufferSize(512);
    until(
        [&] {
            return (double)audio.status()["sampleRate"] == 48000 &&
                   (int)audio.status()["bufferFrames"] == 512;
        },
        "Native48k512 active");
    audio.gain(.1);
    auto raw = root.getChildFile("metadata-audition-original.f32");
    sp::DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.post = [&](auto job) { q.post(std::move(job)); };
    services.resolve = [](auto &) {};
    services.status = [&] { return audio.status(); };
    services.stop = [&] { audio.stop(); };
    services.parameters = [&](const auto &doc, uint64_t gen, double now)
    { audio.updateDesign(doc, gen, now); };
    services.play = [&](const auto &id, const auto &file, int64_t frame, const auto &mode)
    { return audio.playDesign(id, file, frame, raw, mode); };
    sp::DesignSession session(root.getChildFile("catalog.sqlite"), services);
    session.document = altered;
    session.selectTake(a.id);
    session.play();
    session.nameTake("Integrity A renamed while preparing");
    session.editMetadata("Render settings", [](auto &doc) { doc.renderSettings.tailSeconds = .8; });
    until([&] { return q.pending(); }, "Held snapshot preparation completion available");
    q.drain();
    until([&] { return (bool)audio.status()["playing"]; },
          "Native take audition starts from immutable snapshot");
    auto initial = audio.status();
    auto gen = (uint64_t)(int64_t)initial["playGeneration"];
    // Stale queued live state deliberately has the same document/generation and a mismatched live rack/layer
    // set.
    auto live = altered;
    live.activeTake.clear();
    audio.updateDesign(live, gen, sp::monotonicMs());
    session.render();
    until([&] { return q.pending() && session.renderProgress.load() >= 1.; },
          "Actual render completed but UI receipt adoption held");
    session.nameTake("Integrity A kept during audition");
    session.editMetadata("Render output folder", [&](auto &doc)
                         { doc.outputFolder = root.getChildFile("later-renders").getFullPathName(); });
    q.drain();
    session.poll();
    q.drain();
    session.editMetadata("Render settings", [](auto &doc) { doc.renderSettings.bits = 24; });
    session.undo();
    session.redo();
    until(
        [&]
        {
            session.poll();
            q.drain();
            auto s = audio.status();
            return !(bool)s["playing"] && (int64_t)s["sourceSample"] >= a.bodyEnd;
        },
        "Immutable native audition reaches natural end");
    auto final = audio.status();
    check(final["error"].toString().isEmpty() && final["playGeneration"] == initial["playGeneration"] &&
              final["graphEpoch"] == initial["graphEpoch"] &&
              final["preparationsRequested"] == initial["preparationsRequested"] &&
              final["playbackContext"] == sp::designPlaybackContext(d) && (bool) final["immutableReplay"],
          "Metadata/held completion/stale live controls preserve take/generation and preparation identity");
    audio.stop();
    juce::MemoryBlock bytes;
    until(
        [&]
        {
            raw.loadFileAsData(bytes);
            return bytes.getSize() >= (size_t)baseline.getNumSamples() * 8;
        },
        "Original native pre-monitor audition PCM flushed");
    auto pcmError = difference(baseline, (const float *)bytes.getData());
    check(pcmError < 1e-6, "Metadata activity is PCM-identical to uninterrupted immutable replay");
    check(session.document.takes[0].recipeHash == recipe && session.document.takes[0].eventHash == events,
          "Metadata preserves frozen hashes");
    auto pump = [&]
    {
        q.drain();
        session.poll();
    };
    auto reopen = [&](const juce::String &id, const char *label)
    {
        session.selectTake(id);
        auto f = root.getChildFile(label);
        session.save(f);
        until(
            [&]
            {
                pump();
                return !session.working && session.savedFile == f;
            },
            "Context saved");
        session.open(f);
        until(
            [&]
            {
                pump();
                return !session.transitionPending() && session.savedFile == f;
            },
            "Context reopened");
        check(session.document.activeTake == id, "Reopen retains actual selected context");
    };
    auto outputA = session.readyOutput();
    check(outputA.existsAsFile(), "A's completed receipt resolves A output");
    reopen(b.id, "B-unrendered.soundplus");
    check(session.readyOutput() == juce::File(), "Unrendered B never inherits A output after save/reopen");
    auto outputB = sp::renderDesign(
        session.document, root.getChildFile("renders"), session.document.renderSettings, [] { return false; },
        [](double) {});
    session.editMetadata("Record completed output", [&](auto &doc) { doc.outputs.push_back(outputB); });
    auto staticDoc = session.document;
    staticDoc.activeTake.clear();
    auto staticOut = sp::renderDesign(
        staticDoc, root.getChildFile("renders"), staticDoc.renderSettings, [] { return false; },
        [](double) {});
    staticOut.kind = "design-render";
    session.editMetadata("Record completed output", [&](auto &doc) { doc.outputs.push_back(staticOut); });
    for (auto id : {a.id, b.id, juce::String()})
    {
        reopen(id, id == a.id ? "A.soundplus" : id == b.id ? "B.soundplus" : "Live.soundplus");
        check(session.readyOutput() ==
                  (id == a.id ? outputA : juce::File(id == b.id ? outputB.path : staticOut.path)),
              "A/B/Live ready policy survives reopen");
    }
    session.selectTake(a.id);
    session.render();
    until([&] { return q.pending() && session.renderProgress.load() >= 1.; },
          "A render held before context switch");
    session.selectTake(b.id);
    auto beforeReceipts = session.document.outputs.size();
    q.drain();
    check(session.document.activeTake == b.id && session.readyOutput() == juce::File(outputB.path) &&
              session.document.outputs.size() == beforeReceipts + 1,
          "A completion retains receipt without retargeting B delivery");
    session.selectTake(a.id);
    session.render();
    until([&] { return q.pending() && session.renderProgress.load() >= 1.; }, "Second A completion held");
    session.selectTake({});
    q.drain();
    check(session.document.activeTake.isEmpty() && session.readyOutput() == juce::File(staticOut.path),
          "Held take completion cannot steal Live delivery");
    session.selectTake(a.id);
    auto count = session.document.outputs.size();
    session.deleteTake();
    check(session.document.outputs.size() == count && outputA.existsAsFile(),
          "Delete preserves completed receipt history and durable WAV");
    session.undo();
    check(session.document.activeTake == a.id && session.readyOutput().existsAsFile(),
          "Undo restores correct take delivery");
    session.redo();
    check(session.document.activeTake.isEmpty() && session.readyOutput() == juce::File(staticOut.path),
          "Redo deletion resolves Live context");
    session.undo();
    {
        juce::PropertiesFile prefs(root.getChildFile("view.settings"), sp::ui::preferenceOptions());
        sp::DesignerView view(root.getChildFile("view/catalog.sqlite"), audio, prefs);
        Queue vq;
        auto vd = session.document;
        vd.activeTake = b.id;
        sp::TakeIntegrityTestAccess::bind(view, vq, vd);
        view.session.play();
        until(
            [&]
            {
                vq.drain();
                view.session.poll();
                return (bool)audio.status()["playing"] &&
                       audio.status()["playbackContext"].toString() ==
                           sp::designPlaybackContext(view.session.document);
            },
            "Native audition active before real output-settings callback regression");
        auto panelStart = audio.status();
        sp::TakeIntegrityTestAccess::settings(view);
        view.session.poll();
        vq.drain();
        auto panelEnd = audio.status();
        check(panelEnd["playGeneration"] == panelStart["playGeneration"] &&
                  panelEnd["preparationsRequested"] == panelStart["preparationsRequested"] &&
                  panelEnd["error"].toString().isEmpty(),
              "Actual popup metadata callbacks do not restart or fail native take audition");
        view.session.stop();
        sp::TakeIntegrityTestAccess::ready(view, juce::File(outputB.path));
        auto oldLayers = view.session.document.activeMix().layers.size();
        sp::TakeIntegrityTestAccess::print(view);
        until(
            [&]
            {
                vq.drain();
                return !view.session.working;
            },
            "Actual print-back button import completes");
        check(view.session.document.activeTake.isEmpty() &&
                  view.session.document.activeMix().layers.size() == oldLayers + 1 &&
                  view.session.document.activeMix().layers.back().source.path == outputB.path,
              "Actual print-back imports B's matching file as ordinary layer");
        view.session.undo();
        check(view.session.document.activeMix().layers.size() == oldLayers &&
                  juce::File(outputB.path).existsAsFile(),
              "Print Undo retains takes/receipts/WAV");
        auto missing = juce::File(outputB.path).getSiblingFile("missing-owned.wav");
        check(juce::File(outputB.path).moveFileTo(missing), "Externally remove only B output");
        view.session.selectTake(b.id);
        sp::TakeIntegrityTestAccess::ready(view, {});
        sp::TakeIntegrityTestAccess::print(view);
        check(!view.session.working, "Missing output cannot print cached A or B path");
        check(missing.moveFileTo(juce::File(outputB.path)), "Restore exact durable B output");
    }
    // Actual relocation: the original path is absent, not merely a copied online original.
    session.selectTake(a.id);
    auto sourceId = a.sources[0].id;
    auto original = juce::File(a.recipe->layers[0].source.path),
         relocated = root.getChildFile("relocated-identical.wav");
    check(original.moveFileTo(relocated), "Move original take source away");
    auto wrong = root.getChildFile("wrong-source.wav");
    wrong.replaceWithText("not the frozen recording");
    session.relinkTakeSource(sourceId, wrong);
    until([&] { pump(); return session.message.contains("fingerprint differs"); },
          "Incorrect source fingerprint is rejected");
    check(session.takeState == sp::DesignSession::TakeState::ready &&
              session.document.takes[0].complete &&
              session.document.takes[0].sources[0].overridePath.isEmpty(),
          "Rejected relink preserves complete take state and source binding");
    session.relinkTakeSource(sourceId, relocated);
    until(
        [&]
        {
            pump();
            return session.document.takes[0].sources[0].overridePath == relocated.getFullPathName();
        },
        "Verified relocation accepted while original path absent");
    auto unrelated = session.document.original.layers[0];
    unrelated.id = juce::Uuid().toString();
    unrelated.source.id = "external-missing-unrelated";
    unrelated.source.path = root.getChildFile("never-existed.wav").getFullPathName();
    unrelated.source.error = "Offline or missing";
    unrelated.source.online = false;
    session.document.original.layers.push_back(unrelated);
    auto moved = sp::renderDesign(
        session.document, root.getChildFile("relocated-renders"), session.document.renderSettings,
        [] { return false; }, [](double) {});
    check(juce::File(moved.path).existsAsFile() && moved.recipeHash == recipe && moved.eventHash == events &&
              moved.sourceProvenance[0]["effectivePath"].toString() == relocated.getFullPathName() &&
              (int64_t)moved.sourceProvenance[0]["revision"] == a.recipe->layers[0].source.revision,
          "Relocated take renders independently of missing live sources with effective provenance");
    auto movedHash = hash(juce::File(moved.path));
    auto preserved = relocated.getSiblingFile("preserved-original-inode.wav");
    bool mutated = false, rejected = false;
    auto failFolder = root.getChildFile("failed-renders");
    try
    {
        sp::renderDesign(
            session.document, failFolder, session.document.renderSettings, [] { return false; },
            [&](double progress)
            {
                if (progress >= 1 && !mutated)
                {
                    check(relocated.moveFileTo(preserved),
                          "Preserve actual source inode before publication fault");
                    relocated.replaceWithText("changed source after final PCM block");
                    mutated = true;
                }
            });
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(mutated && rejected && failFolder.findChildFiles(juce::File::findFiles, false, "*.wav").isEmpty(),
          "Final actual-source mutation fails without publishing partial output");
    relocated.deleteFile();
    check(preserved.moveFileTo(relocated), "Restore exact relinked source identity");
    check(hash(juce::File(moved.path)) == movedHash && outputA.existsAsFile() &&
              juce::File(outputB.path).existsAsFile(),
          "Existing outputs survive failed publication");
    auto result = sp::message("take-integrity-results");
    auto *o = result.getDynamicObject();
    o->setProperty("checks", checks);
    o->setProperty("nativeMetadataReplayMaxError", pcmError);
    o->setProperty("readerGuardMaxError", readerError);
    o->setProperty("nativeStart", initial);
    o->setProperty("nativeEnd", final);
    o->setProperty("takeA", sp::takeToJson(a));
    o->setProperty("takeB", sp::takeToJson(b));
    o->setProperty("relocatedOutput", moved.path);
    o->setProperty("relocatedOutputSha256", movedHash);
    o->setProperty("effectiveSource", moved.sourceProvenance);
    o->setProperty("originalUnavailable", !original.existsAsFile());
    o->setProperty("finalSourceMutationRejected", rejected);
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(result, true));
    std::cout << "PASS take integrity: " << checks << " checks, native PCM error " << pcmError << "\n";
}
