#include "MultichannelTests.h"
#include "TakeTests.h"
#include "DesignSession.h"
#include "DesignerView.h"
#include "LibraryView.h"
#include "AudioStream.h"
#include "Performance.h"
#include "PerformanceState.h"
#include "DesignLive.h"
#include "WaveformPreview.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <cstring>
#include <deque>
namespace
{
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
template <class F> void until(F f, const char *why, int ms = 15000)
{
    auto end = sp::monotonicMs() + ms;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    check(f(), why);
}
juce::String hash(const juce::File &f)
{
    auto in = f.createInputStream();
    check(in != nullptr, "Hash finalized file");
    return juce::SHA256(*in).toHexString();
}
juce::AudioBuffer<float> signal(int channels, int samples, int isolated = -1, bool antiphase = false)
{
    juce::AudioBuffer<float> b(channels, samples);
    b.clear();
    for (int c = 0; c < channels; ++c)
        if (isolated < 0 || isolated == c)
            for (int n = 0; n < samples; ++n)
            {
                float v = .03f * std::sin((float)n * (c + 1) * .013f);
                if (n == 256 + 17 * c)
                    v += .3f + c * .005f;
                if (antiphase)
                    v = (c % 2 ? -1 : 1) * .2f * std::sin(n * .03f);
                b.setSample(c, n, v);
            }
    return b;
}
void fixture(const juce::File &f, const sp::ChannelLayout &layout, const juce::AudioBuffer<float> &pcm,
             double rate = 48000, bool unknown = false)
{
    if (unknown)
    {
        auto o = f.createOutputStream();
        int bytes = pcm.getNumChannels() * pcm.getNumSamples() * 4;
        o->write("RIFF", 4);
        o->writeInt(36 + bytes);
        o->write("WAVEfmt ", 8);
        o->writeInt(16);
        o->writeShort(3);
        o->writeShort((short)pcm.getNumChannels());
        o->writeInt((int)rate);
        o->writeInt((int)rate * pcm.getNumChannels() * 4);
        o->writeShort((short)(pcm.getNumChannels() * 4));
        o->writeShort(32);
        o->write("data", 4);
        o->writeInt(bytes);
        for (int n = 0; n < pcm.getNumSamples(); ++n)
            for (int c = 0; c < pcm.getNumChannels(); ++c)
                o->writeFloat(pcm.getSample(c, n));
        return;
    }
    juce::WavAudioFormat wav;
    auto o = f.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(o.release(), rate, sp::juceChannelSet(layout), 32, {}, 0));
    check(w && w->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "Finalize role-labelled fixture");
}
struct RawWave
{
    int channels = 0, bits = 0, rate = 0;
    uint32_t mask = 0;
    juce::AudioBuffer<float> pcm;
};
RawWave independentRead(const juce::File &f)
{
    juce::MemoryBlock bytes;
    check(f.loadFileAsData(bytes), "Read actual finalized WAV bytes");
    auto *p = (const char *)bytes.getData();
    size_t size = bytes.getSize();
    check(size >= 44 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WAVE", 4) == 0,
          "Independent RIFF/container check");
    auto u16 = [&](size_t i) { return (uint16_t)((uint8_t)p[i] | ((uint16_t)(uint8_t)p[i + 1] << 8)); };
    auto u32 = [&](size_t i) { return (uint32_t)u16(i) | ((uint32_t)u16(i + 2) << 16); };
    RawWave r;
    size_t data = 0, count = 0;
    for (size_t at = 12; at + 8 <= size;)
    {
        uint32_t n = u32(at + 4);
        check((uint64_t)at + 8 + n <= size, "RIFF chunk is within finalized bytes");
        if (std::memcmp(p + at, "fmt ", 4) == 0)
        {
            r.channels = u16(at + 10);
            r.rate = (int)u32(at + 12);
            r.bits = u16(at + 22);
            if (u16(at + 8) == 0xfffe && n >= 40)
                r.mask = u32(at + 28);
        }
        if (std::memcmp(p + at, "data", 4) == 0)
        {
            data = at + 8;
            count = n;
        }
        at += 8 + n + (n & 1);
    }
    check(r.bits == 32 && r.channels > 0 && count % (r.channels * 4) == 0,
          "Independent float32 sample/frame stride");
    int frames = (int)(count / (r.channels * 4));
    r.pcm.setSize(r.channels, frames);
    for (int n = 0; n < frames; ++n)
        for (int c = 0; c < r.channels; ++c)
        {
            float v;
            std::memcpy(&v, p + data + (n * r.channels + c) * 4, 4);
            check(std::isfinite(v), "Finite actual WAV channel");
            r.pcm.setSample(c, n, v);
        }
    return r;
}
juce::AudioBuffer<float> read(const sp::DesignDocument &d, double rate = 48000, double tail = 0)
{
    sp::DesignReader r(d, rate, 0, -1, tail);
    juce::AudioBuffer<float> b(sp::designChannels(d), (int)r.length()), block(sp::designChannels(d), 4096);
    int at = 0;
    while (auto n = r.read(block, 4096))
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            b.copyFrom(c, at, block, c, 0, n);
        at += n;
    }
    return b;
}
double difference(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b, int frames = -1)
{
    check(a.getNumChannels() == b.getNumChannels(), "Compared PCM channel count");
    if (frames < 0)
    {
        check(a.getNumSamples() == b.getNumSamples(), "Compared PCM frame count");
        frames = a.getNumSamples();
    }
    check(a.getNumSamples() >= frames && b.getNumSamples() >= frames, "Compared consumed PCM exists");
    double max = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < frames; ++n)
            max = std::max(max, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
    return max;
}
sp::DesignDocument design(const juce::File &f, const sp::ChannelLayout &layout)
{
    sp::DesignDocument d;
    d.name = "Multichannel QA";
    d.original.masterDb = 0;
    auto l = sp::makeDesignLayer(sp::inspectDesignSource(f), 0, -1, true);
    d.original.layers = {l};
    sp::setMixLayout(d.original, layout);
    d.selectedLayer = l.id;
    d.renderSettings.layout = "same";
    d.renderSettings.tailSeconds = 0;
    d.renderSettings.bits = 32;
    return d;
}
juce::AudioBuffer<float> rawCapture(const juce::File &f, int channels, int frames)
{
    juce::MemoryBlock bytes;
    until(
        [&]
        {
            f.loadFileAsData(bytes);
            return bytes.getSize() >= (size_t)frames * channels * 4;
        },
        "Native pre-monitor multichannel PCM flushed");
    juce::AudioBuffer<float> b(channels, frames);
    auto *raw = (const float *)bytes.getData();
    for (int n = 0; n < frames; ++n)
        for (int c = 0; c < channels; ++c)
            b.setSample(c, n, raw[n * channels + c]);
    return b;
}
} // namespace
namespace sp
{
struct LayoutViewTestAccess
{
    template <class T> static T &control(juce::Component &parent, const juce::String &name)
    {
        for (auto *child : parent.getChildren())
            if (auto *typed = dynamic_cast<T *>(child);
                typed && (child->getTitle() == name || child->getName() == name ||
                          (dynamic_cast<juce::Button *>(child) &&
                           dynamic_cast<juce::Button *>(child)->getButtonText() == name)))
                return *typed;
        throw std::runtime_error(("Missing real routing control: " + name).toStdString());
    }
    static void library(LibraryView &v, const Asset &a)
    {
        v.selectedAsset = a;
        v.selectedId = a.id;
        v.rangeAssetId = a.id;
        v.regionStart = 0;
        v.regionEnd = a.samples;
        v.snapshot.results = {a};
        v.snapshot.total = 1;
        v.snapshot.offset = 0;
        int chosen = -99;
        v.onAddToDesigner = [&](auto &, int64_t, int64_t, bool, int pair) { chosen = pair; };
        v.auditionChannels.setSelectedId(1, juce::dontSendNotification);
        v.addDesigner.onClick();
        check(chosen == -3, "Actual Library Add leaves an unselected wide source unresolved");
        juce::SparseSet<int> rows;
        rows.addRange({0, 1});
        auto description = v.getDragSourceDescription(rows);
        check(designFromJson(juce::JSON::parse(description.toString()))
                  .original.layers[0]
                  .routing.needsSelection,
              "Actual Library canvas drag carries unresolved wide-source identity");
        v.auditionChannels.setSelectedId(33, juce::dontSendNotification);
        v.addDesigner.onClick();
        auto captured = designFromJson(juce::JSON::parse(v.getDragSourceDescription(rows).toString()));
        check(chosen == 31 && captured.original.layers[0].routing.inputs == std::vector<int>({62, 63}),
              "Actual Add and drag preserve explicitly selected last source pair");
        v.onAddToDesigner = {};
    }
    static DesignOutput selection(DesignerView &v, DesignDocument d, const juce::File &folder)
    {
        v.session.document = d;
        v.session.selectedIds = {d.selectedLayer};
        v.session.document.outputFolder = folder.getFullPathName();
        std::vector<std::function<void()>> completions;
        auto originalPost = v.session.services.post;
        v.session.services.post = [&](auto f) { completions.push_back(std::move(f)); };
        v.session.services.resolve = [](auto &) {};
        v.session.render(true);
        v.session.renderWorker.join();
        for (auto &f : completions)
            f();
        v.session.services.post = originalPost;
        check(!v.session.document.outputs.empty(), "Actual Render Selection command finalizes an output");
        return v.session.document.outputs.back();
    }
    static void run(DesignerView &v, DesignDocument d, DesignDocument wide)
    {
        v.session.protectStartupEdits();
        v.session.document = d;
        v.session.selectedIds = {d.selectedLayer};
        v.refresh();
        auto p = v.makeRoutingPanel();
        check(p != nullptr, "Actual production routing panel is available");
        auto &angle = control<juce::Slider>(*p, "Spatial azimuth (degrees)");
        auto before = v.session.selectedLayer()->routing.azimuth;
        angle.onDragStart();
        angle.setValue(120, juce::sendNotificationSync);
        angle.onDragEnd();
        check(std::abs(v.session.selectedLayer()->routing.azimuth - 120) < .001,
              "Production spatial slider changes real model");
        v.session.undo();
        check(v.session.selectedLayer()->routing.azimuth == before,
              "Spatial gesture is one Undo transaction");
        v.session.redo();
        angle.onDragStart();
        angle.setValue(-60, juce::sendNotificationSync);
        check(p->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)),
              "Routing panel accepts gesture Escape");
        check(std::abs(v.session.selectedLayer()->routing.azimuth - 120) < .001,
              "Escape restores spatial controls");
        // Construct the production panel under each capture phase (no synthetic timer policy).
        v.session.takeDisplay = v.session.document.activeMix();
        v.session.takeState = DesignSession::TakeState::recording;
        v.refresh();
        check(v.layoutControl.isEnabled(), "Routing remains reachable from Design menu while recording");
        p.reset();
        p = v.makeRoutingPanel();
        check(control<juce::Slider>(*p, "Spatial azimuth (degrees)").isEnabled() &&
                  !control<juce::ComboBox>(*p, "Explicit source channel selection").isEnabled() &&
                  !control<juce::ToggleButton>(*p, "Spatial Panner").isEnabled(),
              "Only captured motion remains enabled during Recording");
        v.session.takeState = DesignSession::TakeState::finalizing;
        p.reset();
        p = v.makeRoutingPanel();
        check(!control<juce::Slider>(*p, "Spatial azimuth (degrees)").isEnabled(),
              "Finalizing freezes spatial motion too");
        v.session.takeState = DesignSession::TakeState::idle;
        p.reset();
        v.session.document = wide;
        v.session.selectedIds = {wide.selectedLayer};
        v.refresh();
        p = v.makeRoutingPanel();
        check(v.session.selectedLayer()->routing.needsSelection &&
                  v.session.selectedLayer()->routing.inputs.empty(),
              "Wide source starts explicitly unresolved");
        control<juce::ComboBox>(*p, "Explicit source channel selection")
            .setSelectedId(33, juce::sendNotificationSync);
        check(v.session.selectedLayer()->routing.inputs == std::vector<int>({62, 63}) &&
                  !v.session.selectedLayer()->routing.needsSelection,
              "Actual subset selector accepts final pair of64");
        auto &input = control<juce::ComboBox>(*p, "Route input");
        auto &output = control<juce::ComboBox>(*p, "Route output");
        input.setSelectedId(2, juce::sendNotificationSync);
        output.setSelectedId(1, juce::sendNotificationSync);
        control<juce::Slider>(*p, "Route trim (dB)").setValue(-6, juce::dontSendNotification);
        control<juce::ToggleButton>(*p, "Invert polarity").setToggleState(true, juce::dontSendNotification);
        control<juce::TextButton>(*p, "Set route").onClick();
        auto &routes = v.session.selectedLayer()->routing.routes;
        auto it =
            std::find_if(routes.begin(), routes.end(), [](auto &r) { return r.input == 1 && r.output == 0; });
        check(it != routes.end() && it->invert && std::abs(it->gainDb + 6) < .001,
              "Actual matrix command commits explicit trim/polarity");
        v.session.undo();
        check(v.session.selectedLayer()->routing.routes.empty(),
              "Matrix command Undo restores role-routing policy");
        control<juce::ComboBox>(*p, "Explicit source channel selection")
            .setSelectedId(103, juce::sendNotificationSync);
        check(v.session.selectedLayer()->routing.inputs.size() == 16 &&
                  v.session.selectedLayer()->routing.inputs.front() == 48 &&
                  v.session.selectedLayer()->routing.inputs.back() == 63,
              "Actual wide source selector supports an explicit16-channel window");
        v.session.undo();
        check(v.session.selectedLayer()->routing.inputs == std::vector<int>({62, 63}),
              "Wide window Undo restores original pair");
    }
};
} // namespace sp
void prepareMultichannelUI(const juce::File &folder)
{
    folder.createDirectory();
    auto bus = sp::channelLayout("5.1-side-v1");
    auto visible = [](int channels)
    {
        auto b = signal(channels, 48000 * 30);
        for (int c = 0; c < channels; ++c)
            for (int n = 0; n < b.getNumSamples(); ++n)
                b.setSample(c, n, std::clamp(b.getSample(c, n) * 8.f, -.8f, .8f));
        return b;
    };
    fixture(folder.getChildFile("Surround bed.wav"), bus, visible(6));
    fixture(folder.getChildFile("Mono point.wav"), sp::channelLayout("mono-v1"), visible(1));
    fixture(folder.getChildFile("Unknown six.wav"), sp::discreteLayout(6, true), signal(6, 48000 * 2), 48000,
            true);
    auto d = design(folder.getChildFile("Surround bed.wav"), bus);
    auto l = sp::makeDesignLayer(sp::inspectDesignSource(folder.getChildFile("Mono point.wav")), 0, -1, true);
    sp::initialiseRouting(l, bus);
    l.routing.spatial = true;
    l.effects = {sp::makeStockEffect("delay")};
    d.original.layers.push_back(l);
    d.original.masterDb = -12;
    d.name = "Surround motion QA";
    d.selectedLayer = l.id;
    d.outputFolder = folder.getChildFile("Renders").getFullPathName();
    sp::saveDesignAtomic(folder.getChildFile("Surround QA.soundplus"), d);
}
void runMultichannelTests(const juce::File &parent, const juce::File &application)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    juce::Array<juce::var> records;
    auto layouts = sp::namedLayouts();
    layouts.push_back(sp::discreteLayout(6));
    layouts.push_back(sp::discreteLayout(16));
    for (auto layout : layouts)
    {
        auto f = root.getChildFile(layout.id + ".wav");
        auto expected = signal(layout.size(), 4096);
        fixture(f, layout, expected);
        auto d = design(f, layout);
        check(d.original.layers[0].source.detectedChannels == layout || layout.id.startsWith("discrete-"),
              "Known layout detection uses metadata rather than count");
        auto heard = read(d);
        check(difference(expected, heard) == 0,
              "Role-preserving bed PCM identity on every named/discrete layout");
        auto saved = root.getChildFile(layout.id + ".soundplus");
        sp::saveDesignAtomic(saved, d);
        auto reopened = sp::loadDesign(saved);
        check(reopened.activeMix().layout == layout && difference(read(reopened), heard) == 0,
              "Layout save/reopen preserves all channels");
        auto o = sp::renderDesign(
            d, root.getChildFile("renders"), d.renderSettings, [] { return false; }, [](double) {});
        auto raw = independentRead(juce::File(o.path));
        check(sp::inspectDesignSource(juce::File(o.path)).detectedChannels == layout,
              "Actual rendered WAV reimport preserves explicit ordered layout");
        check(raw.channels == layout.size() && raw.rate == 48000 && difference(raw.pcm, expected) == 0,
              "Independent actual interleaved WAV PCM retains exact channel identity");
        uint32_t mask = 0;
        for (auto r : layout.roles)
        {
            int bit = r == "L"     ? 0
                      : r == "R"   ? 1
                      : r == "C"   ? 2
                      : r == "LFE" ? 3
                      : r == "Lb"  ? 4
                      : r == "Rb"  ? 5
                      : r == "Ls"  ? 9
                      : r == "Rs"  ? 10
                                   : -1;
            if (bit >= 0)
                mask |= 1u << bit;
        }
        if (layout.size() > 2 && !layout.id.startsWith("discrete-"))
            check(raw.mask == mask, "Actual WAVEFORMATEXTENSIBLE role mask and ordering");
        for (auto type : {"gain", "filter", "delay", "reverb"})
        {
            auto isolated = root.getChildFile(layout.id + "-" + type + "-isolated.wav");
            fixture(isolated, layout, signal(layout.size(), 2048, layout.size() - 1));
            auto e = design(isolated, layout);
            e.original.layers[0].effects = {sp::makeStockEffect(type)};
            e.original.masterEffects = {sp::makeStockEffect(type)};
            auto pcm = read(e, 44100, .1);
            double leak = 0, power = 0;
            for (int c = 0; c < layout.size(); ++c)
                for (int n = 0; n < pcm.getNumSamples(); ++n)
                    if (c == layout.size() - 1)
                        power += std::abs(pcm.getSample(c, n));
                    else
                        leak = std::max(leak, (double)std::abs(pcm.getSample(c, n)));
            check(leak == 0 && power > .001, "Every stock layer/master insert and resampling preserves "
                                             "isolated-channel histories without crosstalk");
        }
        auto manipulated = d;
        auto &l = manipulated.original.layers[0];
        l.region.startSample = 32;
        l.region.endSample = 4000;
        l.sync = 50;
        l.reverse = true;
        l.placement = 17;
        l.fadeIn = 90;
        l.fadeOut = 120;
        l.fadeInShape = sp::FadeShape::custom;
        l.fadeInPoints = {{.4, .1}};
        l.rate = 1.2;
        l.gainDb = -9;
        auto edited = read(manipulated);
        check(edited.getNumChannels() == layout.size() && edited.getMagnitude(0, 17) == 0,
              "Reverse/trim/slip/placement/fades/linked rate cover all linked channels");
        auto settings = d.renderSettings;
        settings.splitMono = true;
        auto split =
            sp::renderDesign(d, root.getChildFile("split"), settings, [] { return false; }, [](double) {});
        check(sp::outputFiles(split).size() == layout.size(),
              "Split set contains each actual finalized channel");
        for (int c = 0; c < layout.size(); ++c)
        {
            auto v = split.files[c];
            auto mono = independentRead(juce::File(v["path"].toString()));
            double error = 0;
            for (int n = 0; n < expected.getNumSamples(); ++n)
                error =
                    std::max(error, (double)std::abs(mono.pcm.getSample(0, n) - expected.getSample(c, n)));
            check(mono.channels == 1 && v["role"].toString() == layout.roles[c] && error == 0 &&
                      hash(juce::File(v["path"].toString())) == v["sha256"].toString(),
                  "Split roles/filenames/hashes match independent per-channel PCM");
        }
        bool rejected = false;
        auto fail = root.getChildFile("split-fail");
        try
        {
            sp::renderDesign(
                d, fail, settings, [&]
                { return !fail.findChildFiles(juce::File::findDirectories, false, ".split-*").isEmpty(); },
                [](double) {});
        }
        catch (...)
        {
            rejected = true;
        }
        check(rejected && fail.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty() &&
                  hash(juce::File(split.path)) == o.sha256,
              "Split staging failure leaves no partial published set and preserves prior durable output");
        auto controlled = d;
        controlled.original.layers[0].effects = {sp::makeStockEffect("gain"), sp::makeStockEffect("filter"),
                                                 sp::makeStockEffect("delay"), sp::makeStockEffect("reverb")};
        controlled.original.masterEffects = controlled.original.layers[0].effects;
        for (auto &e : controlled.original.masterEffects)
            e.id = juce::Uuid().toString();
        controlled.takeProcessing = true;
        auto &perf = controlled.original.performance;
        perf.macros[0].enabled = true;
        sp::addMacroMapping(controlled.original, 0, {{controlled.original.layers[0].id, {}, "gain"}, -12, 0});
        auto frozenBed = sp::freezeTake(controlled, "Rate-converted bed");
        frozenBed.generation = 11;
        frozenBed.captureRate = 44100;
        sp::hashTake(frozenBed);
        auto cap = std::make_shared<sp::TakeCapture>(frozenBed);
        sp::PcmStream stream(controlled, 44100, 0);
        stream.startTake(cap);
        std::array<std::atomic<int>, 16> mapping;
        for (int c = 0; c < 16; ++c)
            mapping[c] = c;
        juce::AudioBuffer<float> captured(layout.size(), 3072), part(layout.size(), 512);
        float unity = 1;
        for (int k = 0; k < 6; ++k)
        {
            if (k == 1)
            {
                auto values = sp::mixControls(controlled.original);
                values.macros[0] = .8;
                stream.updatePerformance(values);
            }
            stream.fill(512);
            check(stream.read(part.getArrayOfWritePointers(), layout.size(), 512, unity, 1, &mapping) == 512,
                  "Rate-converted bus consumption");
            for (int c = 0; c < layout.size(); ++c)
                captured.copyFrom(c, k * 512, part, c, 0, 512);
        }
        cap->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
        auto frozenResult = cap->snapshot();
        auto played = controlled;
        played.takes = {frozenResult};
        played.activeTake = frozenResult.id;
        check(frozenResult.complete && frozenResult.events.size() == 1 &&
                  difference(captured, read(played, 44100)) < 1e-6,
              "Every declared layout captures/replays exact per-channel rate-converted applied controls");
        auto bounded = std::make_shared<sp::TakeCapture>(frozenBed, 2);
        auto values = sp::mixControls(controlled.original);
        values.macros[0] = .2;
        check(bounded->append(0, values), "Layout capture accepts first event");
        values.macros[0] = .3;
        check(bounded->append(1, values), "Layout capture accepts second event");
        values.macros[0] = .4;
        check(!bounded->append(2, values), "Layout capture shares bounded overflow policy");
        bounded->finish(512, true, 470);
        check(!bounded->snapshot().complete, "Overflow cannot become complete in new channel contract");
        auto r = juce::var(new juce::DynamicObject());
        r.getDynamicObject()->setProperty("layout", sp::layoutJson(layout));
        r.getDynamicObject()->setProperty("output", o.path);
        r.getDynamicObject()->setProperty("split", split.files);
        records.add(r);
    }
    // Unknown six must not acquire surround roles from channel count or the reader's canonical fallback.
    auto unknown = root.getChildFile("unknown-six.wav");
    fixture(unknown, sp::discreteLayout(6, true), signal(6, 2048), 48000, true);
    auto u = sp::inspectDesignSource(unknown);
    check(u.detectedChannels.id == "unknown-6-v1", "Unknown six-channel admission is honestly unknown");
    auto declared = root.getChildFile("invalid-description.wav");
    juce::StringPairArray misleading;
    misleading.set(juce::WavAudioFormat::bwavDescription, "SoundPlus layout:discrete-16-v1; order:D1,D2");
    {
        juce::WavAudioFormat f;
        std::unique_ptr<juce::AudioFormatWriter> w(
            f.createWriterFor(declared.createOutputStream().release(), 48000,
                              sp::juceChannelSet(sp::channelLayout("5.1-side-v1")), 32, misleading, 0));
        check(w && w->writeFromAudioSampleBuffer(signal(6, 1024), 0, 1024),
              "Misleading producer description fixture finalized");
    }
    auto declaredSource = sp::inspectDesignSource(declared);
    check(declaredSource.detectedChannels.id == "5.1-side-v1" &&
              declaredSource.layoutConfidence == "container-mask",
          "Recognized normative mask wins over invalid producer description; confidence is honest");

    auto ambiguous = design(unknown, sp::channelLayout("5.1-side-v1"));
    bool rejected = false;
    try
    {
        read(ambiguous);
    }
    catch (...)
    {
        rejected = true;
    }
    check(rejected, "Unknown bed cannot silently infer 5.1 or truncate");
    ambiguous.original.layers[0].routing.interpretation = sp::channelLayout("5.1-side-v1");
    check(difference(read(ambiguous), signal(6, 2048)) == 0,
          "Explicit user interpretation enables reviewed six-channel routing");
    // Explicit per-channel trim, polarity, mute; no implicit edits to other channels.
    auto routed = ambiguous;
    routed.original.layers[0].routing.routes.clear();
    for (int c = 0; c < 6; ++c)
        routed.original.layers[0].routing.routes.push_back({c, c, c == 2 ? -6. : 0., c == 1, c == 3});
    auto mapped = read(routed), source = signal(6, 2048);
    double routeError = 0;
    for (int c = 0; c < 6; ++c)
        for (int n = 0; n < 2048; ++n)
        {
            float scale = c == 3   ? 0.f
                          : c == 1 ? -1.f
                          : c == 2 ? (float)juce::Decibels::decibelsToGain(-6.)
                                   : 1.f;
            routeError = std::max(routeError,
                                  (double)std::abs(mapped.getSample(c, n) - source.getSample(c, n) * scale));
        }
    check(routeError < 1e-7, "Explicit matrix gain/polarity/mute preserves independent channel identity");
    auto wide = root.getChildFile("wide64.wav");
    auto widePcm = signal(64, 1024);
    fixture(wide, sp::discreteLayout(64, true), widePcm, 48000, true);
    sp::DesignDocument wideDoc;
    wideDoc.original.masterDb = 0;
    wideDoc.original.layers = {sp::makeDesignLayer(sp::inspectDesignSource(wide), 0, -1, true)};
    wideDoc.original.layers[0].region.auditionPair = 31;
    sp::setMixLayout(wideDoc.original, sp::channelLayout("stereo-v1"));
    auto lastPair = read(wideDoc);
    check(lastPair.getNumChannels() == 2 && lastPair.getSample(0, 500) == widePcm.getSample(62, 500) &&
              lastPair.getSample(1, 500) == widePcm.getSample(63, 500),
          "64-channel admission retains explicit last-pair mapping without truncation");
    auto wideWindow = wideDoc;
    wideWindow.original.layout = sp::discreteLayout(16);
    wideWindow.renderSettings.layout = "same";
    wideWindow.renderSettings.bits = 32;
    wideWindow.renderSettings.tailSeconds = 0;
    auto &windowRouting = wideWindow.original.layers[0].routing;
    windowRouting = {};
    windowRouting.interpretation = sp::discreteLayout(16);
    for (int c = 48; c < 64; ++c)
        windowRouting.inputs.push_back(c);
    auto windowPcm = read(wideWindow);
    juce::AudioBuffer<float> expectedWindow(16, 1024);
    for (int c = 0; c < 16; ++c)
        expectedWindow.copyFrom(c, 0, widePcm, c + 48, 0, 1024);
    check(difference(windowPcm, expectedWindow) == 0,
          "Explicit64-source to16-channel window preserves every selected physical channel");
    auto windowOutput = sp::renderDesign(
        wideWindow, root.getChildFile("wide-window"), wideWindow.renderSettings, [] { return false; },
        [](double) {});
    check(difference(independentRead(juce::File(windowOutput.path)).pcm, expectedWindow) == 0,
          "64-source selected window survives actual16-channel WAV export");
    auto unresolved = wideDoc;
    unresolved.original.layout = {};
    unresolved.original.layers[0].routing = {};
    sp::setMixLayout(unresolved.original, sp::channelLayout("stereo-v1"));
    sp::initialiseRouting(unresolved.original.layers[0], unresolved.original.layout);
    unresolved.selectedLayer = unresolved.original.layers[0].id;
    {
        sp::AudioClient viewAudio(application);
        juce::PropertiesFile prefs(root.getChildFile("view/interface.settings"), sp::ui::preferenceOptions());
        sp::DesignerView view(root.getChildFile("view/library.sqlite"), viewAudio, prefs);
        auto pointDoc = design(root.getChildFile("mono-v1.wav"), sp::channelLayout("5.1-side-v1"));
        pointDoc.original.layers[0].routing.spatial = true;
        sp::LayoutViewTestAccess::run(view, pointDoc, unresolved);
    }
    {
        sp::LibraryView browser(root.getChildFile("browser/library.sqlite"), application, false);
        sp::LayoutViewTestAccess::library(browser, sp::inspectDesignSource(wide));
    }
    auto down = sp::conversionMatrix(sp::channelLayout("5.1-side-v1"), sp::channelLayout("stereo-v1"));
    check(down[0][3] == 0 && down[1][3] == 0 && down[0][2] == down[1][2],
          "Monitor/export conversion excludes LFE and treats center symmetrically");
    // Source metadata and explicit interpretation are independent durable fields.
    auto catalogFile = root.getChildFile("catalog/library.sqlite");
    catalogFile.getParentDirectory().createDirectory();
    juce::String catalogId;
    int64_t catalogRevision = 0;
    {
        sp::Catalog db(catalogFile);
        auto folder = db.addRoot(root);
        sp::FileStamp stamp;
        sp::fileStamp(unknown, stamp);
        db.discover(folder, unknown.getFileName(), stamp, 1);
        auto pending = db.pending();
        check(!pending.empty(), "Catalog has discovered unknown source");
        auto a = pending.front();
        a.detectedChannels = u.detectedChannels;
        a.layoutConfidence = u.layoutConfidence;
        db.metadata(a, u.duration, u.channels, u.sampleRate, {}, u.samples);
        catalogId = a.id;
        catalogRevision = a.revision;
        check(!db.setLayoutOverride(a.id, a.revision + 1, sp::channelLayout("5.1-side-v1")) &&
                  !db.setLayoutOverride(a.id, a.revision, sp::channelLayout("stereo-v1")),
              "Stale revisions and mismatched interpretation counts are rejected");
        check(db.setLayoutOverride(a.id, a.revision, sp::channelLayout("5.1-side-v1")),
              "Explicit source interpretation accepted");
    }
    {
        sp::Catalog db(catalogFile);
        auto a = db.asset(catalogId);
        check(a.revision == catalogRevision && a.detectedChannels.id == "unknown-6-v1" &&
                  a.layoutOverride.id == "5.1-side-v1" && a.layoutConfidence == "unknown",
              "Catalog reopen preserves unknown detection separately from chosen interpretation");
    }
    // Hold the writer migration behind startup recovery: read-only source resolution
    // must work on both pre-layout schemas, then survive the schema commit.
    for (int schema : {4, 5})
    {
        auto migration = root.getChildFile("startup-schema-" + juce::String(schema));
        migration.createDirectory();
        auto file = migration.getChildFile("library.sqlite");
        check(catalogFile.copyFileTo(file), "Copy isolated pre-migration catalog");
        auto draft = design(unknown, sp::discreteLayout(6));
        {
            sp::Catalog db(file);
            draft.original.layers[0].source = db.asset(catalogId);
            draft.original.layers[0].external = false;
            db.exec("ALTER TABLE assets DROP COLUMN layout_override");
            if (schema == 4)
                db.exec("ALTER TABLE assets DROP COLUMN channel_layout");
            db.exec(("PRAGMA user_version=" + juce::String(schema)).toRawUTF8());
        }
        draft.revision = 37;
        sp::DesignRecoveryStore(migration.getChildFile("designer-autosave")).retain(draft, {}, true);
        std::deque<std::function<void()>> held;
        sp::DesignSessionServices services;
        services.timers = false;
        services.submit = [&](auto job)
        {
            held.push_back(std::move(job));
            return true;
        };
        services.post = [](auto job) { job(); };
        sp::DesignSession startup(file, services);
        while (!held.empty())
        {
            auto job = std::move(held.front());
            held.pop_front();
            job();
        }
        check(startup.document.id == draft.id && startup.document.revision == 37 && startup.dirty &&
                  startup.document.original.layers[0].source.error.isEmpty(),
              "Automatic recovery before schema migration retains the real document and sources");
        sp::Catalog before(file, true);
        check(before.asset(catalogId).id == catalogId && before.asset(catalogId).layoutOverride.id.isEmpty(),
              "Old read-only schema has no invented interpretation override");
        {
            sp::Catalog writer(file);
            check(writer.asset(catalogId).revision == catalogRevision,
                  "Background schema migration preserves source revisions");
        }
        check(before.asset(catalogId).id == catalogId,
              "Already-open recovery reader remains valid after migration commits");
    }
    // Real mixed-rate/channel/duration sources through linked operations and selection render.
    auto shortMono = root.getChildFile("mixed-44k-mono.wav"),
         shortStereo = root.getChildFile("mixed-96k-stereo.wav");
    fixture(shortMono, sp::channelLayout("mono-v1"), signal(1, 2205), 44100);
    fixture(shortStereo, sp::channelLayout("stereo-v1"), signal(2, 7200), 96000);
    auto mixed = design(root.getChildFile("5.1-side-v1.wav"), sp::channelLayout("5.1-side-v1"));
    for (auto f : {shortMono, shortStereo})
    {
        auto l = sp::makeDesignLayer(sp::inspectDesignSource(f), 0, -1, true);
        sp::initialiseRouting(l, mixed.original.layout);
        l.placement = 100;
        l.reverse = true;
        l.fadeIn = 80;
        l.fadeOut = 90;
        l.fadeOutShape = sp::FadeShape::custom;
        l.fadeOutPoints = {{.4, .8}};
        l.rate = .9;
        l.routing.routes = {{0, 4, -3, true, false}};
        if (l.routing.inputs.size() == 2)
            l.routing.routes.push_back({1, 5, -6, false, false});
        mixed.original.layers.push_back(l);
    }
    auto mixedPcm = read(mixed, 44100, .1);
    auto sum = mixedPcm;
    sum.clear();
    for (size_t i = 0; i < mixed.original.layers.size(); ++i)
    {
        auto alone = mixed;
        for (size_t j = 0; j < alone.original.layers.size(); ++j)
            alone.original.layers[j].mute = j != i;
        auto one = read(alone, 44100, .1);
        check(one.getNumSamples() == sum.getNumSamples(), "Mixed source linked alignment");
        for (int c = 0; c < 6; ++c)
            sum.addFrom(c, 0, one, c, 0, one.getNumSamples());
    }
    check(difference(mixedPcm, sum) < 1e-6,
          "Mixed48/44.1/96k mono/stereo/surround edits retain independent channel sums");
    auto selected = mixed.renderSettings;
    selected.sampleRate = 44100;
    mixed.selectedLayer = mixed.original.layers.back().id;
    mixed.renderSettings = selected;
    sp::DesignOutput selection;
    {
        sp::AudioClient a(application);
        juce::PropertiesFile prefs(root.getChildFile("selection-ui/settings"), sp::ui::preferenceOptions());
        sp::DesignerView view(root.getChildFile("selection-ui/library.sqlite"), a, prefs);
        selection = sp::LayoutViewTestAccess::selection(view, mixed, root.getChildFile("selection"));
    }
    auto selectedPcm = independentRead(juce::File(selection.path));
    check(selectedPcm.channels == 6 &&
              selectedPcm.pcm.getMagnitude(0, 0, selectedPcm.pcm.getNumSamples()) == 0 &&
              selectedPcm.pcm.getMagnitude(4, 0, selectedPcm.pcm.getNumSamples()) > .001,
          "Render Selection uses selected-layer extent and preserves explicit surround mapping");
    // Monitor assignments and solo do not rewrite the bus. Unassigned outputs never repeat right.
    sp::PcmStream monitorStream(mixed, 48000, 0);
    monitorStream.fill(512);
    juce::AudioBuffer<float> monitorPcm(8, 512);
    float monitorGain = 1;
    monitorStream.read(monitorPcm.getArrayOfWritePointers(), 8, 512, monitorGain, 1);
    for (int c = 2; c < 8; ++c)
        check(monitorPcm.getMagnitude(c, 0, 512) == 0,
              "Default monitor leaves every extra hardware output silent");
    sp::PcmStream mappedMonitor(mixed, 48000, 0);
    mappedMonitor.fill(512);
    std::array<std::atomic<int>, 16> monitorMap;
    for (int c = 0; c < 16; ++c)
        monitorMap[c] = c < 6 ? c : -1;
    mappedMonitor.read(monitorPcm.getArrayOfWritePointers(), 8, 512, monitorGain, 1, &monitorMap, 4);
    for (int c = 0; c < 8; ++c)
        check(c == 4 ? monitorPcm.getMagnitude(c, 0, 512) > .001 : monitorPcm.getMagnitude(c, 0, 512) == 0,
              "Explicit monitor assignment and monitor-only solo select the actual channel");
    // Shared normalization must retain correlated interchannel balance.
    auto correlated = root.getChildFile("correlated.wav");
    auto corr = signal(2, 2048, -1, true);
    for (int n = 0; n < 2048; ++n)
        corr.setSample(1, n, corr.getSample(0, n) * .5f);
    fixture(correlated, sp::channelLayout("stereo-v1"), corr);
    auto linked = design(correlated, sp::channelLayout("stereo-v1"));
    linked.renderSettings.normalize = true;
    auto linkedOutput = sp::renderDesign(
        linked, root.getChildFile("linked"), linked.renderSettings, [] { return false; }, [](double) {});
    auto norm = independentRead(juce::File(linkedOutput.path));
    double normError = 0;
    for (int n = 0; n < 2048; ++n)
        normError =
            std::max(normError, (double)std::abs(norm.pcm.getSample(0, n) - norm.pcm.getSample(1, n) * 2));
    check(normError < 1e-6 && std::abs(norm.pcm.getMagnitude(0, 2048) - std::pow(10., -1. / 20)) < 1e-6,
          "Linked normalization uses one gain and preserves channel balance");
    linked.renderSettings.normalize = false;
    linked.renderSettings.dither = true;
    linked.renderSettings.bits = 16;
    auto noiseA = sp::renderDesign(
        linked, root.getChildFile("dither"), linked.renderSettings, [] { return false; }, [](double) {});
    auto noiseB = sp::renderDesign(
        linked, root.getChildFile("dither"), linked.renderSettings, [] { return false; }, [](double) {});
    check(noiseA.sha256 == noiseB.sha256, "Channel-seeded dither is repeatable");
    // Silence isolates the actual quantization noise, rather than comparing program material.
    auto silenceFile = root.getChildFile("dither-silence.wav");
    juce::AudioBuffer<float> silence(6, 8192);
    silence.clear();
    fixture(silenceFile, sp::channelLayout("5.1-side-v1"), silence);
    auto noiseDoc = design(silenceFile, sp::channelLayout("5.1-side-v1"));
    noiseDoc.renderSettings.bits = 16;
    noiseDoc.renderSettings.dither = true;
    auto noiseOut = sp::renderDesign(
        noiseDoc, root.getChildFile("noise"), noiseDoc.renderSettings, [] { return false; }, [](double) {});
    juce::WavAudioFormat noiseFormat;
    std::unique_ptr<juce::AudioFormatReader> nr(
        noiseFormat.createReaderFor(juce::File(noiseOut.path).createInputStream().release(), true));
    juce::AudioBuffer<float> noise(6, 8192);
    check(nr && nr->read(&noise, 0, 8192, 0, true, true), "Read finalized quantized channel noise");
    for (int c = 1; c < 6; ++c)
    {
        double dot = 0, aa = 0, bb = 0;
        int unequal = 0;
        for (int n = 0; n < 8192; ++n)
        {
            double x = noise.getSample(0, n), y = noise.getSample(c, n);
            dot += x * y;
            aa += x * x;
            bb += y * y;
            unequal += x != y;
        }
        check(aa > 0 && bb > 0 && unequal > 500 && std::abs(dot / std::sqrt(aa * bb)) < .1,
              "Actual per-channel dither has nonidentical decorrelated noise");
    }
    auto tooLong = linked;
    tooLong.original.duration = 48000LL * 3600;
    tooLong.renderSettings.sampleRate = 96000;
    sp::setMixLayout(tooLong.original, sp::discreteLayout(16));
    tooLong.renderSettings.layout = "same";
    bool rejectedSize = false;
    try
    {
        sp::renderDesign(
            tooLong, root.getChildFile("oversize"), tooLong.renderSettings, [] { return false; },
            [](double) {});
    }
    catch (const std::exception &e)
    {
        rejectedSize = juce::String(e.what()).contains("4GiB");
    }
    check(rejectedSize, "Oversized unqualified container is rejected before work");
    auto anti = root.getChildFile("anti.wav");
    fixture(anti, sp::channelLayout("stereo-v1"), signal(2, 2048, -1, true));
    auto a = design(anti, sp::channelLayout("stereo-v1"));
    auto mono = a.renderSettings;
    mono.layout = "mono-v1";
    auto antiOut = sp::renderDesign(a, root.getChildFile("anti"), mono, [] { return false; }, [](double) {});
    check(independentRead(juce::File(antiOut.path)).pcm.getMagnitude(0, 2048) == 0,
          "Explicit stereo to mono coefficients cancel anti-phase content");
    auto monoFile = root.getChildFile("point.wav");
    fixture(monoFile, sp::channelLayout("mono-v1"), signal(1, 48000 * 3));
    auto spatial = design(monoFile, sp::channelLayout("5.1-side-v1"));
    auto &point = spatial.original.layers[0];
    point.routing.spatial = true;
    point.routing.azimuth = -30;
    point.effects = {sp::makeStockEffect("filter"), sp::makeStockEffect("delay")};
    auto left = read(spatial);
    double other = 0;
    for (int c = 1; c < 6; ++c)
        other = std::max(other, (double)left.getMagnitude(c, 0, left.getNumSamples()));
    check(other < 1e-7 && left.getMagnitude(0, 0, left.getNumSamples()) > .01,
          "Spatial L position retains point identity and defaults LFE off");
    auto lfeDesign = spatial;
    lfeDesign.original.layers[0].effects.clear();
    lfeDesign.original.layers[0].routing.lfeDb = -6;
    lfeDesign.original.layers[0].routing.lfeCutoff = 200;
    auto lfeWide = read(lfeDesign);
    lfeDesign.original.layers[0].routing.lfeCutoff = 20;
    auto lfeNarrow = read(lfeDesign);
    check(lfeWide.getMagnitude(3, 1000, lfeWide.getNumSamples() - 1000) > .001 &&
              lfeNarrow.getMagnitude(3, 1000, lfeNarrow.getNumSamples() - 1000) <
                  lfeWide.getMagnitude(3, 1000, lfeWide.getNumSamples() - 1000) * .1,
          "Explicit point LFE send uses its separate lowpass while default-off remains silent");
    auto stereoPointFile = root.getChildFile("stereo-point.wav");
    fixture(stereoPointFile, sp::channelLayout("stereo-v1"), signal(2, 48000, -1, true));
    auto stereoPoint = design(stereoPointFile, sp::channelLayout("5.1-side-v1"));
    stereoPoint.original.layers[0].routing.spatial = true;
    stereoPoint.original.layers[0].routing.width = 0;
    check(read(stereoPoint).getMagnitude(0, 48000) == 0,
          "Coincident anti-phase stereo images cancel without added LFE");
    stereoPoint.original.layers[0].routing.width = 60;
    auto stereoFrozen = sp::freezeTake(stereoPoint, "Stereo width and wrap");
    stereoFrozen.generation = 88;
    sp::hashTake(stereoFrozen);
    auto stereoCapture = std::make_shared<sp::TakeCapture>(stereoFrozen);
    auto stereoBase = stereoPoint;
    stereoBase.takeProcessing = true;
    sp::PcmStream stereoStream(stereoBase, 48000, 0);
    stereoStream.startTake(stereoCapture);
    std::array<std::atomic<int>, 16> stereoIdentity;
    for (int c = 0; c < 16; ++c)
        stereoIdentity[c] = c;
    juce::AudioBuffer<float> stereoConsumed(6, 6144), stereoBlock(6, 512);
    float stereoGain = 1;
    for (int k = 0; k < 12; ++k)
    {
        if (k == 2 || k == 6)
        {
            auto c = sp::mixControls(stereoPoint.original);
            c.spatial[0].width = k == 2 ? 240 : 0;
            c.spatial[0].azimuth = k == 2 ? 179 : -179;
            stereoStream.updatePerformance(c);
        }
        stereoStream.fill(512);
        check(stereoStream.read(stereoBlock.getArrayOfWritePointers(), 6, 512, stereoGain, 1,
                                &stereoIdentity) == 512,
              "Stereo spatial bus consumption");
        for (int c = 0; c < 6; ++c)
            stereoConsumed.copyFrom(c, k * 512, stereoBlock, c, 0, 512);
    }
    stereoCapture->finish(stereoStream.acceptedFrame(), true, stereoStream.consumedOutputFrames());
    auto stereoTake = stereoCapture->snapshot();
    stereoPoint.takes = {stereoTake};
    stereoPoint.activeTake = stereoTake.id;
    check(stereoTake.complete && stereoTake.events.size() == 2 &&
              difference(stereoConsumed, read(stereoPoint)) < 1e-6,
          "Typed stereo width/orientation and shortest-wrap motion replay on every channel");
    double discontinuity = 0;
    auto previous = sp::spatialGains(spatial.original.layout, -180);
    for (double angle = -179.9; angle <= 180; angle += .1)
    {
        auto g = sp::spatialGains(spatial.original.layout, angle);
        double energy = 0;
        for (int c = 0; c < 6; ++c)
        {
            energy += g[c] * g[c];
            discontinuity = std::max(discontinuity, (double)std::abs(g[c] - previous[c]));
        }
        check(std::abs(energy - 1) < 1e-6 && g[3] == 0, "360-degree equal-power ring excludes LFE");
        previous = g;
    }
    check(discontinuity < .01, "Spatial interpolation is continuous at speaker and wrap boundaries");
    // Controlled producer consumes every bus channel, independently of monitor fold-down.
    auto frozen = sp::freezeTake(spatial, "Spatial captured");
    frozen.generation = 7;
    sp::hashTake(frozen);
    auto capture = std::make_shared<sp::TakeCapture>(frozen);
    auto base = spatial;
    base.takeProcessing = true;
    sp::PcmStream producer(base, 48000, 0);
    producer.startTake(capture);
    std::array<std::atomic<int>, 16> identity;
    for (int c = 0; c < 16; ++c)
        identity[c] = c;
    juce::AudioBuffer<float> original(6, 512 * 48), block(6, 512);
    float gain = 1;
    for (int i = 0; i < 48; ++i)
    {
        if (i == 2 || i == 20)
        {
            auto c = sp::mixControls(spatial.original);
            c.spatial[0].azimuth = i == 2 ? 90 : -120;
            producer.updatePerformance(c);
        }
        producer.fill(512);
        check(producer.read(block.getArrayOfWritePointers(), 6, 512, gain, 1, &identity) == 512,
              "Controlled multichannel callback consumption");
        for (int c = 0; c < 6; ++c)
            original.copyFrom(c, i * 512, block, c, 0, 512);
    }
    capture->finish(producer.acceptedFrame(), true, producer.consumedOutputFrames());
    auto take = capture->snapshot();
    check(take.complete && take.events.size() == 2 && take.processing == "stock-layout-takes-v2",
          "Typed bounded spatial applied events finalized");
    auto replay = spatial;
    replay.takes = {take};
    replay.activeTake = take.id;
    auto replayPcm = read(replay);
    double replayError = difference(original, replayPcm);
    check(replayError < 1e-6, "Spatial applied capture replays on all channels with consumed-frame identity");
    auto render = sp::renderDesign(
        replay, root.getChildFile("take"), replay.renderSettings, [] { return false; }, [](double) {});
    check(difference(independentRead(juce::File(render.path)).pcm, replayPcm) < 1e-6,
          "Spatial take render matches capture/replay on every channel");
    // Shared cache identity, per-channel extrema and byte ceilings use real decoded data.
    auto detailAsset = sp::inspectDesignSource(root.getChildFile("5.1-side-v1.wav"));
    std::vector<sp::Asset> details;
    for (int c = 0; c < 6; ++c)
    {
        auto v = detailAsset;
        v.waveformChannel = c;
        v.waveformLayoutKey = "5.1-side-v1";
        details.push_back(v);
    }
    auto pool = std::make_shared<sp::WaveformPool>(root.getChildFile("channel-peaks"));
    pool->demand(0, {detailAsset});
    pool->demand(4, details);
    until(
        [&]
        {
            pool->tick();
            if (!pool->get(detailAsset))
                return false;
            for (auto &a : details)
                if (!pool->get(a))
                    return false;
            return true;
        },
        "Shared scheduler adopts real selected-channel waveforms");
    auto all = pool->get(detailAsset);
    for (int c = 0; c < 6; ++c)
    {
        auto one = pool->get(details[(size_t)c]);
        check(one->status == sp::WaveformData::Status::ready && one->peaks.size() <= 2048,
              "Per-channel detail has its own bounded peak identity");
        for (size_t n = 0; n < one->peaks.size(); ++n)
            check(all->peaks[n].first <= one->peaks[n].first && all->peaks[n].second >= one->peaks[n].second,
                  "Overview conservatively contains each individual channel extrema");
    }
    auto alternate = details[0];
    alternate.waveformLayoutKey = "5.1-rear-v1";
    check(sp::waveformSourceKey(alternate) != sp::waveformSourceKey(details[0]) &&
              sp::waveformSourceKey(details[0]) != sp::waveformSourceKey(detailAsset),
          "Waveform identity includes layout/selection/detail version");
    until(
        [&]
        {
            pool->tick();
            return !(bool)pool->metrics()["pending"];
        },
        "Detail demand settles");
    auto work = (int64_t)pool->metrics()["requests"];
    for (int i = 0; i < 100; ++i)
        pool->tick();
    check((int64_t)pool->metrics()["requests"] == work &&
              (int64_t)pool->metrics()["retainedPeakBytes"] <= 48 * 65536 * 8 &&
              (int64_t)pool->metrics()["displayPeakBytes"] <= 160 * 2048 * 8,
          "Channel detail does not multiply resident source/summary budgets or churn settled work");
    pool->demand(4, {});
    // Multiple output layouts/sets retain actual identity across preset changes and reopen.
    auto outputDoc = spatial;
    outputDoc.takes = {take};
    outputDoc.activeTake = take.id;
    auto surround = render;
    auto stereoSettings = outputDoc.renderSettings;
    stereoSettings.layout = "stereo-v1";
    auto stereoOut = sp::renderDesign(
        outputDoc, root.getChildFile("outputs"), stereoSettings, [] { return false; }, [](double) {});
    auto splitSettings = outputDoc.renderSettings;
    splitSettings.splitMono = true;
    auto splitOut = sp::renderDesign(
        outputDoc, root.getChildFile("outputs"), splitSettings, [] { return false; }, [](double) {});
    outputDoc.outputs = {surround, stereoOut, splitOut};
    sp::DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.status = [] { return sp::message("status"); };
    services.stop = [] {};
    services.resolve = [](auto &) {};
    sp::DesignSession outputSession(root.getChildFile("output-session/library.sqlite"), services);
    outputSession.document = outputDoc;
    outputSession.selectTake(take.id);
    outputSession.chooseOutput(surround.path);
    outputSession.editMetadata("Export preset", [](auto &d) { d.renderSettings.layout = "stereo-v1"; });
    check(outputSession.readyOutput() == juce::File(surround.path) &&
              surround.channelProvenance["export"]["id"].toString() == "5.1-side-v1",
          "Export preset cannot relabel a selected existing5.1 render");
    auto outputSave = root.getChildFile("Output choices.soundplus");
    sp::saveDesignAtomic(outputSave, outputSession.document);
    outputSession.document = sp::loadDesign(outputSave);
    outputSession.selectTake(take.id);
    check(outputSession.readyOutput() == juce::File(surround.path),
          "Actual finalized output choice survives reopen");
    outputSession.chooseOutput(splitOut.path);
    check(outputSession.readyFiles().size() == 6, "Ready split delivery binds the complete actual file set");
    auto firstStem = juce::File(splitOut.files[0]["path"].toString()),
         heldStem = firstStem.getSiblingFile("held-owned-stem.wav");
    check(firstStem.moveFileTo(heldStem), "Temporarily remove actual split stem");
    check(outputSession.readyOutput() != juce::File(splitOut.path),
          "Partially missing output set cannot remain ready");
    check(heldStem.moveFileTo(firstStem), "Restore durable stem");
    outputSession.chooseOutput(splitOut.path);
    outputSession.deleteTake();
    outputSession.undo();
    check(outputSession.document.activeTake == take.id && outputSession.readyFiles().size() == 6 &&
              juce::File(surround.path).existsAsFile(),
          "Delete/Undo retains layout take, receipts and finalized set");
    // Controlled same-byte relocation: original really absent; layer interpretation remains frozen.
    auto takeSource = juce::File(take.recipe->layers[0].source.path),
         relocated = root.getChildFile("relocated-point.wav");
    check(takeSource.moveFileTo(relocated), "Move actual layout take source");
    auto relocatedDoc = outputDoc;
    auto &provenance = relocatedDoc.takes[0].sources[0];
    provenance.overridePath = relocated.getFullPathName();
    auto stamp = sp::inspectDesignSource(relocated);
    provenance.overrideSize = stamp.size;
    provenance.overrideModified = stamp.modified;
    provenance.overrideInode = stamp.inode;
    auto relocatedOut = sp::renderDesign(
        relocatedDoc, root.getChildFile("relocated"), relocatedDoc.renderSettings, [] { return false; },
        [](double) {});
    check(relocatedOut.recipeHash == take.recipeHash && relocatedOut.eventHash == take.eventHash &&
              relocatedOut.sourceProvenance[0]["effectivePath"].toString() == relocated.getFullPathName(),
          "Layout take relocation preserves recipe interpretation and effective-source receipt");
    check(relocated.moveFileTo(takeSource), "Restore exact source inode for native replay fixture");
    // Real helper: multichannel pre-monitor PCM, stereo monitoring, spatial events and immutable replay.
    sp::AudioClient audio(application);
    until([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native device available");
    audio.deviceRate(48000);
    audio.bufferSize(512);
    until(
        [&]
        {
            auto s = audio.status();
            return (double)s["sampleRate"] == 48000 && (int)s["bufferFrames"] == 512;
        },
        "Native48k512 configured");
    audio.gain(.04f);
    auto draft = spatial;
    auto t = sp::freezeTake(draft, "Native surround motion");
    t.checkpoint = root.getChildFile("native-checkpoint.soundplus").getFullPathName();
    draft.takes = {t};
    auto snapshot = root.getChildFile("native-draft.soundplus");
    sp::saveDesignAtomic(snapshot, draft);
    auto raw = root.getChildFile("native-original.f32");
    audio.reserveTake(true);
    auto generation = audio.playDesign("design:" + draft.id, snapshot, 0, raw, "record");
    until(
        [&]
        {
            auto s = audio.status();
            return (bool)s["playing"] && (int)s["busChannels"] == 6;
        },
        "Native six-channel graph playing through stereo monitor");
    for (int i = 0; i < 60; ++i)
    {
        auto edited = draft;
        edited.original.layers[0].routing.azimuth = -160 + i * 5.;
        audio.updateDesign(edited, generation, sp::monotonicMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto nativeRecordingStatus = audio.status();
    audio.finishTake();
    until(
        [&]
        {
            auto s = audio.status();
            return (int)s["takePhase"] == 3 && (bool)s["takeCheckpointReady"];
        },
        "Native spatial capture durable Ready");
    audio.reserveTake(false);
    auto captured = sp::loadDesign(juce::File(t.checkpoint)).takes[0];
    check(captured.complete && captured.events.size() > 2, "Native applied spatial motion retained");
    auto nativeDoc = spatial;
    nativeDoc.takes = {captured};
    nativeDoc.activeTake = captured.id;
    nativeDoc.renderSettings.tailSeconds = .1;
    auto saved = root.getChildFile("Native surround take.soundplus");
    sp::saveDesignAtomic(saved, nativeDoc);
    auto expectedNative = read(nativeDoc, 48000, .1);
    auto capturePcm = rawCapture(raw, 6, (int)captured.outputFrames);
    double nativeError = difference(capturePcm, expectedNative, (int)captured.outputFrames);
    check(nativeError < 1e-6, "Real captured pre-monitor six-channel PCM matches immutable replay");
    auto replayRaw = root.getChildFile("native-replay.f32");
    audio.playDesign("design:" + nativeDoc.id, saved, 0, replayRaw);
    until(
        [&]
        {
            auto s = audio.status();
            return (bool)s["playing"] && (bool)s["immutableReplay"];
        },
        "Native immutable layout take playback");
    auto live = nativeDoc;
    live.activeTake.clear();
    live.original.layers.clear();
    audio.updateDesign(live, (uint64_t)(int64_t)audio.status()["playGeneration"], sp::monotonicMs());
    audio.monitorSolo(0);
    audio.monitorChannel(0, 4);
    audio.monitorChannel(1, -1);
    until([&] { return !(bool)audio.status()["playing"]; },
          "Native immutable replay completes despite mismatched Live/monitor edits");
    check(difference(rawCapture(replayRaw, 6, expectedNative.getNumSamples()), expectedNative) < 1e-6,
          "Monitor route/solo/gain never changes native pre-monitor PCM");
    auto rs = nativeDoc.renderSettings;
    rs.sampleRate = 44100;
    auto out = sp::renderDesign(
        nativeDoc, root.getChildFile("native-render"), rs, [] { return false; }, [](double) {});
    check(difference(independentRead(juce::File(out.path)).pcm, read(nativeDoc, 44100, .1)) < 1e-6,
          "Native take resampled export retains per-channel stateful tails");
    audio.stop();
    for (int c = 0; c < 16; ++c)
        audio.monitorChannel(c, -2);
    audio.monitorSolo(-1);
    // R10 against the actual helper, including a held session preparation.
    std::deque<std::function<void()>> jobs;
    int nativePlayCalls = 0;
    sp::DesignSessionServices nativeServices;
    nativeServices.automaticRecovery = nativeServices.timers = false;
    nativeServices.status = [&] { return audio.status(); };
    nativeServices.stop = [&] { audio.stop(); };
    nativeServices.play = [&](auto &id, auto &file, int64_t first, auto &mode)
    {
        ++nativePlayCalls;
        return audio.playDesign(id, file, first, {}, mode);
    };
    nativeServices.parameters = [&](auto &d, uint64_t g, double ms) { audio.updateDesign(d, g, ms); };
    nativeServices.resolve = [](auto &) {};
    nativeServices.retain = [](auto &, auto &, bool) {};
    nativeServices.submit = [&](auto f)
    {
        jobs.push_back(std::move(f));
        return true;
    };
    nativeServices.post = [](auto f) { f(); };
    sp::DesignSession nativeSession(root.getChildFile("native-context/library.sqlite"), nativeServices);
    auto flush = [&]
    {
        while (!jobs.empty())
        {
            auto f = std::move(jobs.front());
            jobs.pop_front();
            f();
        }
    };
    auto prime = [&]
    {
        nativeSession.document = nativeDoc;
        nativeSession.selectTake(captured.id);
        nativeSession.deleteTake();
    };
    prime();
    nativeSession.play();
    flush();
    until(
        [&]
        {
            auto s = audio.status();
            return (bool)s["playing"] && (double)s["preMonitorPeak"] > 1e-8 &&
                   s["playbackContext"].toString() == sp::designPlaybackContext(nativeSession.document);
        },
        "Actual helper owns active Live context before Undo");
    nativeSession.undo();
    until(
        [&]
        {
            auto s = audio.status();
            return !(bool)s["playing"] && s["playGeneration"] == s["intentGeneration"];
        },
        "Undo restoring Take stops actual Live stream");
    check(nativeSession.document.activeTake == captured.id, "Native context display is restored Take");
    nativeSession.play();
    flush();
    until(
        [&]
        {
            auto s = audio.status();
            return (bool)s["playing"] && (double)s["preMonitorPeak"] > 1e-8 &&
                   s["playbackContext"].toString() == sp::designPlaybackContext(nativeSession.document);
        },
        "Actual helper owns restored immutable Take context");
    nativeSession.redo();
    until(
        [&]
        {
            auto s = audio.status();
            return !(bool)s["playing"] && s["playGeneration"] == s["intentGeneration"];
        },
        "Redo restoring Live stops actual Take stream");
    prime();
    nativeSession.play();
    check(!jobs.empty(), "Native session preview held before play");
    auto calls = nativePlayCalls;
    nativeSession.undo();
    flush();
    check(nativeSession.document.activeTake == captured.id && nativePlayCalls == calls,
          "Held stale Live completion never reaches actual helper after Take restoration");
    // Legacy canonical hashes and original WAV bytes must remain unchanged.
    auto repo = application.getParentDirectory();
    while (!repo.getChildFile("AGENTS.md").existsAsFile() && repo != repo.getParentDirectory())
        repo = repo.getParentDirectory();
    auto historical = repo.getChildFile("docs/verification/performance-v13-native-qa.soundplus.json.gz");
    check(historical.existsAsFile(), "Committed historical compatibility fixture exists");
    if (historical.existsAsFile())
    {
        juce::GZIPDecompressorInputStream gz(historical.createInputStream().release(), true,
                                             juce::GZIPDecompressorInputStream::gzipFormat);
        auto legacyFile = root.getChildFile("Legacy.soundplus");
        legacyFile.replaceWithText(gz.readEntireStreamAsString());
        auto legacy = sp::loadDesign(legacyFile);
        auto before = legacy.takes[0].recipeHash, eventHash = legacy.takes[0].eventHash;
        sp::saveDesignAtomic(root.getChildFile("Legacy reopened.soundplus"), legacy);
        auto reopened = sp::loadDesign(root.getChildFile("Legacy reopened.soundplus"));
        check(reopened.takes[0].recipeHash == before && reopened.takes[0].eventHash == eventHash,
              "Historical manifest4 hashes survive load/save/reopen unchanged");
        // Recreate the historical PCM in an owned directory so this regression
        // does not depend on the original developer machine's absolute paths.
        auto inputs = root.getChildFile("historical-generated-inputs");
        prepareTakeUiFixture(inputs);
        auto renderCopy = legacy;
        auto &take = renderCopy.takes[0];
        for (auto &source : take.sources)
        {
            auto layer = std::find_if(take.recipe->layers.begin(), take.recipe->layers.end(),
                                      [&](auto &l) { return l.source.id == source.id; });
            check(layer != take.recipe->layers.end(), "Historical source target exists");
            auto file = inputs.getChildFile(juce::File(layer->source.path).getFileName());
            check(hash(file) == source.sha256, "Regenerated historical source has identical bytes");
            sp::FileStamp stamp;
            check(sp::fileStamp(file, stamp), "Owned historical source stamp");
            source.overridePath = file.getFullPathName();
            source.overrideSize = stamp.size;
            source.overrideModified = stamp.modified;
            source.overrideInode = stamp.inode;
        }
        check(take.recipeHash == before && take.eventHash == eventHash,
              "Explicit same-byte test relink leaves canonical historical identity unchanged");
        auto o = sp::renderDesign(
            renderCopy, root.getChildFile("legacy-render"), legacy.renderSettings, [] { return false; },
            [](double) {});
        check(o.sha256 == legacy.outputs.back().sha256,
              "Legacy stock-takes-v1 PCM/container parity matches historical delivered WAV");
        sp::setMixLayout(legacy.activeMix(), sp::channelLayout("5.1-side-v1"));
        legacy.renderSettings.layout = "same";
        sp::saveDesignAtomic(root.getChildFile("Converted legacy.soundplus"), legacy);
        auto converted = sp::loadDesign(root.getChildFile("Converted legacy.soundplus"));
        check(converted.takes[0].recipeHash == before && converted.takes[0].eventHash == eventHash &&
                  converted.outputs.back().sha256 == reopened.outputs.back().sha256,
              "Deliberate Live conversion retains historical take/receipt identity");
    }
    auto result = sp::message("multichannel-results");
    auto *o = result.getDynamicObject();
    o->setProperty("checks", checks);
    o->setProperty("preallocatedEventBytes", (juce::int64)(sizeof(sp::TakeEvent) * sp::maxTakeEvents));
    o->setProperty("layouts", records);
    o->setProperty("controlledCaptureReplayMaxError", replayError);
    o->setProperty("nativeCaptureReplayMaxError", nativeError);
    o->setProperty("nativeR10HelperOwnership", true);
    o->setProperty("portableHistoricalSources", true);
    o->setProperty("nativeTake", sp::takeToJson(captured));
    o->setProperty("nativeRecordingStatus", nativeRecordingStatus);
    o->setProperty("nativeCapturePath", raw.getFullPathName());
    o->setProperty("nativeReplayPath", replayRaw.getFullPathName());
    o->setProperty("comparedNativeFrames", (juce::int64)captured.outputFrames);
    o->setProperty("nativeOutput", out.path);
    o->setProperty("nativeStatus", audio.status());
    o->setProperty("physicalSurroundMonitoring", false);
    o->setProperty("humanListening", false);
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(result, true));
    std::cout << "PASS multichannel identity, processing, spatial takes, delivery: " << checks
              << " checks; native error " << nativeError << "\n";
}
