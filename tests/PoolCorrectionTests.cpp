#include "DesignerView.h"
#include "DesignerCanvas.h"
#include "PoolMappingReview.h"
#include "DesignLive.h"
#include "Performance.h"
#include "AudioStream.h"
#include "LibraryService.h"
#include <CoreFoundation/CoreFoundation.h>
#include <juce_cryptography/juce_cryptography.h>
#include <deque>
#include <iostream>
namespace sp
{
struct PoolCorrectionViewAccess
{
    static void bind(DesignerView &v, const DesignDocument &d, DesignSessionServices services)
    {
        v.stopTimer();
        v.session.stopTimer();
        v.session.protectStartupEdits();
        v.session.document = d;
        v.session.selectedIds = {d.selectedLayer};
        v.session.services = std::move(services);
        v.refresh();
    }
    static void native(DesignerView &v, const DesignDocument &d)
    {
        v.stopTimer();
        v.session.protectStartupEdits();
        v.session.document = d;
        v.session.selectedIds = {d.selectedLayer};
        v.refresh();
    }
    static void print(DesignerView &v)
    {
        v.addPrint.onClick();
    }
    static auto routing(DesignerView &v)
    {
        return v.makeRoutingPanel();
    }
    static auto review(DesignerView &v, const juce::String &id)
    {
        return v.makePoolReviewPanel(id);
    }
};
} // namespace sp
namespace
{
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
juce::String text(const juce::var &v)
{
    return juce::JSON::toString(v, true);
}
sp::Asset wave(const juce::File &file, int channels, int frames = 960000, bool untagged = false)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    auto stream = file.createOutputStream();
    if (channels > 16 || untagged)
    {
        int bytes = channels * frames * 4;
        stream->write("RIFF", 4);
        stream->writeInt(36 + bytes);
        stream->write("WAVEfmt ", 8);
        stream->writeInt(16);
        stream->writeShort(3);
        stream->writeShort((short)channels);
        stream->writeInt(48000);
        stream->writeInt(48000 * channels * 4);
        stream->writeShort((short)(channels * 4));
        stream->writeShort(32);
        stream->write("data", 4);
        stream->writeInt(bytes);
        for (int n = 0; n < frames; ++n)
            for (int c = 0; c < channels; ++c)
                stream->writeFloat((float)(.01 * (c + 1) * std::sin(n * (.019 + c * .003))));
        stream.reset();
        return sp::inspectDesignSource(file);
    }
    juce::WavAudioFormat f;
    std::unique_ptr<juce::AudioFormatWriter> w(
        f.createWriterFor(stream.release(), 48000, (unsigned)channels, 32, {}, 0));
    juce::AudioBuffer<float> b(channels, 4096);
    for (int at = 0; at < frames; at += 4096)
    {
        int n = std::min(4096, frames - at);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                b.setSample(c, i, (float)(.035 * (c + 1) * std::sin((at + i) * (.019 + c * .003))));
        check(w->writeFromAudioSampleBuffer(b, 0, n), "Generated channel-tag fixture finalized");
    }
    w.reset();
    return sp::inspectDesignSource(file);
}
sp::DesignDocument design(const sp::Asset &a)
{
    sp::DesignDocument d;
    d.name = "Pool corrections QA";
    d.original.masterDb = 0;
    d.original.layers = {sp::makeDesignLayer(a, 0, a.samples, true)};
    d.selectedLayer = d.original.layers[0].id;
    sp::setMixLayout(d.original, sp::channelLayout("5.1-side-v1"));
    d.renderSettings.bits = 32;
    d.renderSettings.tailSeconds = 0;
    d.renderSettings.layout = "same";
    d.renderSettings.channels = 6;
    auto &l = d.original.layers[0];
    l.routing.spatial = true;
    l.routing.azimuth = -130;
    l.routing.width = 75;
    l.routing.divergence = .2;
    l.routing.lfeDb = -12;
    l.routing.lfeCutoff = 87;
    l.pool.emplace();
    auto e = sp::poolEntry(l, d.original.layout);
    sp::verifyPoolEntry(e);
    l.pool->selected = e.id;
    l.pool->entries = {e};
    d.playhead = 96000;
    sp::validateDesign(d);
    return d;
}
struct Harness
{
    std::deque<std::function<void()>> jobs, posts;
    double now = 1000;
    int stops = 0, plays = 0, parameters = 0;
    uint64_t generation = 0;
    juce::var state = sp::message("status");
    std::unique_ptr<sp::DesignGraph> graph;
    sp::DesignDocument adopted;
    sp::DesignSessionServices services()
    {
        sp::DesignSessionServices s;
        s.automaticRecovery = s.timers = false;
        s.submit = [this](auto f)
        {
            jobs.push_back(std::move(f));
            return true;
        };
        s.post = [this](auto f) { posts.push_back(std::move(f)); };
        s.now = [this] { return now; };
        s.resolve = [](auto &) {};
        s.retain = [](const auto &, const auto &, bool) {};
        s.write = [](const auto &f, const auto &d) { sp::saveDesignAtomic(f, d); };
        s.reserveTake = [](bool) {};
        s.finishTake = [](bool) {};
        s.status = [this] { return state.clone(); };
        s.stop = [this]
        {
            ++stops;
            state.getDynamicObject()->setProperty("playing", false);
        };
        s.play = [this](const auto &id, const auto &f, int64_t at, const auto &)
        {
            ++plays;
            adopted = sp::loadDesign(f);
            graph = std::make_unique<sp::DesignGraph>(adopted);
            graph->seek(at);
            auto *o = state.getDynamicObject();
            o->setProperty("id", id);
            o->setProperty("playing", true);
            o->setProperty("playGeneration", (juce::int64)++generation);
            o->setProperty("sourceSample", (juce::int64)at);
            return generation;
        };
        s.parameters = [this](const auto &d, uint64_t gen, double)
        {
            check(gen == generation, "Live parameters adopt the owned generation");
            ++parameters;
            adopted = d;
            graph->updateParameters(sp::designParameters(d));
        };
        return s;
    }
    void drain()
    {
        while (!jobs.empty() || !posts.empty())
        {
            auto &q = jobs.empty() ? posts : jobs;
            auto f = std::move(q.front());
            q.pop_front();
            f();
        }
    }
    void poll(sp::DesignSession &s)
    {
        now += 100;
        s.poll();
        drain();
    }
};
template <class T> T &control(juce::Component &p, const juce::String &name)
{
    for (auto *c : p.getChildren())
        if (auto *t = dynamic_cast<T *>(c); t && (t->getTitle() == name || t->getName() == name))
            return *t;
    throw std::runtime_error(("Missing production control " + name).toStdString());
}
juce::MouseEvent pointer(sp::DesignerCanvas &c, juce::Point<float> p, juce::Point<float> down)
{
    return {juce::Desktop::getInstance().getMainMouseSource(),
            p,
            juce::ModifierKeys::leftButtonModifier,
            1,
            0,
            0,
            0,
            0,
            &c,
            &c,
            juce::Time::getCurrentTime(),
            down,
            juce::Time::getCurrentTime(),
            1,
            p != down};
}
double compare(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b, int first = 0)
{
    check(a.getNumChannels() == b.getNumChannels() && a.getNumSamples() == b.getNumSamples(),
          "PCM dimensions agree");
    double error = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = first; n < a.getNumSamples(); ++n)
            error = std::max(error, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
    return error;
}
void adopted(Harness &h, sp::DesignSession &s)
{
    auto expected = sp::audioSourceProjection(s.document);
    sp::DesignGraph reference(expected);
    reference.seek(h.graph->position());
    juce::AudioBuffer<float> a(6, 4096), b(6, 4096);
    h.graph->render(a, 0, 4096);
    reference.render(b, 0, 4096);
    check(compare(a, b, 2048) < 1e-6, "Actual updated graph settles to expected panner/trim PCM");
    auto &l = s.document.activeMix().layers[0];
    auto &e = l.pool->entries[0];
    check(text(sp::routingJson(e.routing)) == text(sp::routingJson(l.routing)) &&
              e.region.startSample == l.region.startSample && e.region.endSample == l.region.endSample,
          "Live metadata and selected entry remain synchronized");
}
void live(const juce::File &root, const sp::Asset &a, sp::AudioClient &client)
{
    juce::PropertiesFile prefs(root.getChildFile("prefs"), sp::ui::preferenceOptions());
    sp::DesignerView view(root.getChildFile("catalog/library.sqlite"), client, prefs);
    Harness h;
    auto d = design(a);
    sp::PoolCorrectionViewAccess::bind(view, d, h.services());
    auto &s = view.session;
    s.play();
    h.drain();
    check(s.ownsPlayback() && h.plays == 1, "Owned pooled Live playback begins");
    auto panel = sp::PoolCorrectionViewAccess::routing(view);
    auto &angle = control<juce::Slider>(*panel, "Spatial azimuth (degrees)");
    auto &width = control<juce::Slider>(*panel, "Stereo image width (degrees)");
    for (auto [slider, value] : {std::pair{&angle, 65.}, std::pair{&width, 160.}})
    {
        slider->onDragStart();
        slider->setValue(value, juce::sendNotificationSync);
        slider->onDragEnd();
        h.poll(s);
        check(s.ownsPlayback() && h.stops == 0 && h.plays == 1 && h.generation == 1,
              "R18 actual spatial callback must keep playing on the same generation");
        adopted(h, s);
        s.undo();
        h.poll(s);
        check(s.ownsPlayback() && h.stops == 0 && h.plays == 1, "R18 panner Undo keeps Live playback");
        adopted(h, s);
        s.redo();
        h.poll(s);
        check(s.ownsPlayback() && h.stops == 0 && h.plays == 1, "R18 panner Redo keeps Live playback");
        adopted(h, s);
    }
    sp::DesignerCanvas canvas(
        s, [](const auto &) -> std::shared_ptr<const sp::WaveformData> { return {}; }, prefs);
    canvas.setSize(1000, 180);
    canvas.viewport.pixelsPerSecond = 35;
    canvas.viewport.startSeconds = 0;
    canvas.snapping.enabled = false;
    for (bool left : {true, false})
    {
        auto shape = sp::clipGeometry(s.document.activeMix().layers[0], 0, canvas.viewport, 48000);
        juce::Point<float> from(left ? shape.left + 1 : shape.right - 1, shape.clipTop + shape.height * .6f),
            to = from + juce::Point<float>(left ? 18.f : -18.f, 0);
        canvas.mouseDown(pointer(canvas, from, from));
        canvas.mouseDrag(pointer(canvas, to, from));
        canvas.mouseUp(pointer(canvas, to, from));
        h.poll(s);
        check(s.ownsPlayback() && h.stops == 0 && h.plays == 1, "R18 actual trim gesture must keep playing");
        check(left ? s.document.activeMix().layers[0].region.startSample > 0
                   : s.document.activeMix().layers[0].region.endSample < a.samples,
              "Actual canvas changed the intended source edge");
        adopted(h, s);
        s.undo();
        h.poll(s);
        check(s.ownsPlayback() && h.stops == 0, "R18 trim Undo keeps playing");
        s.redo();
        h.poll(s);
        check(s.ownsPlayback() && h.stops == 0 && h.plays == 1, "R18 trim Redo keeps playing");
        adopted(h, s);
    }
    auto &l = s.document.activeMix().layers[0];
    auto other = l.pool->entries.front();
    other.id = juce::Uuid().toString();
    other.region.startSample = 48000;
    other.region.endSample = 720000;
    l.pool->entries.push_back(other);
    s.selectPoolEntry(l.id, other.id);
    h.drain();
    check(!s.ownsPlayback() && h.stops == 1,
          "Different entry sharing a file but different range still stops");
    s.play();
    h.drain();
    auto source = juce::File(a.path), absent = source.getSiblingFile("temporarily-absent.wav");
    check(source.moveFileTo(absent), "Owned fixture temporarily absent");
    auto old = s.document.activeMix().layers[0].pool->selected;
    s.selectPoolEntry(l.id, l.pool->entries[0].id);
    h.drain();
    check(s.ownsPlayback() && h.stops == 1 && s.document.activeMix().layers[0].pool->selected == old,
          "Failed activation retains source and playback intent");
    check(absent.moveFileTo(source), "Owned fixture restored");
    s.edit("Structural mapping", [](auto &doc) { doc.activeMix().layers[0].routing.spatial = false; });
    check(!s.ownsPlayback() && h.stops == 2, "Structural mapping change retains deliberate stop policy");
    h.state.getDynamicObject()->setProperty("id", "reference:protected");
    h.state.getDynamicObject()->setProperty("playing", true);
    s.selectPoolEntry(s.document.selectedLayer, s.document.original.layers[0].pool->entries[0].id);
    h.drain();
    check(h.stops == 2 && h.plays == 2 && h.state["id"].toString() == "reference:protected",
          "Pool activation cannot seize parked Library/reference ownership");
    auto guarded = text(sp::designToJson(s.document));
    for (auto phase : {sp::DesignSession::TakeState::arming, sp::DesignSession::TakeState::recording,
                       sp::DesignSession::TakeState::finalizing})
    {
        s.takeState = phase;
        s.selectPoolEntry(s.document.selectedLayer, other.id);
        check(text(sp::designToJson(s.document)) == guarded, "Capture guards unchanged");
    }
    s.takeState = sp::DesignSession::TakeState::idle;
    std::cout << "R18 production panner/canvas/history, generation and PCM checks passed\n";
}
void reviews(const juce::File &root, const sp::Asset &a, const sp::Asset &six, sp::AudioClient &client)
{
    juce::PropertiesFile prefs(root.getChildFile("prefs"), sp::ui::preferenceOptions());
    sp::DesignerView view(root.getChildFile("catalog/library.sqlite"), client, prefs);
    Harness h;
    for (auto input : {std::vector<int>{0, 3}, std::vector<int>{0, 2, 4}, std::vector<int>{2},
                       std::vector<int>{3, 0}, std::vector<int>{2, 3}, std::vector<int>{0, 1, 2, 3, 4, 5}})
    {
        auto d = design(a);
        auto &l = d.original.layers[0];
        auto e = sp::inspectPoolEntry(six, 0, six.samples, true);
        sp::LayerRouting r;
        r.inputs = input;
        r.interpretation = input.size() == 1   ? sp::channelLayout("mono-v1")
                           : input.size() == 2 ? sp::channelLayout("stereo-v1")
                                               : sp::discreteLayout((int)input.size());
        for (int i = 0; i < (int)input.size(); ++i)
            r.routes.push_back({i, i, -1.23456789, i == 1, i == 2});
        r.azimuth = -130;
        r.width = 75;
        r.divergence = .2;
        r.lfeDb = -12;
        r.lfeCutoff = 87;
        sp::reviewPoolEntry(e, r, d.original.layout);
        l.pool->entries.push_back(e);
        sp::activatePoolEntry(l, e.id, d.original.layout);
        sp::syncPoolSelection(d.original);
        sp::PoolCorrectionViewAccess::bind(view, d, h.services());
        auto panel = sp::PoolCorrectionViewAccess::review(view, e.id);
        check(panel != nullptr, "Production review factory returns actual editor");
        check(panel->inputs() == input, "R19 constructor preserves complete ordered custom selection");
        check(text(sp::routingJson(panel->resolved)) == text(sp::routingJson(r)),
              "R19 constructor preserves exact explicit routing");
        panel->refresh(true);
        panel->refresh(false);
        check(panel->use.isEnabled(), "Unchanged review remains valid");
        auto before = text(sp::designToJson(view.session.document));
        panel->use.onClick();
        h.drain();
        check(text(sp::designToJson(view.session.document)) == before,
              "Unchanged active review retains whole document and choice provenance");
        sp::DesignGraph rendered(view.session.document);
        juce::AudioBuffer<float> tags(6, 256);
        rendered.render(tags, 0, 256);
        for (int c = 0; c < 6; ++c)
        {
            double expected = 0;
            if (c < (int)input.size() && c != 2)
            {
                int physical = input[(size_t)c];
                expected = .035 * (physical + 1) * std::sin(100 * (.019 + physical * .003)) *
                           std::pow(10., -1.23456789 / 20) * (c == 1 ? -1 : 1);
            }
            check(std::abs(tags.getSample(c, 100) - expected) < 1e-6,
                  "Independent channel tags agree after unchanged production review");
        }
        auto saved = root.getChildFile(e.id + ".soundplus");
        sp::saveDesignAtomic(saved, view.session.document);
        check(text(sp::routingJson(sp::loadDesign(saved).original.layers[0].routing)) ==
                  text(sp::routingJson(r)),
              "Precise reviewed matrix survives save/reopen");
        panel = sp::PoolCorrectionViewAccess::review(view, e.id);
        panel->physical.setSelectedId(999, juce::dontSendNotification);
        panel->custom.setText("2, 5", false);
        panel->refresh(true);
        check(!panel->use.isEnabled(), "Changed input selection cannot retain stale reviewed routes");
        for (size_t choice = 0; choice < panel->choices.size(); ++choice)
            if (panel->choices[choice].id == "stereo-v1")
                panel->interpretation.setSelectedId((int)choice + 1, juce::dontSendNotification);
        panel->mapping.setSelectedId(2, juce::dontSendNotification);
        panel->refresh(false);
        check(panel->use.isEnabled(), "Deliberate physical-order replacement validates");
        panel->use.onClick();
        h.drain();
        check(view.session.document.original.layers[0].routing.inputs == std::vector<int>({1, 4}),
              "Actual Use applies deliberate custom edit");
        view.session.undo();
        check(text(sp::routingJson(view.session.document.original.layers[0].routing)) ==
                  text(sp::routingJson(r)),
              "Undo restores precise complete review");
        view.session.redo();
        check(view.session.document.original.layers[0].routing.inputs == std::vector<int>({1, 4}),
              "Redo restores edited inputs");
        view.session.undo();
        auto stale = sp::PoolCorrectionViewAccess::review(view, e.id);
        view.session.document.id = juce::Uuid().toString();
        auto retained = text(sp::designToJson(view.session.document));
        stale->use.onClick();
        h.drain();
        check(text(sp::designToJson(view.session.document)) == retained,
              "Stale document Use cannot mutate another context");
        sp::PoolCorrectionViewAccess::bind(view, d, h.services());
        check(text(sp::routingJson(view.session.document.activeMix().layers[0].routing)) ==
                  text(sp::routingJson(r)),
              "Production Use callback preserves exact entry routing");
        check(view.session.document.activeMix().layers[0].routing.inputs == input,
              "Use retains exact physical channel identity");
    }
    auto d = design(a);
    sp::PoolCorrectionViewAccess::bind(view, d, h.services());
    view.session.document.original.layers[0].pool->policy = sp::poolChoicePolicy;
    view.session.document.original.layers[0].pool->seed = 42;
    view.session.document.original.layers[0].pool->ordinal = 2;
    d = view.session.document;
    view.session.play();
    h.drain();
    auto oldStops = h.stops, oldPlays = h.plays;
    auto id = d.original.layers[0].pool->selected;
    auto r = d.original.layers[0].routing;
    auto panel = sp::PoolCorrectionViewAccess::review(view, id);
    check(panel->mapping.getSelectedId() == 3 && panel->resolved.spatial,
          "R19 implicit spatial routing is an existing reviewed state");
    panel->use.onClick();
    h.drain();
    check(text(sp::routingJson(view.session.document.original.layers[0].routing)) == text(sp::routingJson(r)),
          "R19 implicit spatial/LFE state survives actual Use");
    check(view.session.ownsPlayback() && h.stops == oldStops && h.plays == oldPlays,
          "Unchanged spatial review retains active playback and generation");
    auto unchanged = text(sp::designToJson(view.session.document));
    check(unchanged == text(sp::designToJson(d)), "Spatial/LFE unchanged review preserves entire document");
    for (auto source :
         {wave(root.getChildFile("odd7.wav"), 7, 1024), wave(root.getChildFile("wide64.wav"), 64, 1024)})
    {
        auto d2 = design(a);
        auto &layer = d2.original.layers[0];
        auto entry = sp::inspectPoolEntry(source, 0, source.samples, true);
        auto route = r;
        route.inputs.clear();
        route.interpretation = sp::channelLayout("mono-v1");
        route.legacyAnalysisMean = source.channels == 64;
        if (route.legacyAnalysisMean)
            for (int c = 0; c < 64; ++c)
                route.inputs.push_back(c);
        else
            route.inputs = {6};
        sp::reviewPoolEntry(entry, route, d2.original.layout);
        layer.pool->entries.push_back(entry);
        sp::activatePoolEntry(layer, entry.id, d2.original.layout);
        sp::syncPoolSelection(d2.original);
        sp::PoolCorrectionViewAccess::bind(view, d2, h.services());
        auto editor = sp::PoolCorrectionViewAccess::review(view, entry.id);
        check(editor->physical.getSelectedId() == (source.channels == 64 ? 2 : 103),
              "Mean or exact odd final channel preset retained");
        check(editor->inputs() == route.inputs &&
                  text(sp::routingJson(editor->resolved)) == text(sp::routingJson(route)),
              "Odd final/mean complete routing retained");
        editor->use.onClick();
        h.drain();
        check(text(sp::routingJson(view.session.document.original.layers[0].routing)) ==
                  text(sp::routingJson(route)),
              "Odd final/mean Use is faithful");
        auto old = entry;
        old.reviewedBus = sp::channelLayout("stereo-v1");
        sp::PoolMappingReview obsolete(old, d2.original.layout, 2);
        check(obsolete.mapping.getNumItems() == 2, "Obsolete destination does not offer existing routing");
    }
    view.session.stop();
    auto mono = wave(root.getChildFile("mono.wav"), 1, 96000);
    auto monoDoc = design(mono);
    monoDoc.playhead = 0;
    sp::PoolCorrectionViewAccess::bind(view, monoDoc, h.services());
    view.session.play();
    h.drain();
    auto monoBefore = text(sp::designToJson(view.session.document));
    auto monoStops = h.stops, monoPlays = h.plays;
    auto monoReview = sp::PoolCorrectionViewAccess::review(view, monoDoc.original.layers[0].pool->selected);
    monoReview->refresh(true);
    monoReview->refresh(false);
    check(monoReview->resolved.spatial && monoReview->resolved.lfeDb == -12 &&
              monoReview->resolved.lfeCutoff == 87,
          "Mono initial entry retains implicit spatial and nondefault LFE controls");
    monoReview->use.onClick();
    h.drain();
    check(text(sp::designToJson(view.session.document)) == monoBefore && view.session.ownsPlayback() &&
              h.stops == monoStops && h.plays == monoPlays,
          "Mono initial-entry review preserves exact document and playing intent");
    auto monoSave = root.getChildFile("mono-spatial.soundplus");
    sp::saveDesignAtomic(monoSave, view.session.document);
    check(text(sp::routingJson(sp::loadDesign(monoSave).original.layers[0].routing)) ==
              text(sp::routingJson(monoDoc.original.layers[0].routing)),
          "Mono spatial/LFE routing survives save and reopen");
    view.session.stop();
    auto unknownSource = wave(root.getChildFile("untagged-six.wav"), 6, 1024, true);
    auto unknown = sp::inspectPoolEntry(unknownSource, 0, unknownSource.samples, true);
    sp::PoolMappingReview unreviewed(unknown, d.original.layout, 2);
    check(unknown.source.detectedChannels.id.startsWith("unknown-") &&
              unreviewed.interpretation.getSelectedId() == 0 && !unreviewed.use.isEnabled(),
          "Actual untagged six requires explicit ordered interpretation");
    std::cout << "R19 production review constructor/refresh/Use checks passed\n";
}
void writePcm(const juce::File &file, const juce::AudioBuffer<float> &pcm)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    auto out = file.createOutputStream();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(out.release(), 48000,
                            sp::juceChannelSet(pcm.getNumChannels() == 6 ? sp::channelLayout("5.1-side-v1")
                                                                         : sp::channelLayout("stereo-v1")),
                            32, {}, 0));
    check(w && w->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "Portable PCM finalized");
}
juce::AudioBuffer<float> filePcm(const juce::File &file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(file));
    check(r != nullptr, "Portable finalized WAV readable");
    juce::AudioBuffer<float> b((int)r->numChannels, (int)r->lengthInSamples);
    std::vector<float *> p;
    for (int c = 0; c < b.getNumChannels(); ++c)
        p.push_back(b.getWritePointer(c));
    check(r->read(p.data(), b.getNumChannels(), 0, b.getNumSamples()), "Read all portable channels");
    return b;
}
void portable(const juce::File &root, sp::AudioClient &client)
{
    auto a = wave(root.getChildFile("sources/stereo.wav"), 2, 32000),
         six = wave(root.getChildFile("sources/six.wav"), 6, 40960);
    auto d = design(a);
    d.playhead = 0;
    auto &l = d.original.layers[0];
    auto entry = sp::inspectPoolEntry(six, 1200, 22000, true);
    sp::LayerRouting r;
    r.inputs = {0, 3};
    r.interpretation = sp::channelLayout("stereo-v1");
    r.routes = {
        {0, 0, -1.23456789, false, false}, {1, 1, -3.14159265, true, false}, {0, 2, -4.25, false, true}};
    sp::reviewPoolEntry(entry, r, d.original.layout);
    l.pool->entries.push_back(entry);
    sp::activatePoolEntry(l, entry.id, d.original.layout);
    sp::syncPoolSelection(d.original);
    juce::PropertiesFile prefs(root.getChildFile("prefs"), sp::ui::preferenceOptions());
    sp::DesignerView view(root.getChildFile("catalog/library.sqlite"), client, prefs);
    Harness h;
    sp::PoolCorrectionViewAccess::bind(view, d, h.services());
    auto panel = sp::PoolCorrectionViewAccess::review(view, entry.id);
    panel->use.onClick();
    h.drain();
    d = view.session.document;
    auto take = sp::freezeTake(d, "Portable reviewed subset");
    take.generation = 7;
    sp::hashTake(take);
    auto capture = std::make_shared<sp::TakeCapture>(take);
    auto live = d;
    live.takeProcessing = true;
    sp::PcmStream stream(live, 48000, 0);
    stream.startTake(capture);
    std::array<std::atomic<int>, 16> identity;
    for (int c = 0; c < 16; ++c)
        identity[c] = c;
    juce::AudioBuffer<float> consumed(6, 8192), block(6, 512);
    float gain = 1;
    for (int at = 0; at < 8192; at += 512)
    {
        stream.fill(512);
        check(stream.read(block.getArrayOfWritePointers(), 6, 512, gain, 1, &identity) == 512,
              "Consume exact multichannel take prefix");
        for (int c = 0; c < 6; ++c)
            consumed.copyFrom(c, at, block, c, 0, 512);
    }
    capture->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
    take = capture->snapshot();
    check(take.complete, "Portable consumed capture complete");
    d.takes = {take};
    d.activeTake = take.id;
    sp::DesignReader reader(d, 48000, 0, -1, 0);
    juce::AudioBuffer<float> replay(6, (int)reader.length());
    int at = 0;
    while (auto n = reader.read(block, 512))
    {
        for (int c = 0; c < 6; ++c)
            replay.copyFrom(c, at, block, c, 0, n);
        at += n;
    }
    check(compare(consumed, replay) < 1e-6, "Portable capture/replay match");
    writePcm(root.getChildFile("pcm/consumed.wav"), consumed);
    writePcm(root.getChildFile("pcm/replay.wav"), replay);
    auto interleaved = sp::renderDesign(
        d, root.getChildFile("outputs"), d.renderSettings, [] { return false; }, [](double) {});
    auto splitSettings = d.renderSettings;
    splitSettings.splitMono = true;
    auto split =
        sp::renderDesign(d, root.getChildFile("outputs"), splitSettings, [] { return false; }, [](double) {});
    check(compare(consumed, filePcm(juce::File(interleaved.path))) < 1e-6, "Portable capture/offline match");
    d.outputs = {interleaved, split};
    auto oldTake = text(sp::takeToJson(d.takes[0]));
    auto oldOutputs = text(sp::designToJson(d)["outputs"]);
    d.activeTake.clear();
    std::reverse(d.original.layers[0].pool->entries.begin(), d.original.layers[0].pool->entries.end());
    d.original.layers[0].gainDb = -9;
    sp::syncPoolSelection(d.original);
    d.activeTake = take.id;
    check(text(sp::takeToJson(d.takes[0])) == oldTake && text(sp::designToJson(d)["outputs"]) == oldOutputs,
          "Later Live pool edits preserve old take and receipts byte semantics");
    sp::PoolCorrectionViewAccess::bind(view, d, h.services());
    view.session.selectTake(take.id);
    view.session.chooseOutput(split.path);
    check(view.session.readyFiles().size() == 6, "Portable selected split is available");
    sp::PoolCorrectionViewAccess::print(view);
    h.drain();
    check(view.session.document.original.layers.size() == 2 &&
              view.session.document.original.layers.back().source.channels == 6,
          "Actual Print callback keeps multichannel master");
    view.session.selectTake(take.id);
    view.session.chooseOutput(split.path);
    sp::saveDesignAtomic(root.getChildFile("document.soundplus"), view.session.document);
    auto restored = sp::loadDesign(root.getChildFile("document.soundplus"));
    check(text(sp::takeToJson(restored.takes[0])) == oldTake,
          "Portable saved/reopened frozen identity unchanged");
    auto index = juce::var(new juce::DynamicObject());
    auto *o = index.getDynamicObject();
    auto paths = juce::var(new juce::DynamicObject());
    paths.getDynamicObject()->setProperty(a.path, "sources/stereo.wav");
    paths.getDynamicObject()->setProperty(six.path, "sources/six.wav");
    paths.getDynamicObject()->setProperty(interleaved.path,
                                          juce::File(interleaved.path).getRelativePathFrom(root));
    paths.getDynamicObject()->setProperty(split.path, juce::File(split.path).getRelativePathFrom(root));
    for (auto &member : *split.files.getArray())
        paths.getDynamicObject()->setProperty(
            member["path"].toString(), juce::File(member["path"].toString()).getRelativePathFrom(root));
    o->setProperty("pathMap", paths);
    o->setProperty("take", take.id);
    o->setProperty("selectedEntry", entry.id);
    o->setProperty("physicalInputs", juce::Array<juce::var>{0, 3});
    o->setProperty("sourceFirst", 1200);
    o->setProperty("sourceLast", 22000);
    o->setProperty("captureFrames", 8192);
    o->setProperty("pcmTolerance", 1e-6);
    root.getChildFile("index.json").replaceWithText(juce::JSON::toString(index, true));
    std::cout << "Portable evidence " << root.getFullPathName() << "\n";
}
void pump()
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, .003, false);
}
void nativeControls(const juce::File &root, const sp::Asset &a, sp::AudioClient &client)
{
    root.getChildFile("catalog").createDirectory();
    {
        sp::Catalog initialise(root.getChildFile("catalog/library.sqlite"));
    }
    root.getChildFile("index").createDirectory();
    client.gain(.01f);
    client.deviceRate(48000);
    client.bufferSize(512);
    juce::PropertiesFile prefs(root.getChildFile("prefs"), sp::ui::preferenceOptions());
    auto peaks = std::make_shared<sp::WaveformPool>(root.getChildFile("peaks"));
    sp::DesignerView view(root.getChildFile("catalog/library.sqlite"), client, prefs, peaks);
    auto d = design(a);
    sp::PoolCorrectionViewAccess::native(view, d);
    auto &s = view.session;
    s.play();
    double deadline = sp::monotonicMs() + 15000;
    while (sp::monotonicMs() < deadline &&
           (!(bool)client.status()["playing"] || client.status()["id"].toString() != "design:" + d.id))
    {
        pump();
        s.poll();
    }
    auto start = client.status();
    if (!(bool)start["playing"] || !s.ownsPlayback())
        std::cout << "Native start failure " << juce::JSON::toString(start, true) << " session " << s.message
                  << "\n";
    check((bool)start["playing"] && s.ownsPlayback(), "Native pooled Live starts on CoreAudio");
    auto panel = sp::PoolCorrectionViewAccess::routing(view);
    auto &angle = control<juce::Slider>(*panel, "Spatial azimuth (degrees)");
    auto &width = control<juce::Slider>(*panel, "Stereo image width (degrees)");
    auto tiny = wave(root.getChildFile("tiny.wav"), 2, 512);
    std::vector<juce::File> batches;
    for (int b = 0; b < 8; ++b)
    {
        auto folder = root.getChildFile("incoming/batch-" + juce::String(b));
        folder.createDirectory();
        for (int i = 0; i < 32; ++i)
            check(juce::File(tiny.path).copyFileTo(folder.getChildFile("tag-" + juce::String(i) + ".wav")),
                  "Owned bounded indexing fixture copied");
        batches.push_back(folder);
    }
    sp::LibraryService library(root.getChildFile("index/library.sqlite"));
    int overlap = 0;
    juce::Array<juce::var> observations;
    auto active = sp::monotonicMs();
    angle.onDragStart();
    width.onDragStart();
    sp::DesignerCanvas canvas(
        s, [](const auto &) -> std::shared_ptr<const sp::WaveformData> { return {}; }, prefs);
    canvas.setSize(1000, 180);
    canvas.viewport.pixelsPerSecond = 35;
    canvas.snapping.enabled = false;
    for (int i = 0; i < 100; ++i)
    {
        if (i % 12 == 0 && i / 12 < 8)
            library.addFolder(batches[(size_t)i / 12]);
        angle.setValue(-140 + i * 2.5, juce::sendNotificationSync);
        width.setValue(40 + i, juce::sendNotificationSync);
        if (i % 20 == 0)
        {
            angle.onDragEnd();
            width.onDragEnd();
            auto shape = sp::clipGeometry(s.document.original.layers[0], 0, canvas.viewport, 48000);
            auto from = juce::Point<float>(shape.left + 1, shape.clipTop + shape.height * .6f);
            auto to = from + juce::Point<float>(5, 0);
            canvas.mouseDown(pointer(canvas, from, from));
            canvas.mouseDrag(pointer(canvas, to, from));
            canvas.mouseUp(pointer(canvas, to, from));
            s.undo();
            s.redo();
            s.undo();
            angle.onDragStart();
            width.onDragStart();
        }
        peaks->demand(6, {a, tiny});
        peaks->tick();
        s.poll();
        pump();
        juce::Thread::sleep(25);
        auto status = client.status();
        check((bool)status["playing"] && status["playGeneration"] == start["playGeneration"] &&
                  status["preparationsRequested"] == start["preparationsRequested"],
              "Native pooled Live controls retain playing generation and graph policy");
        auto progress = library.snapshot().progress;
        if (progress.activeWorkers > 0 || progress.metadataBacklog > 0)
            ++overlap;
        auto item = juce::var(new juce::DynamicObject());
        auto *o = item.getDynamicObject();
        o->setProperty("elapsedMs", sp::monotonicMs() - active);
        o->setProperty("status", status);
        o->setProperty("indexWorkers", progress.activeWorkers);
        o->setProperty("backlog", progress.metadataBacklog);
        o->setProperty("azimuth", s.document.original.layers[0].routing.azimuth);
        o->setProperty("width", s.document.original.layers[0].routing.width);
        observations.add(item);
    }
    angle.onDragEnd();
    width.onDragEnd();
    s.poll();
    pump();
    auto end = client.status();
    check(overlap > 0, "Native controls overlap actual background indexing");
    check((int64_t)end["heardEdit"] > 0 && (int64_t)end["underruns"] == 0,
          "Native parameters heard with zero underruns");
    auto report = juce::var(new juce::DynamicObject());
    auto *o = report.getDynamicObject();
    o->setProperty("start", start);
    o->setProperty("activeEnd", end);
    o->setProperty("samples", observations);
    o->setProperty("indexOverlapSamples", overlap);
    o->setProperty("waveforms", peaks->metrics());
    o->setProperty("condition",
                   "256 generated real WAV files in eight batches; 100 real panner callbacks, canvas "
                   "trim/history callbacks, shared previews; no semantic/maximum-capacity claim");
    root.getChildFile("native-controls.json").replaceWithText(juce::JSON::toString(report, true));
    s.stop();
    std::cout << "Native corrected control samples 100, indexing overlap " << overlap << "\n";
}

} // namespace
void runPoolCorrectionTests(const juce::File &parent, const juce::File &application, const juce::String &mode)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto stereo = wave(root.getChildFile("stereo.wav"), 2),
         six = wave(root.getChildFile("six.wav"), 6, 96000);
    sp::AudioClient client(application);
    if (mode != "r19")
        live(root.getChildFile("live"), stereo, client);
    if (mode != "r18")
        reviews(root.getChildFile("reviews"), stereo, six, client);
    if (mode.isEmpty())
    {
        portable(root.getChildFile("portable"), client);
        nativeControls(root.getChildFile("native"), stereo, client);
        parent.getChildFile("latest-evidence.txt").replaceWithText(root.getFullPathName());
    }
    std::cout << "Pool correction checks " << checks << "\n";
}

