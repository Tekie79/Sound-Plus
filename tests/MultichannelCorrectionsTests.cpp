#include "MultichannelCorrectionsTests.h"
#include "DesignSession.h"
#include "DesignerView.h"
#include "LibraryView.h"
#include "TakeTests.h"
#include <juce_cryptography/juce_cryptography.h>
#include <deque>
#include <iostream>
namespace
{
int checks = 0;
void check(bool ok, const char *reason)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(reason);
}
template <class F> bool rejects(F f)
{
    try
    {
        f();
    }
    catch (const std::exception &)
    {
        return true;
    }
    return false;
}
juce::String hash(const juce::File &file)
{
    auto in = file.createInputStream();
    check(in != nullptr, "Fixture file exists");
    return juce::SHA256(*in).toHexString();
}
void wave(const juce::File &file, int channels = 1, int frames = 4096)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat format;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), 48000, (unsigned)channels, 24, {}, 0));
    check(writer != nullptr, "Generated correction fixture writer");
    juce::AudioBuffer<float> pcm(channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < frames; ++n)
            pcm.setSample(c, n, .1f * (c + 1) * std::sin((n + 1) * .017 * (c + 1)));
    check(writer->writeFromAudioSampleBuffer(pcm, 0, frames), "Generated correction fixture PCM");
}
sp::DesignDocument design(const juce::File &file)
{
    sp::DesignDocument d;
    d.name = "Correction QA";
    d.original.layers = {sp::makeDesignLayer(sp::inspectDesignSource(file), 0, -1, true)};
    d.selectedLayer = d.original.layers[0].id;
    d.renderSettings.bits = 32;
    d.renderSettings.tailSeconds = 0;
    return d;
}
struct Worker
{
    std::deque<std::function<void()>> jobs;
    double now = 0;
    sp::DesignSessionServices services()
    {
        sp::DesignSessionServices s;
        s.automaticRecovery = s.timers = false;
        s.submit = [this](auto f)
        {
            jobs.push_back(std::move(f));
            return true;
        };
        s.post = [](auto f) { f(); };
        s.now = [this] { return now; };
        s.resolve = [](auto &) {};
        s.status = [] { return sp::message("status"); };
        s.stop = [] {};
        return s;
    }
    void drain()
    {
        while (!jobs.empty())
        {
            auto f = std::move(jobs.front());
            jobs.pop_front();
            f();
        }
    }
};
} // namespace
namespace sp
{
struct CorrectionViewTestAccess
{
    static void failWrite(DesignSession &s)
    {
        s.services.write = [](const auto &, const auto &)
        { throw std::runtime_error("Controlled write failure"); };
    }
    static void acceptFixtureTake(DesignSession &s, PerformTake t, DesignOutput o)
    {
        s.takeMutation = true;
        s.edit("Accept deterministic fixture take",
               [&](auto &d)
               {
                   d.takes.push_back(t);
                   d.outputs.push_back(o);
               });
        s.takeMutation = false;
    }
};
} // namespace sp
namespace
{
void preferences(const juce::File &root)
{
    auto source = root.getChildFile("input.wav");
    wave(source);
    auto d = design(source);
    auto rendered = sp::renderDesign(
        d, root.getChildFile("outputs"), d.renderSettings, [] { return false; }, [](double) {});
    d.outputs = {rendered};
    auto prototype = sp::freezeTake(d, "Repeated preference QA");
    prototype.complete = true;
    prototype.bodyEnd = prototype.outputFrames = 4096;
    sp::hashTake(prototype);
    auto receipt = [&](const sp::PerformTake &t, int serial)
    {
        auto o = rendered;
        o.takeId = t.id;
        o.recipeHash = t.recipeHash;
        o.eventHash = t.eventHash;
        o.processing = t.processing;
        o.kind = "take-render";
        o.bodyEnd = t.bodyEnd;
        o.capturedFrames = t.outputFrames;
        o.projectRate = t.sampleRate;
        o.captureRate = t.captureRate;
        o.sourceProvenance = sp::takeToJson(t)["sources"];
        auto file = root.getChildFile("outputs/take-" + juce::String(serial) + ".wav");
        check(juce::File(rendered.path).copyFileTo(file), "Separate durable fixture output");
        o.path = file.getFullPathName();
        return o;
    };
    for (int i = 0; i < 16; ++i)
    {
        auto t = prototype;
        t.id = juce::Uuid().toString();
        d.takes.push_back(t);
        d.outputs.push_back(receipt(t, i));
    }
    Worker w;
    sp::DesignSession s(root.getChildFile("catalog/library.sqlite"), w.services());
    s.document = d;
    s.chooseOutput(rendered.path);
    for (int i = 0; i < 16; ++i)
    {
        s.selectTake(d.takes[(size_t)i].id);
        s.chooseOutput(d.outputs[(size_t)i + 1].path);
    }
    check(s.document.outputSelections.size() == 17, "Live plus sixteen current take preferences accepted");
    auto file = root.getChildFile("Lifetime preferences.soundplus");
    s.save(file);
    w.drain();
    check(sp::loadDesign(file).outputSelections.size() == 17, "First explicit save with all contexts");
    for (int i = 16; i < 23; ++i)
    {
        auto removed = s.document.takes.front().id;
        auto count = s.document.outputs.size();
        s.selectTake(removed);
        s.deleteTake();
        check(!s.document.outputSelections.contains(removed) && s.document.takes.size() == 15,
              "Delete removes take and preference transactionally");
        check(s.document.outputs.size() == count, "Delete keeps finalized receipts");
        s.undo();
        check(s.document.outputSelections.contains(removed) && s.document.takes.size() == 16,
              "Undo restores take and preference");
        s.redo();
        check(!s.document.outputSelections.contains(removed) && s.document.takes.size() == 15,
              "Redo removes both again");
        auto t = prototype;
        t.id = juce::Uuid().toString();
        auto o = receipt(t, i);
        sp::CorrectionViewTestAccess::acceptFixtureTake(s, t, o);
        s.selectTake(t.id);
        s.chooseOutput(o.path);
        check(s.document.takes.size() == 16 && s.document.outputSelections.size() == 17,
              "More than seventeen lifetime IDs stay within current-context preference bound");
        w.now += 1000;
        s.poll();
        w.drain();
        auto autoFile = root.getChildFile("catalog/designer-autosave/" + d.id + ".soundplus");
        auto recovered = sp::loadDesign(autoFile);
        check(recovered.revision == s.document.revision && recovered.outputSelections.size() == 17,
              "Autosave retains latest lifetime-churn edits");
        s.save(file);
        w.drain();
        auto reopened = sp::loadDesign(file);
        check(reopened.outputSelections == s.document.outputSelections && reopened.takes.size() == 16,
              "Explicit save and reopen preserve only extant preferences");
    }
    for (auto &o : s.document.outputs)
        check(juce::File(o.path).existsAsFile(), "All old durable files remain");
    auto damaged = sp::designToJson(s.document);
    damaged["outputSelections"].getDynamicObject()->setProperty("deleted-take", rendered.path);
    auto repaired = sp::designFromJson(damaged);
    check(repaired.outputSelections.size() == 17 && repaired.takes.size() == 16 &&
              repaired.outputs.size() == s.document.outputs.size(),
          "Affected eighteen-entry document prunes only dangling preference metadata");
    sp::saveDesignAtomic(root.getChildFile("Repaired.soundplus"), repaired);
    auto corrupt = s.document;
    corrupt.outputSelections["missing"] = rendered.path;
    check(rejects([&] { sp::validateDesign(corrupt); }) &&
              rejects([&] { sp::saveDesignAtomic(file, corrupt); }),
          "Editing/save validation rejects dangling in-memory keys");
    auto validHash = hash(file);
    int cancellationChecks = 0;
    check(
        rejects([&] { sp::saveDesignAtomic(file, s.document, [&] { return ++cancellationChecks >= 2; }); }) &&
            hash(file) == validHash,
        "Unrelated failed publication preserves previous valid save");
    s.editMetadata("Fresh edit before failed save", [](auto &doc) { doc.name += " edited"; });
    sp::CorrectionViewTestAccess::failWrite(s);
    s.save(file);
    w.drain();
    check(s.dirty && hash(file) == validHash,
          "Controlled session write failure preserves fresh dirty work and previous valid save");
    auto selections = damaged["outputSelections"].getDynamicObject();
    for (int i = 0; i < 1002; ++i)
        selections->setProperty("stale-" + juce::String(i), rendered.path);
    check(rejects([&] { sp::designFromJson(damaged); }),
          "Metadata recovery retains an explicit bounded-input guard");
}
} // namespace
void runMultichannelCorrectionsTests(const juce::File &parent, const juce::File &app)
{
    juce::ignoreUnused(app);
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    preferences(root.getChildFile("R11"));
    auto result = sp::message("multichannel-corrections-results");
    result.getDynamicObject()->setProperty("checks", checks);
    result.getDynamicObject()->setProperty(
        "scenarios",
        juce::Array<juce::var>{"R11 lifetime preference churn, Delete/Undo/Redo/save/autosave/recovery"});
    result.getDynamicObject()->setProperty("root", root.getFullPathName());
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(result, true));
    std::cout << "PASS multichannel corrections: " << checks << " checks\n";
}
