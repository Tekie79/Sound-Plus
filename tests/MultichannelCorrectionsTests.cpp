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
#include <CoreFoundation/CoreFoundation.h>
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
    static std::unique_ptr<juce::Component> routingPanel(DesignerView &v)
    {
        return v.makeRoutingPanel();
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
    static void candidate(LibraryView &v, const Asset &a)
    {
        v.selectedAsset = a;
        v.selectedId = a.id;
        v.rangeRequest = v.requested;
        v.table.deselectAllRows();
    }
    static void prepareAudition(LibraryView &v, const Asset &r, const Asset &c)
    {
        v.stopTimer();
        v.library.setChangeCallback({});
        v.cancelPendingUpdate();
        v.pinnedReference.source = r;
        v.pinnedReference.startSample = 1000;
        v.pinnedReference.endSample = r.samples - 1000;
        v.snapshot.reference.present = true;
        candidate(v, c);
    }
    static void reference(LibraryView &v, int64_t seek)
    {
        v.playReference(seek);
    }
    static void auditionMode(LibraryView &v, int id)
    {
        v.auditionChannels.setSelectedId(id, juce::sendNotificationSync);
    }
    static PlaybackRegion auditionSpec(LibraryView &v)
    {
        return v.playingSpec;
    }
    static juce::String auditionPath(LibraryView &v)
    {
        return v.playingPath;
    }
    static void interpretation(LibraryView &v, const Asset &a, const ChannelLayout &l)
    {
        v.updateAuditionInterpretation(a, l);
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
    check((int)sp::designToJson(reopened)["version"] == 9 && reopened.renderSettings.splitMono &&
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

void routingEditors(const juce::File &root, const juce::File &app)
{
    auto file = root.getChildFile("stereo.wav");
    wave(file, 2);
    auto d = design(file);
    d.original.masterDb = 0;
    sp::setMixLayout(d.original, sp::channelLayout("stereo-v1"));
    auto first = d.selectedLayer;
    d.original.layers[0].routing.routes = {{0, 0, -6.043, true, false}, {1, 1, -2.5, false, true}};
    auto second = sp::makeDesignLayer(sp::inspectDesignSource(file), 0, -1, true);
    sp::initialiseRouting(second, d.original.layout);
    second.name = "Other layer";
    second.mute = true;
    second.routing.routes = {{0, 1, -12, false, false}};
    d.original.layers.push_back(second);
    Worker worker;
    sp::AudioClient audio(app);
    juce::PropertiesFile prefs(root.getChildFile("view.settings"), sp::ui::preferenceOptions());
    sp::DesignerView view(root.getChildFile("library.sqlite"), audio, prefs);
    sp::CorrectionViewTestAccess::bind(view, d, worker.services());
    auto &s = sp::CorrectionViewTestAccess::session(view);
    auto panel = sp::CorrectionViewTestAccess::routingPanel(view);
    auto ctl = [&]<class T>(const char *name) -> T &
    { return sp::CorrectionViewTestAccess::control<T>(*panel, name); };
    auto refresh = []
    {
        // Exercise the production timer through the native UI dispatch loop.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .25, false);
    };
    auto route = [&](int input, int output) -> sp::ChannelRoute &
    {
        auto &v = s.document.original.layers[0].routing.routes;
        auto i =
            std::find_if(v.begin(), v.end(), [&](auto &r) { return r.input == input && r.output == output; });
        check(i != v.end(), "Edited route exists");
        return *i;
    };
    check(ctl.operator()<juce::ToggleButton>("Invert polarity").getToggleState() &&
              !ctl.operator()<juce::ToggleButton>("Mute route").getToggleState() &&
              std::abs(ctl.operator()<juce::Slider>("Route trim (dB)").getValue() + 6) < .001,
          "Routing construction loads selected actual trim/polarity/mute");
    auto &muted = ctl.operator()<juce::ToggleButton>("Mute route");
    muted.setToggleState(true, juce::dontSendNotification);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(route(0, 0).gainDb == -6.043 && route(0, 0).invert && route(0, 0).mute,
          "Mute-only edit preserves full-precision trim and polarity");
    check(pcm(s.document).getMagnitude(0, 4096) == 0, "Muted routes produce actual silent mix");
    s.undo();
    refresh();
    check(!muted.getToggleState() && ctl.operator()<juce::ToggleButton>("Invert polarity").getToggleState(),
          ("Undo refreshes actual selected route editors: model=" + juce::String((int)route(0, 0).mute) +
           " UI=" + juce::String((int)muted.getToggleState()))
              .toRawUTF8());
    s.redo();
    refresh();
    check(muted.getToggleState(), "Redo refreshes route mute");
    muted.setToggleState(false, juce::dontSendNotification);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    auto plain = d;
    plain.original.layers.resize(1);
    plain.original.layers[0].routing.routes.clear();
    auto source = pcm(plain);
    auto actual = pcm(s.document);
    for (int n = 0; n < 4096; ++n)
        check(std::abs(actual.getSample(0, n) + source.getSample(0, n) * std::pow(10., -6.043 / 20)) < 1e-7 &&
                  actual.getSample(1, n) == 0,
              "Route trim/polarity/mute state is audible in actual PCM");
    ctl.operator()<juce::ComboBox>("Route input").setSelectedId(2, juce::sendNotificationSync);
    ctl.operator()<juce::ComboBox>("Route output").setSelectedId(2, juce::sendNotificationSync);
    check(std::abs(ctl.operator()<juce::Slider>("Route trim (dB)").getValue() + 2.5) < .001 &&
              !ctl.operator()<juce::ToggleButton>("Invert polarity").getToggleState() &&
              muted.getToggleState(),
          "Input/output reselection loads the second route's distinct values");
    auto &invert = ctl.operator()<juce::ToggleButton>("Invert polarity");
    invert.setToggleState(true, juce::dontSendNotification);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(route(1, 1).gainDb == -2.5 && route(1, 1).invert && route(1, 1).mute,
          "Polarity-only edit preserves second route trim/mute");
    muted.setToggleState(false, juce::dontSendNotification);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    actual = pcm(s.document);
    for (int n = 0; n < 4096; ++n)
        check(std::abs(actual.getSample(1, n) + source.getSample(1, n) * std::pow(10., -2.5 / 20)) < 1e-7,
              "Unmuted second route produces its saved negative gain in PCM");
    auto &trim = ctl.operator()<juce::Slider>("Route trim (dB)");
    trim.setValue(-11.2, juce::sendNotificationSync);
    s.editMetadata("Unrelated rename", [](auto &doc) { doc.name += " renamed"; });
    refresh();
    check(std::abs(trim.getValue() + 11.2) < .001,
          "Uncommitted route draft survives timer/unrelated model refresh");
    ctl.operator()<juce::ComboBox>("Route output").setSelectedId(1, juce::sendNotificationSync);
    check(muted.getToggleState() && std::abs(trim.getValue()) < 1e-8,
          ("Absent explicit route appears as zero/muted: i=" +
           juce::String(ctl.operator()<juce::ComboBox>("Route input").getSelectedId()) +
           " o=" + juce::String(ctl.operator()<juce::ComboBox>("Route output").getSelectedId()) + " mute=" +
           juce::String((int)muted.getToggleState()) + " trim=" + juce::String(trim.getValue(), 12))
              .toRawUTF8());
    auto revision = s.document.revision;
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(s.document.revision == revision && s.document.original.layers[0].routing.routes.size() == 2,
          "Inspecting absent route cannot materialize unity");
    trim.setValue(-19, juce::sendNotificationSync);
    s.selectLayer(second.id);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(s.document.original.layers[1].routing.routes[0].gainDb == -12 && route(0, 0).gainDb == -6.043,
          "Queued stale editor cannot apply draft to newly selected layer");
    s.selectLayer(first);
    refresh();
    panel.reset();
    panel = sp::CorrectionViewTestAccess::routingPanel(view);
    check(ctl.operator()<juce::ToggleButton>("Invert polarity").getToggleState() &&
              std::abs(ctl.operator()<juce::Slider>("Route trim (dB)").getValue() + 6) < .001,
          "Close/reopen restores actual selected route state");
    s.edit("Implicit role routing", [](auto &doc) { doc.original.layers[0].routing.routes.clear(); });
    refresh();
    revision = s.document.revision;
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(s.document.original.layers[0].routing.routes.empty() && s.document.revision == revision,
          "Inspecting implicit role route does not materialize it");
    s.edit("Mono default coefficients",
           [](auto &doc) { sp::setMixLayout(doc.original, sp::channelLayout("mono-v1")); });
    refresh();
    auto &monoMute = ctl.operator()<juce::ToggleButton>("Mute route");
    monoMute.setToggleState(true, juce::dontSendNotification);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(std::abs(route(0, 0).gainDb - 20 * std::log10(.5)) < 1e-10 && route(0, 0).mute,
          "Mute on implicit half-gain preserves coefficient precision");
    monoMute.setToggleState(false, juce::dontSendNotification);
    ctl.operator()<juce::TextButton>("Set route").onClick();
    actual = pcm(s.document);
    for (int n = 0; n < 4096; ++n)
        check(std::abs(actual.getSample(0, n) - (source.getSample(0, n) + source.getSample(1, n)) * .5) <
                  2e-7,
              "Materialized default role routes preserve actual mono PCM");
    auto &monoTrim = ctl.operator()<juce::Slider>("Route trim (dB)");
    monoTrim.setValue(-20, juce::sendNotificationSync);
    auto other = design(file);
    sp::setMixLayout(other.original, sp::channelLayout("stereo-v1"));
    auto otherFile = root.getChildFile("Other document.soundplus");
    sp::saveDesignAtomic(otherFile, other);
    s.open(otherFile);
    worker.drain();
    ctl.operator()<juce::TextButton>("Set route").onClick();
    check(s.document.id == other.id && s.document.original.layers[0].routing.routes.empty(),
          "Document adoption safely rebinds and rejects stale route draft");
}

void replacement(const juce::File &root, const juce::File &app)
{
    auto folder = root.getChildFile("sources");
    folder.createDirectory();
    auto file = folder.getChildFile("replace.wav");
    wave(file, 6);
    auto database = root.getChildFile("library.sqlite");
    sp::LibraryService library(database);
    until([&] { return library.ready(); }, "Actual indexing service starts");
    library.addFolder(folder);
    auto ready = [&](int count, int64_t revision = 0)
    {
        sp::Asset a;
        until(
            [&]
            {
                sp::Catalog db(database, true);
                auto rows = db.search("");
                if (rows.size() != 1)
                    return false;
                a = rows[0];
                return a.state == "ready" && a.channels == count && a.revision >= revision;
            },
            "Actual discovery/metadata publishes replaced source");
        return a;
    };
    auto a = ready(6);
    auto id = a.id;
    auto overrideWith = [&](sp::Asset source, const sp::ChannelLayout &layout)
    {
        library.setLayoutOverride(source, layout);
        until(
            [&]
            {
                sp::Catalog db(database, true);
                return db.asset(id).layoutOverride == layout;
            },
            "Actual queued override commits for matching revision");
        return ready(source.channels);
    };
    a = overrideWith(a, sp::channelLayout("5.1-side-v1"));
    auto frozen = design(file);
    frozen.original.layers[0].source = a;
    auto originalFrozen = sp::designToJson(frozen).toString();
    auto frozenSource = frozen.original.layers[0].source;
    auto old = a;
    int replacementSerial = 0;
    auto replace = [&](int channels)
    {
        check(file.deleteFile(), "Replace owned fixture only");
        wave(file, channels, 4800 + channels + ++replacementSerial * 31);
        library.rescan();
        a = ready(channels, a.revision + 1);
        check(a.id == id && a.layoutOverride.id.isEmpty(),
              "Changed source revision uses safe detected layout; stable catalog identity");
    };
    replace(2);
    library.setLayoutOverride(old, sp::channelLayout("5.1-side-v1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    a = ready(2);
    check(a.layoutOverride.id.isEmpty(), "Delayed stale six-channel override cannot reach stereo revision");
    a = overrideWith(a, sp::discreteLayout(2));
    old = a;
    replace(6);
    library.setLayoutOverride(old, sp::discreteLayout(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    a = ready(6);
    check(a.layoutOverride.id.isEmpty(), "Delayed stereo override cannot reach six-channel revision");
    a = overrideWith(a, sp::channelLayout("5.1-side-v1"));
    replace(6);
    a = overrideWith(a, sp::channelLayout("5.1-side-v1"));
    auto rev = a.revision;
    auto sha = hash(file);
    library.rescan();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    a = ready(6);
    check(a.revision == rev && a.layoutOverride.id == "5.1-side-v1",
          "Unchanged reopen/rescan retains reviewed interpretation");
    auto renamed = folder.getChildFile("renamed.wav");
    check(file.moveFileTo(renamed), "Owned same-inode rename");
    file = renamed;
    library.rescan();
    until(
        [&]
        {
            sp::Catalog db(database, true);
            a = db.asset(id);
            return a.relative == "renamed.wav";
        },
        "Indexer recognizes rename");
    check(a.revision == rev && a.layoutOverride.id == "5.1-side-v1" && hash(file) == sha,
          "Rename retains identity/override and byte hash");
    auto nested = folder.getChildFile("relocated");
    nested.createDirectory();
    auto moved = nested.getChildFile(file.getFileName());
    check(file.moveFileTo(moved), "Owned same-byte relocation");
    file = moved;
    library.rescan();
    until(
        [&]
        {
            sp::Catalog db(database, true);
            a = db.asset(id);
            return a.relative == "relocated/renamed.wav";
        },
        "Indexer resolves same-inode folder relocation");
    check(a.revision == rev && a.layoutOverride.id == "5.1-side-v1" && hash(file) == sha,
          "Verified same-byte same-inode relocation retains interpretation");
    check(frozen.original.layers[0].source.revision == frozenSource.revision &&
              frozen.original.layers[0].source.layoutOverride == frozenSource.layoutOverride,
          "Catalog updates cannot rewrite a frozen layer source");
    // Recover a catalog left by the reviewed bug, then exercise metadata's guard.
    {
        sp::Catalog db(database);
        db.exec("UPDATE assets SET layout_override='{\"id\":\"stereo-v1\",\"roles\":[\"L\",\"R\"]}'");
        auto stale = db.asset(id);
        check(stale.layoutOverride.id.isEmpty(), "Already incompatible override is never exposed");
        db.metadata(stale, a.duration, 6, a.sampleRate, {}, a.samples);
        check(db.asset(id).layoutOverride.id.isEmpty(),
              "Metadata refresh clears incompatible historical override");
    }
    a = ready(6);
    // Actual Designer Add retains complete supported-source semantics.
    Worker worker;
    sp::DesignSession session(root.getChildFile("session/library.sqlite"), worker.services());
    session.add(a, 0, a.samples, false, -2);
    worker.drain();
    check(session.document.activeMix().layers.size() == 1 &&
              session.document.activeMix().layers[0].routing.inputs.size() == 6,
          "Designer Add succeeds after replacement with full supported source");
    auto b = pcm(session.document);
    check(b.getMagnitude(0, b.getNumSamples()) > 0, "Replaced source is usable in actual Designer graph");
    sp::PcmStream stream(file, 48000, 0, -1, -2, a.layoutOverride);
    stream.fill();
    check(stream.available() > 0 && stream.channelCount() == 6,
          "Detected fallback auditions complete source without stale count mismatch");
    auto state = sp::message("R14");
    state.getDynamicObject()->setProperty("id", id);
    state.getDynamicObject()->setProperty("revision", a.revision);
    state.getDynamicObject()->setProperty("path", file.getFullPathName());
    root.getChildFile("replacement-evidence.json").replaceWithText(juce::JSON::toString(state, true));
}
void auditionOwner(const juce::File &root, const juce::File &app)
{
    root.createDirectory();
    auto file = root.getChildFile("reference-six.wav");
    constant(file, {.01f, .02f, .03f, .04f, .05f, .06f}, 48000 * 8);
    auto reference = sp::inspectDesignSource(file);
    reference.layoutOverride = sp::channelLayout("5.1-side-v1");
    auto candidateFile = root.getChildFile("candidate.wav");
    wave(candidateFile, 2);
    auto candidate = sp::inspectDesignSource(candidateFile);
    sp::LibraryView view(root.getChildFile("library.sqlite"), app, false);
    sp::CorrectionViewTestAccess::prepareAudition(view, reference, candidate);
    auto &audio = view.audioClient();
    audio.gain(0);
    until([&] { return audio.status()["device"].toString().isNotEmpty(); }, "Native reference helper opens");
    sp::CorrectionViewTestAccess::reference(view, 1000);
    // A mode change during pending adoption must preserve reference ownership.
    sp::CorrectionViewTestAccess::auditionMode(view, 1);
    auto assertSpec = [&](int pair)
    {
        auto spec = sp::CorrectionViewTestAccess::auditionSpec(view);
        check(sp::CorrectionViewTestAccess::auditionPath(view) == file.getFullPathName() &&
                  spec.auditionPair == pair && spec.sourceLayoutOverride == "5.1-side-v1" &&
                  spec.revision == reference.revision && spec.startSample == 1000 &&
                  spec.endSample == reference.samples - 1000,
              "Actual replay specification stays with reference path/range/revision/interpretation");
        sp::PcmStream stream(file, 48000, spec.startSample, spec.endSample, pair,
                             sp::channelLayout(spec.sourceLayoutOverride));
        stream.fill(1024);
        juce::AudioBuffer<float> b(2, 1024);
        float gain = 1;
        stream.read(b.getArrayOfWritePointers(), 2, 1024, gain, 1);
        auto matrix = sp::conversionMatrix(reference.layoutOverride, sp::channelLayout("stereo-v1"));
        for (int c = 0; c < 2; ++c)
        {
            double expected = 0;
            if (pair == -1)
                expected = .035;
            else if (pair >= 0)
                expected = (pair * 2 + c + 1) * .01;
            else
                for (int i = 0; i < 6; ++i)
                    expected += matrix[c][i] * (i + 1) * .01;
            for (int n = 0; n < 1024; ++n)
                check(std::abs(b.getSample(c, n) - expected) < 1e-7,
                      "Reference audition has expected independent channel PCM");
        }
        until(
            [&]
            {
                auto s = audio.status();
                return (bool)s["playing"] && s["id"].toString() == "reference-" + reference.id &&
                       (int)s["auditionPair"] == pair;
            },
            "Native helper adopts actual reference mode");
    };
    assertSpec(-1);
    for (auto layout : {sp::ChannelLayout{}, sp::discreteLayout(6), sp::channelLayout("stereo-v1")})
    {
        candidate.layoutOverride = layout;
        sp::CorrectionViewTestAccess::candidate(view, candidate);
        sp::CorrectionViewTestAccess::auditionMode(view, 2);
        assertSpec(0);
        sp::CorrectionViewTestAccess::auditionMode(view, 100);
        assertSpec(-2);
    }
    auto generation = audio.status()["intentGeneration"];
    sp::CorrectionViewTestAccess::interpretation(view, candidate, sp::discreteLayout(2));
    check(audio.status()["intentGeneration"] == generation,
          "Candidate interpretation cannot restart reference stream");
    auto newer = reference;
    ++newer.revision;
    sp::CorrectionViewTestAccess::interpretation(view, newer, sp::discreteLayout(6));
    check(audio.status()["intentGeneration"] == generation,
          "New source revision cannot mutate old active owner");
    sp::CorrectionViewTestAccess::interpretation(view, reference, sp::discreteLayout(6));
    check(sp::CorrectionViewTestAccess::auditionSpec(view).sourceLayoutOverride == sp::discreteLayout(6).id,
          "Matching owner interpretation edit updates exact revision");
    audio.stop();
    generation = audio.status()["intentGeneration"];
    sp::CorrectionViewTestAccess::candidate(view, candidate);
    sp::CorrectionViewTestAccess::auditionMode(view, 1);
    check(audio.status()["intentGeneration"] == generation, "Stopped selection/mode change stays silent");
    // A different intent, including Designer, cannot be stolen by this stale Library view.
    audio.playRegion("design:protected", candidateFile, {});
    generation = audio.status()["intentGeneration"];
    sp::CorrectionViewTestAccess::auditionMode(view, 2);
    check(audio.status()["intentGeneration"] == generation &&
              audio.status()["intentId"].toString() == "design:protected",
          "Protected Designer/other audition intent cannot be stolen");
    audio.stop();
}

void legacyConversion(const juce::File &root, const juce::File &app)
{
    int cases = 0;
    double maximum = 0;
    for (int channels : {1, 2, 3, 4, 5, 6, 16, 63, 64})
    {
        std::vector<float> levels;
        for (int c = 0; c < channels; ++c)
            levels.push_back(.001f * (c + 1));
        auto file = root.getChildFile("source-" + juce::String(channels) + ".wav");
        file.getParentDirectory().createDirectory();
        constant(file, levels, 4096);
        for (bool known : {false, true})
        {
            if (known && channels != 2 && channels != 6)
                continue;
            for (int pair : {-1, 0, (channels - 1) / 2})
            {
                auto legacy = design(file);
                legacy.original.masterDb = 0;
                auto &layer = legacy.original.layers[0];
                layer.region.auditionPair = pair;
                layer.pan = .37;
                if (known)
                    layer.source.layoutOverride =
                        sp::channelLayout(channels == 6 ? "5.1-side-v1" : "stereo-v1");
                auto old = pcm(legacy);
                for (auto target : {sp::channelLayout("stereo-v1"), sp::channelLayout("5.1-side-v1")})
                {
                    auto converted = legacy;
                    sp::setMixLayout(converted.original, target);
                    converted.renderSettings.layout = "same";
                    auto &r = converted.original.layers[0].routing;
                    bool mono = pair < 0 || pair * 2 + 1 >= channels;
                    check(r.interpretation.id == (mono ? "mono-v1" : "stereo-v1") &&
                              r.legacyAnalysisMean == (pair < 0),
                          "Converted legacy interpretation is explicit Mono mean or original physical pair");
                    if (pair < 0)
                    {
                        check((int)r.inputs.size() == channels,
                              "Analysis mean retains all physical source indices");
                        for (int i = 0; i < channels; ++i)
                            check(r.inputs[(size_t)i] == i, "Analysis physical input order");
                    }
                    else
                    {
                        check(r.inputs[0] == pair * 2 && (int)r.inputs.size() == (mono ? 1 : 2),
                              "Non-first and odd-final legacy physical pair retained");
                        if (!mono)
                            check(r.inputs[1] == pair * 2 + 1, "Second physical pair member retained");
                    }
                    auto actual = pcm(converted);
                    double mean = 0;
                    for (auto level : levels)
                        mean += level;
                    mean /= channels;
                    for (int c = 0; c < target.size(); ++c)
                    {
                        double expected = 0;
                        if (target.id == "stereo-v1")
                        {
                            if (mono)
                                expected =
                                    (pair < 0 ? mean : levels[(size_t)pair * 2]) *
                                    (c == 0
                                         ? std::cos((layer.pan + 1) * juce::MathConstants<double>::pi / 4)
                                         : std::sin((layer.pan + 1) * juce::MathConstants<double>::pi / 4));
                            else
                                expected =
                                    levels[(size_t)pair * 2 + c] *
                                    (c == 0 ? std::cos(layer.pan * juce::MathConstants<double>::halfPi) : 1);
                        }
                        else if (mono && c == 2)
                            expected = pair < 0 ? mean : levels[(size_t)pair * 2];
                        else if (!mono && c < 2)
                            expected = levels[(size_t)pair * 2 + c];
                        for (int n = 0; n < 4096; ++n)
                        {
                            maximum = std::max(maximum, std::abs(actual.getSample(c, n) - expected));
                            check(std::abs(actual.getSample(c, n) - expected) < 1e-7,
                                  "Converted signal matches independent pair/mean and bus oracle");
                        }
                    }
                    if (target.id == "stereo-v1")
                        check(pcmDifference(old, actual, 4096) < 1e-7,
                              "Converted legacy Stereo preserves actual historical PCM");
                    auto saved = root.getChildFile("Converted.soundplus");
                    sp::saveDesignAtomic(saved, converted);
                    auto reopened = sp::loadDesign(saved);
                    check(reopened.original.layers[0].routing.legacyAnalysisMean == r.legacyAnalysisMean &&
                              reopened.original.layers[0].routing.inputs == r.inputs &&
                              pcmDifference(actual, pcm(reopened), 4096) == 0,
                          "Converted inputs and PCM persist through real save/reopen");
                    ++cases;
                }
            }
        }
    }
    auto anti = root.getChildFile("anti-phase.wav");
    constant(anti, {.2f, -.2f}, 4096);
    auto mean = design(anti);
    mean.original.layers[0].region.auditionPair = -1;
    sp::setMixLayout(mean.original, sp::channelLayout("stereo-v1"));
    check(pcm(mean).getMagnitude(0, 4096) == 0,
          "Legacy stereo Analysis Mix preserves arithmetic anti-phase cancellation");
    auto version1 = mean;
    version1.original.routingVersion = 1;
    check(rejects([&] { sp::validateDesign(version1); }),
          "Mean conversion cannot masquerade under old routing identity");
    auto file = root.getChildFile("source-6.wav");
    auto legacy = design(file);
    legacy.name = "Legacy pair and analysis conversion";
    legacy.original.masterDb = 0;
    legacy.original.layers[0].region.auditionPair = 1;
    auto second = legacy.original.layers[0];
    second.id = juce::Uuid().toString();
    second.region.auditionPair = -1;
    second.placement = 4800;
    legacy.original.layers.push_back(second);
    auto historicalDocument = historical(root.getChildFile("immutable"), app);
    legacy.takes = historicalDocument.takes;
    legacy.outputs = historicalDocument.outputs;
    auto oldRecipe = legacy.takes[0].recipeHash, oldEvents = legacy.takes[0].eventHash,
         oldOutput = legacy.outputs.back().sha256;
    sp::saveDesignAtomic(root.getChildFile("Legacy before conversion.soundplus"), legacy);
    Worker worker;
    sp::DesignSession session(root.getChildFile("session/library.sqlite"), worker.services());
    session.document = legacy;
    session.edit("Convert legacy inputs",
                 [](auto &doc)
                 {
                     sp::setMixLayout(doc.original, sp::channelLayout("5.1-side-v1"));
                     doc.renderSettings.layout = "same";
                 });
    check(session.document.original.layers[0].routing.inputs == std::vector<int>({2, 3}) &&
              session.document.original.layers[1].routing.legacyAnalysisMean,
          "One undoable conversion preserves pair and mean across whole mix");
    session.undo();
    check(session.document.original.layout.id.isEmpty() &&
              session.document.original.layers[0].region.auditionPair == 1 &&
              session.document.original.layers[1].region.auditionPair == -1,
          "Undo restores entire legacy mix");
    session.redo();
    session.save(root.getChildFile("Legacy converted.soundplus"));
    worker.drain();
    auto reopened = sp::loadDesign(root.getChildFile("Legacy converted.soundplus"));
    check(reopened.takes[0].recipeHash == oldRecipe && reopened.takes[0].eventHash == oldEvents &&
              reopened.outputs.back().sha256 == oldOutput,
          "Conversion/save/Undo/Redo retain historical frozen take and output identities");
    auto invalid = legacy;
    invalid.original.layers[1].region.auditionPair = 31;
    auto before = juce::JSON::toString(sp::designToJson(invalid), true);
    check(rejects([&] { sp::setMixLayout(invalid.original, sp::channelLayout("5.1-side-v1")); }) &&
              before == juce::JSON::toString(sp::designToJson(invalid), true),
          "Unrepresentable later layer leaves entire legacy mix unchanged");
    auto state = sp::message("R16");
    state.getDynamicObject()->setProperty("cases", cases);
    state.getDynamicObject()->setProperty("maximumOracleError", maximum);
    root.getChildFile("conversion-evidence.json").replaceWithText(juce::JSON::toString(state, true));
}

void nativePreferenceCapture(const juce::File &root, const juce::File &app, sp::DesignDocument d)
{
    Worker worker;
    sp::AudioClient audio(app);
    audio.gain(0);
    juce::PropertiesFile prefs(root.getChildFile("view.settings"), sp::ui::preferenceOptions());
    sp::DesignerView view(root.getChildFile("library.sqlite"), audio, prefs);
    sp::CorrectionViewTestAccess::bind(view, d, worker.services());
    auto &session = sp::CorrectionViewTestAccess::session(view);
    session.selectTake(d.takes.front().id);
    session.deleteTake();
    check(session.document.takes.size() == 15 && session.document.outputSelections.size() == 16,
          "Native replacement capture starts with Live plus fifteen valid take preferences");
    session.recordTake();
    worker.drain();
    until(
        [&]
        {
            session.poll();
            worker.drain();
            return session.takeState == sp::DesignSession::TakeState::ready ||
                   session.takeState == sp::DesignSession::TakeState::incomplete;
        },
        "Actual session/helper capture reaches durable terminal state");
    check(session.takeState == sp::DesignSession::TakeState::ready,
          ("Native capture with retained preferences must be Ready: " + session.message).toRawUTF8());
    auto *t = sp::selectedTake(session.document);
    check(t && t->complete && t->outputFrames > 0 && session.document.takes.size() == 16 &&
              session.document.outputSelections.size() == 16,
          "Temporary capture graph clears removed-context preferences without mutating document preferences");
    session.document.outputFolder = root.getChildFile("outputs").getFullPathName();
    session.render();
    sp::CorrectionViewTestAccess::waitRender(session);
    worker.drain();
    auto output = session.readyOutput();
    check(output.existsAsFile(), "New preference-bearing native take renders successfully");
    session.chooseOutput(output.getFullPathName());
    auto saved = root.getChildFile("Native preference capture.soundplus");
    session.save(saved);
    worker.drain();
    auto reopened = sp::loadDesign(saved);
    check(reopened.outputSelections.size() == 17 && reopened.takes.size() == 16 &&
              sp::selectedTake(reopened)->complete,
          "Actual native replacement capture and explicit output restore bounded preferences/save/reopen");
    root.getChildFile("evidence.json")
        .replaceWithText(juce::JSON::toString(sp::takeToJson(*sp::selectedTake(reopened)), true));
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
    nativePreferenceCapture(root.getChildFile("R11-native"), app,
                            sp::loadDesign(root.getChildFile("R11/Lifetime preferences.soundplus")));
    legacySplit(root.getChildFile("R17"), app);
    auto spatial = spatialCorrection(root.getChildFile("R12"), app);
    routingEditors(root.getChildFile("R13"), app);
    replacement(root.getChildFile("R14"), app);
    auditionOwner(root.getChildFile("R15"), app);
    legacyConversion(root.getChildFile("R16"), app);
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
    scenarios.append("R13 actual route editors: precision, one-field edits, implicit/absent, drafts, "
                     "selection/context, Undo/Redo and PCM");
    scenarios.append(
        "R14 actual index replacement, overrides, revisions, rename/relocation and Designer Add");
    scenarios.append("R15 reference owner mode transitions, native adoption and independent audition PCM");
    scenarios.append("R16 physical pairs, odd finals, all-channel mean, PCM, transactional conversion and "
                     "immutable historical identities");
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