void preparePoolCorrectionUI(const juce::File &root)
{
    if (root.exists())
        throw std::runtime_error("Use a fresh owned QA directory");
    root.createDirectory();
    auto a = wave(root.getChildFile("media/Stereo.wav"), 2),
         six = wave(root.getChildFile("media/Six.wav"), 6, 96000);
    auto d = design(a);
    d.playhead = 0;
    d.outputFolder = root.getChildFile("renders").getFullPathName();
    auto &layer = d.original.layers[0];
    layer.rate = .25;
    layer.pool->entries.push_back(sp::inspectPoolEntry(six, 0, six.samples, true));
    auto region = layer.pool->entries[0];
    region.id = juce::Uuid().toString();
    region.region.startSample = 24000;
    region.region.endSample = 720000;
    layer.pool->entries.push_back(region);
    sp::syncPoolSelection(d.original);
    d.original.performance.assignments[0].layer = layer.id;
    sp::saveDesignAtomic(root.getChildFile("Correction QA.soundplus"), d);
    auto catalog = root.getChildFile("catalog");
    catalog.createDirectory();
    {
        sp::Catalog create(catalog.getChildFile("library.sqlite"));
    }
    sp::DesignRecoveryStore recovery(catalog.getChildFile("designer-autosave"));
    recovery.retain(d, root.getChildFile("Correction QA.soundplus"), false);
    recovery.activate(d.id);
    juce::PropertiesFile prefs(catalog.getChildFile("interface.settings"), sp::ui::preferenceOptions());
    prefs.setValue("designerActive", true);
    prefs.setValue("browserOpen", true);
    prefs.setValue("browserWidth", 300);
    prefs.setValue("inspectorOpen", true);
    prefs.setValue("alternativesOpen", true);
    prefs.setValue("monitorVolume", .04);
    prefs.saveIfNeeded();
    std::cout << root.getFullPathName() << "\n";
}
