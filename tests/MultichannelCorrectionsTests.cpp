#include "MultichannelCorrectionsTests.h"
#include "DesignSession.h"
#include "DesignerView.h"
#include "LibraryView.h"
#include "TakeTests.h"
#include "AudioStream.h"
#include "DesignLive.h"
#include "Performance.h"
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
    static void waitRender(DesignSession &s)
    {
        s.renderWorker.join();
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

std::array<double, 16> spatialOracle(const sp::ChannelLayout &layout, double angle, double divergence)
{
    std::vector<std::pair<double, int>> ring;
    for (int c = 0; c < layout.size(); ++c)
    {
        auto r = layout.roles[c];
        if (r == "LFE")
            continue;
        double a = r == "L"    ? -30
                   : r == "R"  ? 30
                   : r == "C"  ? 0
                   : r == "Ls" ? -90
                   : r == "Rs" ? 90
                   : r == "Lb" ? -150
                               : 150;
        ring.emplace_back(a, c);
    }
    std::sort(ring.begin(), ring.end());
    // Independent principal-angle oracle: remainder, literal speaker geometry,
    // and long-double normalized equal-power coefficients.
    long double position = std::remainder(angle, 360.0);
    if (position < ring.front().first)
        position += 360;
    std::array<double, 16> g{};
    for (size_t i = 0; i < ring.size(); ++i)
    {
        auto lo = ring[i], hi = ring[(i + 1) % ring.size()];
        if (i + 1 == ring.size())
            hi.first += 360;
        if (position >= lo.first && position <= hi.first)
        {
            long double fraction = (position - lo.first) / (hi.first - lo.first);
            g[(size_t)lo.second] = (double)std::cos(fraction * std::acos(-1.L) / 2);
            g[(size_t)hi.second] = (double)std::sin(fraction * std::acos(-1.L) / 2);
            break;
        }
    }
    double energy = 0;
    for (auto [a, c] : ring)
    {
        juce::ignoreUnused(a);
        g[(size_t)c] = (1 - divergence) * g[(size_t)c] + divergence / std::sqrt((double)ring.size());
        energy += g[(size_t)c] * g[(size_t)c];
    }
    for (auto &v : g)
        v /= std::sqrt(energy);
    return g;
}
juce::AudioBuffer<float> pcm(const sp::DesignDocument &d)
{
    sp::DesignReader r(d, 48000, 0, -1, 0);
    juce::AudioBuffer<float> all(sp::designChannels(d), (int)r.length()), block(sp::designChannels(d), 4096);
    int at = 0;
    while (auto n = r.read(block, 4096))
    {
        for (int c = 0; c < all.getNumChannels(); ++c)
            all.copyFrom(c, at, block, c, 0, n);
        at += n;
    }
    return all;
}
double pcmDifference(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b, int count = -1)
{
    check(a.getNumChannels() == b.getNumChannels(), "PCM channel identities");
    if (count < 0)
    {
        check(a.getNumSamples() == b.getNumSamples(), "PCM frame extents");
        count = a.getNumSamples();
    }
    check(a.getNumSamples() >= count && b.getNumSamples() >= count, "PCM prefix exists");
    double error = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < count; ++n)
        {
            check(std::isfinite(a.getSample(c, n)) && std::isfinite(b.getSample(c, n)),
                  "Finite compared PCM");
            error = std::max(error, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
        }
    return error;
}
void constant(const juce::File &file, const std::vector<float> &levels, int frames = 192000)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), 48000, (unsigned)levels.size(), 32, {}, 0));
    juce::AudioBuffer<float> b((int)levels.size(), frames);
    for (int c = 0; c < (int)levels.size(); ++c)
        for (int n = 0; n < frames; ++n)
            b.setSample(c, n, levels[(size_t)c]);
    check(writer && writer->writeFromAudioSampleBuffer(b, 0, frames), "Known constant channel fixture");
}
template <class F> void until(F f, const char *reason, int ms = 15000)
{
    auto end = sp::monotonicMs() + ms;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    check(f(), reason);
}
juce::AudioBuffer<float> capturePcm(const juce::File &file, int frames)
{
    juce::MemoryBlock bytes;
    until(
        [&]
        {
            file.loadFileAsData(bytes);
            return bytes.getSize() >= (size_t)frames * 6 * 4;
        },
        "Consumed pre-monitor PCM flushed");
    juce::AudioBuffer<float> b(6, frames);
    auto *data = (const float *)bytes.getData();
    for (int n = 0; n < frames; ++n)
        for (int c = 0; c < 6; ++c)
            b.setSample(c, n, data[n * 6 + c]);
    return b;
}
juce::var spatialCorrection(const juce::File &root, const juce::File &app)
{
    auto repo = app.getParentDirectory();
    while (!repo.getChildFile("AGENTS.md").existsAsFile() && repo != repo.getParentDirectory())
        repo = repo.getParentDirectory();
    auto extract = [&](const char *name, const juce::File &target)
    {
        auto f = repo.getChildFile("docs/verification/" + juce::String(name));
        check(f.existsAsFile(), "Mandatory pre-repair spatial fixture exists");
        juce::GZIPDecompressorInputStream in(f.createInputStream().release(), true,
                                             juce::GZIPDecompressorInputStream::gzipFormat);
        auto out = target.createOutputStream();
        check(out != nullptr, "Owned spatial fixture output");
        out->writeFromInputStream(in, -1);
        out->flush();
    };
    root.createDirectory();
    auto file = root.getChildFile("Old negative wraps.soundplus");
    extract("multichannel-v141-spatial-v1.soundplus.gz", file);
    auto old = sp::loadDesign(file);
    auto source = root.getChildFile("old-constant.wav");
    extract("multichannel-v141-spatial-source.wav.gz", source);
    auto &fingerprint = old.takes[0].sources[0];
    check(hash(source) == fingerprint.sha256, "Historical constant source bytes");
    sp::FileStamp stamp;
    sp::fileStamp(source, stamp);
    fingerprint.overridePath = source.getFullPathName();
    fingerprint.overrideSize = stamp.size;
    fingerprint.overrideModified = stamp.modified;
    fingerprint.overrideInode = stamp.inode;
    auto originalRecipe = old.takes[0].recipeHash, originalEvent = old.takes[0].eventHash;
    auto oldPcm = pcm(old);
    check(old.takes[0].processing == "stock-layout-takes-v1" && old.takes[0].recipe->routingVersion == 1 &&
              oldPcm.getMagnitude(oldPcm.getNumSamples() - 512, 512) == 0,
          "Historical version1 retains its declared affected final silence");
    auto oldRender = sp::renderDesign(
        old, root.getChildFile("old-render"), old.renderSettings, [] { return false; }, [](double) {});
    check(oldRender.sha256 == old.outputs[0].sha256,
          "Historical version1 WAV bytes/receipt remain unchanged after math repair");
    Worker w;
    sp::DesignSession session(root.getChildFile("copy-session/library.sqlite"), w.services());
    session.document = old;
    session.selectTake(old.activeTake);
    session.createCorrectedSpatialCopy();
    w.drain();
    check(session.document.takes.size() == 2, "Explicit corrected-copy session action retains original");
    auto corrected = session.document;
    auto &copy = corrected.takes.back();
    check(copy.processing == "stock-layout-takes-v2" && copy.recipe->routingVersion == 2 &&
              copy.id != old.activeTake && copy.correctedFrom == old.activeTake &&
              copy.originalRecipeHash == originalRecipe && copy.originalEventHash == originalEvent &&
              copy.recipeHash != originalRecipe && copy.eventHash == originalEvent &&
              session.readyOutput() == juce::File(),
          "Corrected copy has explicit new processing/lineage and no inherited Ready render");
    session.undo();
    check(session.document.takes.size() == 1 && session.document.activeTake == old.activeTake,
          "Undo corrected copy restores old take context");
    session.redo();
    corrected = session.document;
    auto fixed = pcm(corrected);
    double oracleError = 0;
    std::array<double, 11> targets{0, -90, -180, 90, 0, -90, -180, 90, 0, -90, -130};
    for (size_t i = 0; i < targets.size(); ++i)
    {
        auto expected = spatialOracle(sp::channelLayout("5.1-side-v1"), targets[i], 0);
        for (int c = 0; c < 6; ++c)
            for (int n = (int)i * 1200 + 800; n < (int)i * 1200 + 1100; ++n)
                oracleError =
                    std::max(oracleError, std::abs(fixed.getSample(c, n) - .2f * expected[(size_t)c]));
    }
    check(oracleError < 2e-6,
          "Corrected historical sequence matches independent per-channel steady-state oracle");
    auto correctedFile = root.getChildFile("Corrected negative wraps.soundplus");
    session.document.outputFolder = root.getChildFile("corrected-render").getFullPathName();
    session.render();
    sp::CorrectionViewTestAccess::waitRender(session);
    w.drain();
    corrected = session.document;
    check(corrected.outputs.size() == old.outputs.size() + 1 &&
              corrected.outputs.back().processing == "stock-layout-takes-v2" &&
              corrected.outputs.back().channelProvenance["correctionOrigin"]["takeId"].toString() ==
                  old.activeTake &&
              session.readyOutput() == juce::File(corrected.outputs.back().path),
          "Corrected copy renders a new context-bound v2 receipt with original lineage");
    sp::saveDesignAtomic(correctedFile, corrected);
    check(sp::loadDesign(correctedFile).takes[0].recipeHash == originalRecipe &&
              sp::loadDesign(correctedFile).takes[1].correctedFrom == old.activeTake,
          "Both old identity and correction lineage persist");
    for (auto layout : sp::namedLayouts())
        if (sp::horizontalLayout(layout))
            for (double divergence : {0., .25, .8, 1.})
                for (double base : {-180., -130., -30., 0., 89.9, 180.})
                    for (int revolution = -1000; revolution <= 1000; revolution += 37)
                    {
                        auto angle = base + 360. * revolution;
                        auto actual = sp::spatialGains(layout, angle, divergence);
                        auto expected = spatialOracle(layout, angle, divergence);
                        double energy = 0;
                        for (int c = 0; c < layout.size(); ++c)
                        {
                            check(std::abs(actual[(size_t)c] - expected[(size_t)c]) < 2e-6,
                                  "Many positive/negative revolutions match periodic independent geometry");
                            energy += actual[(size_t)c] * actual[(size_t)c];
                            if (layout.roles[c] == "LFE")
                                check(actual[(size_t)c] == 0, "Panner excludes LFE at every divergence");
                        }
                        check(std::abs(energy - 1) < 2e-6,
                              "Periodic full-range panner stays nonzero/unit-energy");
                    }
    auto bus = sp::channelLayout("5.1-side-v1");
    for (double a : {-1e100, 1e100, -850., -130.})
    {
        auto g = sp::spatialGains(bus, a);
        double e = 0;
        for (auto v : g)
            e += v * v;
        check(std::abs(e - 1) < 2e-6, "Arbitrary finite accumulated position retains unit energy");
    }
    auto boundaryA = sp::spatialGains(bus, 180. - 1e-5), boundaryB = sp::spatialGains(bus, -180. + 1e-5);
    for (int c = 0; c < 6; ++c)
        check(std::abs(boundaryA[(size_t)c] - boundaryB[(size_t)c]) < 1e-6, "Principal boundary continuity");
    auto mono = root.getChildFile("constant-live.wav");
    constant(mono, {.2f});
    auto d = design(mono);
    d.original.masterDb = 0;
    sp::setMixLayout(d.original, bus);
    d.original.layers[0].routing.spatial = true;
    d.renderSettings.layout = "same";
    sp::DesignGraph graph(d);
    juce::AudioBuffer<float> block(6, 1536);
    double adoptionError = 0;
    for (double target : targets)
    {
        auto edited = d;
        edited.original.layers[0].routing.azimuth = target;
        graph.updateParameters(sp::designParameters(edited));
        block.clear();
        graph.render(block, 0, 1536);
        auto expected = spatialOracle(bus, target, 0);
        for (int c = 0; c < 6; ++c)
            for (int n = 1024; n < 1536; ++n)
                adoptionError =
                    std::max(adoptionError, std::abs(block.getSample(c, n) - .2f * expected[(size_t)c]));
    }
    check(adoptionError < 2e-6,
          "Real graph parameter adoption survives the reachable repeated negative-wrap sequence");
    for (auto levels :
         {std::vector<float>{.2f, 0.f}, std::vector<float>{0.f, .2f}, std::vector<float>{.2f, -.2f}})
        for (double divergence : {0., .3})
        {
            auto stereo = root.getChildFile("stereo-" + juce::Uuid().toString() + ".wav");
            constant(stereo, levels, 20000);
            auto image = design(stereo);
            image.original.masterDb = 0;
            sp::setMixLayout(image.original, bus);
            image.original.layers[0].routing.spatial = true;
            image.original.layers[0].routing.divergence = divergence;
            sp::DesignGraph stereoGraph(image);
            for (size_t i = 0; i < targets.size(); ++i)
            {
                double width = i % 2 ? 0 : 120;
                auto edited = image;
                auto &r = edited.original.layers[0].routing;
                r.azimuth = targets[i];
                r.width = width;
                stereoGraph.updateParameters(sp::designParameters(edited));
                block.clear();
                stereoGraph.render(block, 0, 1536);
                auto left = spatialOracle(bus, targets[i] - width / 2, r.divergence),
                     right = spatialOracle(bus, targets[i] + width / 2, r.divergence);
                for (int c = 0; c < 6; ++c)
                    check(std::abs(block.getSample(c, 1400) -
                                   (levels[0] * left[(size_t)c] + levels[1] * right[(size_t)c]) /
                                       std::sqrt(2.)) < 3e-6,
                          "Stereo images/width/divergence and anti-phase follow independent channel oracle");
            }
        }
    auto frozen = sp::freezeTake(d, "Controlled repeated wraps");
    frozen.generation = 7;
    sp::hashTake(frozen);
    auto capture = std::make_shared<sp::TakeCapture>(frozen);
    auto base = d;
    base.takeProcessing = true;
    sp::PcmStream producer(base, 48000, 0);
    producer.startTake(capture);
    std::array<std::atomic<int>, 16> identity;
    for (int c = 0; c < 16; ++c)
        identity[(size_t)c] = c;
    juce::AudioBuffer<float> consumed(6, (int)targets.size() * 1536), slice(6, 512);
    float gain = 1;
    int at = 0;
    for (double target : targets)
    {
        auto controls = sp::mixControls(d.original);
        controls.spatial[0].azimuth = target;
        producer.updatePerformance(controls);
        for (int k = 0; k < 3; ++k)
        {
            producer.fill(512);
            check(producer.read(slice.getArrayOfWritePointers(), 6, 512, gain, 1, &identity) == 512,
                  "Controlled six-channel consumption");
            for (int c = 0; c < 6; ++c)
                consumed.copyFrom(c, at, slice, c, 0, 512);
            at += 512;
        }
    }
    capture->finish(producer.acceptedFrame(), true, producer.consumedOutputFrames());
    auto recorded = capture->snapshot();
    auto controlled = d;
    controlled.takes = {recorded};
    controlled.activeTake = recorded.id;
    check(recorded.complete && recorded.processing == "stock-layout-takes-v2" && recorded.events.size() >= 10,
          "Repeated applied spatial motion recorded under explicit v2 identity");
    check(pcmDifference(consumed, pcm(controlled)) < 1e-6,
          "Controlled capture/replay preserves every intended channel");
    auto output = sp::renderDesign(
        controlled, root.getChildFile("controlled-render"), controlled.renderSettings, [] { return false; },
        [](double) {});
    controlled.outputs = {output};
    sp::saveDesignAtomic(root.getChildFile("Controlled wraps.soundplus"), controlled);
    // Actual helper/device capture. Monitoring is muted; assertions use pre-monitor PCM.
    sp::AudioClient audio(app);
    until([&] { return (double)audio.status()["sampleRate"] > 0; },
          "Native device available for wrap capture");
    audio.deviceRate(48000);
    audio.bufferSize(512);
    audio.gain(0);
    until(
        [&]
        {
            auto s = audio.status();
            return (double)s["sampleRate"] == 48000 && (int)s["bufferFrames"] == 512;
        },
        "Native wrap capture configured 48k/512");
    auto draft = d;
    auto take = sp::freezeTake(draft, "Native repeated negative wraps");
    take.checkpoint = root.getChildFile("native-checkpoint.soundplus").getFullPathName();
    draft.takes = {take};
    auto snapshot = root.getChildFile("Native draft.soundplus");
    sp::saveDesignAtomic(snapshot, draft);
    auto raw = root.getChildFile("native-original.f32");
    audio.reserveTake(true);
    auto generation = audio.playDesign("design:" + d.id, snapshot, 0, raw, "record");
    until([&] { return (bool)audio.status()["playing"] && (int)audio.status()["busChannels"] == 6; },
          "Native v2 six-channel capture starts");
    for (size_t i = 0; i < targets.size(); ++i)
    {
        auto before = audio.status();
        auto edited = d;
        edited.original.layers[0].routing.azimuth = targets[i];
        audio.updateDesign(edited, generation, sp::monotonicMs());
        if (i > 0)
            until([&] { return (int)audio.status()["takeEvents"] > (int)before["takeEvents"]; },
                  "Each negative-wrap target is applied/captured");
        auto frame = (int64_t)audio.status()["takeEnd"];
        until([&] { return (int64_t)audio.status()["takeEnd"] >= frame + 2048; },
              "Each native target settles beyond ten ms");
    }
    auto nativeStatus = audio.status();
    audio.finishTake();
    until(
        [&]
        {
            auto s = audio.status();
            return (int)s["takePhase"] == 3 && (bool)s["takeCheckpointReady"];
        },
        "Native wrap capture finalizes durably");
    audio.reserveTake(false);
    auto native = sp::loadDesign(juce::File(take.checkpoint)).takes[0];
    auto nativeDoc = d;
    nativeDoc.takes = {native};
    nativeDoc.activeTake = native.id;
    auto nativePcm = capturePcm(raw, (int)native.outputFrames);
    auto replay = pcm(nativeDoc);
    double nativeError = pcmDifference(nativePcm, replay, (int)native.outputFrames);
    auto expectedLast = spatialOracle(bus, -130, 0);
    double nativeOracleError = 0;
    for (int c = 0; c < 6; ++c)
        for (int n = nativePcm.getNumSamples() - 512; n < nativePcm.getNumSamples(); ++n)
            nativeOracleError = std::max(nativeOracleError,
                                         std::abs(nativePcm.getSample(c, n) - .2f * expectedLast[(size_t)c]));
    check(nativeError < 1e-6 && nativeOracleError < 2e-6,
          "Native capture/replay and final expected channel energy defeat shared-bug parity");
    auto nativeOutput = sp::renderDesign(
        nativeDoc, root.getChildFile("native-render"), nativeDoc.renderSettings, [] { return false; },
        [](double) {});
    nativeDoc.outputs = {nativeOutput};
    auto nativeFile = root.getChildFile("Native negative wraps.soundplus");
    sp::saveDesignAtomic(nativeFile, nativeDoc);
    auto replayRaw = root.getChildFile("native-replay.f32");
    audio.playDesign("design:" + nativeDoc.id, nativeFile, 0, replayRaw);
    until(
        [&]
        {
            auto state = audio.status();
            return (bool)state["playing"] && (bool)state["immutableReplay"];
        },
        "Actual helper adopts immutable v2 replay");
    until([&] { return !(bool)audio.status()["playing"]; }, "Actual helper completes immutable wrap replay");
    auto replayCapture = capturePcm(replayRaw, (int)native.outputFrames);
    check(pcmDifference(nativePcm, replayCapture, (int)native.outputFrames) < 1e-6,
          "Native pre-monitor replay matches the original consumed capture on every channel");
    audio.stop();
    // Adopt a v1 editable document through the actual asynchronous Open command.
    Worker migration;
    sp::DesignSession migrated(root.getChildFile("migration/library.sqlite"), migration.services());
    migrated.open(file);
    migration.drain();
    check(migrated.document.original.routingVersion == 2 && migrated.document.revision > old.revision &&
              migrated.dirty && migrated.document.takes[0].recipeHash == originalRecipe &&
              migrated.document.takes[0].recipe->routingVersion == 1,
          "Open migrates editable Live processing with retained revision while preserving frozen v1 take");
    Worker stale;
    sp::DesignSession switched(root.getChildFile("stale-copy/library.sqlite"), stale.services());
    switched.document = old;
    switched.selectTake(old.activeTake);
    switched.createCorrectedSpatialCopy();
    switched.selectTake({});
    stale.drain();
    check(switched.document.takes.size() == 1 && switched.document.activeTake.isEmpty() && !switched.working,
          "Held corrected-copy completion cannot steal a newly selected Live context");
    auto recoveryRoot = root.getChildFile("startup-migration");
    sp::DesignRecoveryStore(recoveryRoot.getChildFile("designer-autosave")).retain(old, file, false);
    Worker startup;
    auto startupServices = startup.services();
    startupServices.automaticRecovery = true;
    sp::DesignSession recovered(recoveryRoot.getChildFile("library.sqlite"), startupServices);
    startup.drain();
    check(recovered.document.original.routingVersion == 2 && recovered.dirty &&
              recovered.document.takes[0].recipeHash == originalRecipe,
          "Automatic recovery upgrades editable policy while preserving frozen identity");
    startup.now = 1000;
    recovered.poll();
    startup.drain();
    check(sp::loadDesign(recoveryRoot.getChildFile("designer-autosave/" + old.id + ".soundplus"))
                  .original.routingVersion == 2,
          "Migrated startup revision is actually retained in recovery");
    auto pendingRoot = root.getChildFile("startup-edit-guard");
    sp::DesignRecoveryStore(pendingRoot.getChildFile("designer-autosave")).retain(old, file, false);
    auto pendingFile = pendingRoot.getChildFile("designer-autosave/" + old.id + ".soundplus");
    auto pendingHash = hash(pendingFile);
    Worker held;
    auto heldServices = held.services();
    heldServices.automaticRecovery = true;
    sp::DesignSession fresh(pendingRoot.getChildFile("library.sqlite"), heldServices);
    auto freshId = fresh.document.id;
    fresh.editMetadata("Fresh user edit", [](auto &doc) { doc.name = "Fresh user work"; });
    held.drain();
    check(fresh.document.id == freshId && fresh.document.name == "Fresh user work" &&
              hash(pendingFile) == pendingHash,
          "Held policy migration cannot overwrite fresh startup edits or the retained original");
    auto evidence = juce::var(new juce::DynamicObject());
    auto *e = evidence.getDynamicObject();
    e->setProperty("oldWavSha256", oldRender.sha256);
    e->setProperty("oldFinalWindowSilent", true);
    e->setProperty("correctedOracleMaximumError", oracleError);
    e->setProperty("graphAdoptionMaximumError", adoptionError);
    e->setProperty("nativeCaptureReplayMaximumError", nativeError);
    e->setProperty("nativeOracleMaximumError", nativeOracleError);
    e->setProperty("nativeConsumedFrames", (juce::int64)native.outputFrames);
    e->setProperty("nativeTake", sp::takeToJson(native));
    e->setProperty("nativeStatus", nativeStatus);
    e->setProperty("nativeRaw", raw.getFullPathName());
    e->setProperty("nativeReplayRaw", replayRaw.getFullPathName());
    e->setProperty("nativeDocument", nativeFile.getFullPathName());
    e->setProperty("correctedDocument", correctedFile.getFullPathName());
    e->setProperty("oldRender", oldRender.path);
    e->setProperty("humanListening", false);
    e->setProperty("physicalSurroundMapping", false);
    return evidence;
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
    auto spatial = spatialCorrection(root.getChildFile("R12"), app);
    auto result = sp::message("multichannel-corrections-results");
    result.getDynamicObject()->setProperty("checks", checks);
    result.getDynamicObject()->setProperty("R12", spatial);
    result.getDynamicObject()->setProperty(
        "scenarios",
        juce::Array<juce::var>{"R11 lifetime preference churn, Delete/Undo/Redo/save/autosave/recovery"});
    auto scenarios = result["scenarios"];
    scenarios.append(
        "R17 historical legacy panel callbacks, split/interleaved/mono membership, save/reopen/Print");
    scenarios.append("R12 periodicity/oracle, mono/stereo/divergence, parameter adoption, native "
                     "capture/replay, old/new dispatch and corrected-copy provenance");
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
    d.original.routingVersion = 1;
    auto t = sp::freezeTake(d, "Repeated negative wraps v1");
    t.recipe = d.original;
    t.processing = "stock-layout-takes-v1";
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
