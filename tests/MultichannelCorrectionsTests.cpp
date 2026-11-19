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
    std::deque<std::function<void()>> jobs, ui;
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
        s.post = [this](auto f) { ui.push_back(std::move(f)); };
        s.now = [this] { return now; };
        s.resolve = [](auto &) {};
        s.status = [] { return sp::message("status"); };
        s.stop = [] {};
        return s;
    }
    void drain()
    {
        while (!jobs.empty() || !ui.empty())
        {
            auto &queue = jobs.empty() ? ui : jobs;
            auto f = std::move(queue.front());
            queue.pop_front();
            f();
        }
    }
};
} // namespace
namespace sp
{
struct CorrectionViewTestAccess
{
    template <class T> static T &control(juce::Component &p, const juce::String &title)
    {
        for (auto *c : p.getChildren())
            if (auto *v = dynamic_cast<T *>(c);
                v && (c->getTitle() == title || c->getName() == title ||
                      (dynamic_cast<juce::Button *>(c) &&
                       dynamic_cast<juce::Button *>(c)->getButtonText() == title)))
                return *v;
        throw std::runtime_error(("Missing production control: " + title).toStdString());
    }
    static DesignSession &session(DesignerView &v)
    {
        return v.session;
    }
    static void bind(DesignerView &v, DesignDocument d, DesignSessionServices services)
    {
        v.session.protectStartupEdits();
        v.session.document = std::move(d);
        v.session.selectedIds = {v.session.document.selectedLayer};
        v.session.services.submit = std::move(services.submit);
        v.session.services.post = std::move(services.post);
        v.session.services.now = std::move(services.now);
        v.session.services.resolve = [](auto &) {};
        v.session.services.stop = [] {};
        v.session.selectTake(v.session.document.activeTake);
        v.refresh();
    }
    static std::unique_ptr<juce::Component> renderPanel(DesignerView &v)
    {
        return v.makeRenderPanel();
    }
    static void render(DesignerView &v)
    {
        v.render.onClick();
        v.session.renderWorker.join();
    }
    static void chooseOutput(DesignerView &v, const juce::String &path)
    {
        v.refresh();
        for (size_t i = 0; i < v.session.document.outputs.size(); ++i)
            if (v.session.document.outputs[i].path == path)
                v.readyOutputs.setSelectedId((int)i + 1, juce::sendNotificationSync);
    }
    static void print(DesignerView &v)
    {
        v.addPrint.onClick();
    }
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
sp::DesignDocument historical(const juce::File &root, const juce::File &app)
{
    auto repo = app.getParentDirectory();
    while (!repo.getChildFile("AGENTS.md").existsAsFile() && repo != repo.getParentDirectory())
        repo = repo.getParentDirectory();
    auto fixture = repo.getChildFile("docs/verification/performance-v13-native-qa.soundplus.json.gz");
    check(fixture.existsAsFile(), "Mandatory historical legacy fixture exists");
    juce::GZIPDecompressorInputStream in(fixture.createInputStream().release(), true,
                                         juce::GZIPDecompressorInputStream::gzipFormat);
    root.createDirectory();
    auto saved = root.getChildFile("Historical.soundplus");
    saved.replaceWithText(in.readEntireStreamAsString());
    auto d = sp::loadDesign(saved);
    auto inputs = root.getChildFile("inputs");
    prepareTakeUiFixture(inputs);
    for (auto &take : d.takes)
        for (auto &source : take.sources)
        {
            auto layer = std::find_if(take.recipe->layers.begin(), take.recipe->layers.end(),
                                      [&](auto &l) { return l.source.id == source.id; });
            check(layer != take.recipe->layers.end(), "Historical source identity");
            auto file = inputs.getChildFile(juce::File(layer->source.path).getFileName());
            check(hash(file) == source.sha256, "Generated historical source fingerprint unchanged");
            sp::FileStamp stamp;
            sp::fileStamp(file, stamp);
            source.overridePath = file.getFullPathName();
            source.overrideSize = stamp.size;
            source.overrideModified = stamp.modified;
            source.overrideInode = stamp.inode;
        }
    return d;
}
void legacySplit(const juce::File &root, const juce::File &app)
{
    auto d = historical(root, app);
    auto id = d.activeTake;
    auto oldRecipe = d.takes[0].recipeHash, oldEvent = d.takes[0].eventHash;
    auto oldOutputHash = d.outputs[0].sha256;
    d.outputFolder = root.getChildFile("outputs").getFullPathName();
    Worker w;
    sp::AudioClient audio(app);
    juce::PropertiesFile prefs(root.getChildFile("ui.settings"), sp::ui::preferenceOptions());
    sp::DesignerView view(root.getChildFile("library.sqlite"), audio, prefs);
    sp::CorrectionViewTestAccess::bind(view, d, w.services());
    auto &s = sp::CorrectionViewTestAccess::session(view);
    auto panel = sp::CorrectionViewTestAccess::renderPanel(view);
    auto &layout = sp::CorrectionViewTestAccess::control<juce::ComboBox>(*panel, "Export layout");
    layout.setSelectedId(1, juce::sendNotificationSync);
    auto &split = sp::CorrectionViewTestAccess::control<juce::ToggleButton>(
        *panel, "Split mono set + multichannel Print master");
    split.setToggleState(true, juce::dontSendNotification);
    split.onClick();
    check(s.document.renderSettings.splitMono && s.document.renderSettings.layout.isEmpty(),
          "Actual legacy render panel enables split without converting DSP");
    s.undo();
    check(!s.document.renderSettings.splitMono, "Undo legacy split setting");
    s.redo();
    auto render = [&]
    {
        sp::CorrectionViewTestAccess::render(view);
        w.drain();
        check(!s.rendering && s.document.outputs.size() > 1,
              "Actual legacy Render callback publishes receipt");
        return s.document.outputs.back();
    };
    auto stereo = render();
    check(stereo.channelProvenance.isVoid() && stereo.outputSetVersion == 1 && stereo.files.size() == 2,
          "Legacy split set has independent metadata contract");
    for (int c = 0; c < 2; ++c)
    {
        auto v = stereo.files[c];
        check(v["role"].toString() == (c == 0 ? "L" : "R") &&
                  hash(juce::File(v["path"].toString())) == v["sha256"].toString(),
              "Actual legacy stem role/path/hash");
    }
    split.setToggleState(false, juce::dontSendNotification);
    split.onClick();
    auto interleaved = render();
    check(interleaved.files.isVoid(), "Legacy interleaved output stays a single file");
    check(interleaved.sha256 == oldOutputHash && stereo.sha256 == oldOutputHash,
          "Legacy interleaved and split Print-master PCM/container bytes retain the historical checksum");
    auto &channels = sp::CorrectionViewTestAccess::control<juce::ComboBox>(*panel, "Channels");
    channels.setSelectedId(1, juce::sendNotificationSync);
    split.setToggleState(true, juce::dontSendNotification);
    split.onClick();
    auto mono = render();
    check(mono.files.size() == 1 && mono.files[0]["role"].toString() == "C",
          "Legacy mono split member identity");
    sp::CorrectionViewTestAccess::chooseOutput(view, stereo.path);
    auto save = root.getChildFile("Legacy split delivery.soundplus");
    s.save(save);
    w.drain();
    auto reopened = sp::loadDesign(save);
    check((int)sp::designToJson(reopened)["version"] == 6 && reopened.renderSettings.splitMono &&
              reopened.renderSettings.layout.isEmpty(),
          "Versioned split metadata survives legacy document reopen");
    s.document = reopened;
    s.selectTake(id);
    for (auto &o : reopened.outputs)
        if (o.path == stereo.path)
            check(juce::JSON::toString(o.files, true) == juce::JSON::toString(stereo.files, true) &&
                      o.outputSetVersion == 1,
                  "Every legacy stem role/path/hash survives serialization exactly");
    check(s.readyFiles().size() == 2 && s.readyOutput() == juce::File(stereo.path),
          "Reopen retains selected stereo stem set rather than latest mono output");
    s.chooseOutput(interleaved.path);
    check(s.readyFiles().size() == 1 && s.readyOutput() == juce::File(interleaved.path),
          "Select interleaved delivery independently");
    s.undo();
    check(s.readyFiles().size() == 2, "Undo restores selected split set");
    s.redo();
    check(s.readyOutput() == juce::File(interleaved.path), "Redo restores interleaved choice");
    s.chooseOutput(stereo.path);
    auto stem = juce::File(stereo.files[0]["path"].toString()),
         held = stem.getSiblingFile("held-owned-L.wav");
    check(stem.moveFileTo(held), "Hold owned legacy stem");
    check(s.readyFiles().isEmpty() && s.readyOutput() == juce::File(),
          "Missing selected legacy member invalidates delivery without substituting another output");
    check(held.moveFileTo(stem), "Restore owned legacy stem");
    check(s.readyFiles().size() == 2, "Restored complete selected set becomes ready");
    auto count = s.document.original.layers.size();
    sp::CorrectionViewTestAccess::print(view);
    w.drain();
    check(s.document.activeTake.isEmpty() && s.document.original.layers.size() == count + 1 &&
              s.document.original.layers.back().source.path == stereo.path,
          "Actual Print callback imports durable interleaved master, not first stem");
    s.undo();
    check(s.document.original.layers.size() == count, "Undo Print preserves set");
    s.redo();
    check(s.document.takes[0].recipeHash == oldRecipe && s.document.takes[0].eventHash == oldEvent &&
              s.document.outputs[0].sha256 == oldOutputHash,
          "Legacy split workflow preserves existing canonical take and output hashes");
    s.save(save);
    w.drain();
    check(sp::loadDesign(save).takes[0].recipeHash == oldRecipe,
          "Printed legacy design reopens with unchanged take identity");
}

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
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    preferences(root.getChildFile("R11"));
    legacySplit(root.getChildFile("R17"), app);
    auto result = sp::message("multichannel-corrections-results");
    result.getDynamicObject()->setProperty("checks", checks);
    result.getDynamicObject()->setProperty(
        "scenarios",
        juce::Array<juce::var>{"R11 lifetime preference churn, Delete/Undo/Redo/save/autosave/recovery"});
    auto scenarios = result["scenarios"];
    scenarios.append(
        "R17 historical legacy panel callbacks, split/interleaved/mono membership, save/reopen/Print");
    result.getDynamicObject()->setProperty("scenarios", scenarios);
    result.getDynamicObject()->setProperty("root", root.getFullPathName());
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(result, true));
    std::cout << "PASS multichannel corrections: " << checks << " checks\n";
}
void prepareSpatialV1Fixture(const juce::File &folder)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (folder.exists())
        throw std::runtime_error("Use a fresh owned historical fixture directory");
    folder.createDirectory();
    auto file = folder.getChildFile("constant-point.wav");
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), 48000, 1, 32, {}, 0));
    juce::AudioBuffer<float> pcm(1, 14400);
    pcm.clear();
    for (int n = 0; n < 14400; ++n)
        pcm.setSample(0, n, .2f);
    check(writer && writer->writeFromAudioSampleBuffer(pcm, 0, 14400), "Historical constant source");
    writer.reset();
    auto d = design(file);
    d.name = "Historical repeated negative wraps";
    d.original.masterDb = 0;
    sp::setMixLayout(d.original, sp::channelLayout("5.1-side-v1"));
    d.original.layers[0].routing.spatial = true;
    d.renderSettings.layout = "same";
    auto t = sp::freezeTake(d, "Repeated negative wraps v1");
    t.generation = 1;
    std::array<double, 11> targets{0, -90, -180, 90, 0, -90, -180, 90, 0, -90, -130};
    for (size_t i = 0; i < targets.size(); ++i)
    {
        auto c = sp::mixControls(d.original);
        c.spatial[0].azimuth = targets[i];
        t.events.push_back({(int64_t)i * 1200, i + 1, 1, c});
    }
    t.complete = true;
    t.reason.clear();
    t.bodyEnd = t.outputFrames = 14400;
    sp::hashTake(t);
    d.takes = {t};
    d.activeTake = t.id;
    auto output = sp::renderDesign(
        d, folder.getChildFile("outputs"), d.renderSettings, [] { return false; }, [](double) {});
    d.outputs = {output};
    sp::saveDesignAtomic(folder.getChildFile("Negative wraps v1.soundplus"), d);
    std::cout << juce::JSON::toString(sp::designToJson(d), true) << "\n";
}
