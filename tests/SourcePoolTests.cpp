#include "AudioPlaybackService.h"
#include "DesignAudio.h"
#include "PerformanceState.h"
#include "SourcePools.h"
#include "DesignSession.h"
#include "DesignLive.h"
#include "AudioStream.h"
#include "TakeCapture.h"
#include "UIComponents.h"
#include "SourcePoolView.h"
#include <juce_cryptography/juce_cryptography.h>
#include <deque>
#include <iostream>
namespace sp
{
struct SourcePoolViewTestAccess
{
    static void focus(SourcePoolView &v, int row)
    {
        v.list.selectRow(row);
    }
    static void enter(SourcePoolView &v, int row)
    {
        v.returnKeyPressed(row);
    }
    static void next(SourcePoolView &v)
    {
        v.next.onClick();
    }
    static void previous(SourcePoolView &v)
    {
        v.previous.onClick();
    }
    static void lock(SourcePoolView &v)
    {
        v.lock.onClick();
    }
    static juce::String name(SourcePoolView &v, int row)
    {
        return v.getNameForRow(row);
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
template <class F> void rejects(F f, const char *why)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, why);
}
juce::String json(const juce::var &v)
{
    return juce::JSON::toString(v, true);
}
sp::Asset fixture(const juce::File &f, int channels = 1, int rate = 48000, int frames = 48000,
                  sp::ChannelLayout layout = {}, bool shaped = false)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::AudioBuffer<float> b(channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < frames; ++n)
            b.setSample(
                c, n,
                shaped
                    ? .38f * (float)std::sin((n * (.026 + c * .002))) *
                          (float)(.12 + .88 * std::pow(std::sin(n / (double)rate * 3.141592653589793 * 2), 4))
                    : (float)(c + 1) * .01f * (n % 257 == 0 ? .5f : 1.f));
    if (channels > 16 || layout.id.startsWith("unknown-"))
    {
        // Untagged float RIFF: independent physical-channel fixture, never guessed surround.
        auto out = f.createOutputStream();
        int bytes = channels * frames * 4;
        out->write("RIFF", 4);
        out->writeInt(36 + bytes);
        out->write("WAVEfmt ", 8);
        out->writeInt(16);
        out->writeShort(3);
        out->writeShort((short)channels);
        out->writeInt(rate);
        out->writeInt(rate * channels * 4);
        out->writeShort((short)(channels * 4));
        out->writeShort(32);
        out->write("data", 4);
        out->writeInt(bytes);
        for (int n = 0; n < frames; ++n)
            for (int c = 0; c < channels; ++c)
                out->writeFloat(b.getSample(c, n));
        out.reset();
    }
    else
    {
        juce::WavAudioFormat format;
        auto out = f.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer(
            layout.id.isEmpty()
                ? format.createWriterFor(out.release(), rate, (unsigned)channels, 32, {}, 0)
                : format.createWriterFor(out.release(), rate, sp::juceChannelSet(layout), 32, {}, 0));
        check(writer && writer->writeFromAudioSampleBuffer(b, 0, frames),
              "Generated independently tagged PCM fixture finalized");
    }
    return sp::inspectDesignSource(f);
}
sp::DesignDocument document(const sp::Asset &a, bool aware = true)
{
    sp::DesignDocument d;
    d.original.masterDb = 0;
    d.original.layers = {sp::makeDesignLayer(a, 0, a.samples, true)};
    d.selectedLayer = d.original.layers[0].id;
    if (aware)
        sp::setMixLayout(d.original, sp::channelLayout("stereo-v1"));
    d.renderSettings.bits = 32;
    d.renderSettings.tailSeconds = 0;
    return d;
}
void makePool(sp::DesignDocument &d, const std::vector<sp::Asset> &assets)
{
    auto &l = d.original.layers[0];
    sp::SourcePool p;
    auto initial = sp::poolEntry(l, d.original.layout);
    sp::verifyPoolEntry(initial);
    p.selected = initial.id;
    p.entries = {initial};
    for (auto &a : assets)
    {
        auto e = sp::inspectPoolEntry(a, 0, a.samples, true);
        auto other = sp::makeDesignLayer(a, 0, a.samples, true);
        if (d.original.layout.id.isNotEmpty())
            sp::initialiseRouting(other, d.original.layout);
        sp::reviewPoolEntry(e, other.routing, d.original.layout);
        p.entries.push_back(std::move(e));
    }
    l.pool = std::move(p);
    sp::syncPoolSelection(d.original);
    sp::validateDesign(d);
}
void liveSourceOracle(const juce::File &root)
{
    using namespace sp;
    using namespace juce;
    root.createDirectory();
    for (int rate : {44100, 48000, 96000})
    {
        constexpr int length = 12000;
        auto media = [&](const String &name, float left, float right)
        {
            auto file = root.getChildFile(name + String(rate) + ".wav");
            WavAudioFormat format;
            std::unique_ptr<AudioFormatWriter> writer(
                format.createWriterFor(file.createOutputStream().release(), rate, 2, 24, {}, 0));
            AudioBuffer<float> pcm(2, length);
            for (int n = 0; n < length; n++)
            {
                pcm.setSample(0, n, left);
                pcm.setSample(1, n, right);
            }
            check(writer && writer->writeFromAudioSampleBuffer(pcm, 0, length),
                  "Write two distinct source-role constants");
            writer.reset();
            return inspectDesignSource(file);
        };
        auto base = media("bed", .125f, -.25f), alternate = media("alt", -.375f, .0625f);
        auto d = document(base);
        d.sampleRate = rate;
        d.original.layers[0].fadeIn = d.original.layers[0].fadeOut = 0;
        makePool(d, {alternate});
        auto &l = d.original.layers[0];
        addPerformancePad(d);
        assignBlendLayer(d.original, 1, l.id, 1);
        auto legacy = freezeTake(d, "legacy");
        l.pool->live = true;
        check((int)designToJson(d)["version"] == 22, "New manifest only for opt-in live bank");
        auto draft = freezeTake(d, "source events");
        check(draft.recipeHash == legacy.recipeHash,
              "Arming without an applied event preserves original recipe hash");
        TakeCapture cap(draft);
        cap.phase = TakeCapture::recording;
        DesignGraph graph(d);
        graph.seek(0);
        AudioBuffer<float> live(2, length);
        const auto entry = l.pool->entries[1].id;
        auto issue = [&](String op, int overlap, String id)
        {
            auto *o = new DynamicObject();
            var v(o);
            o->setProperty("eventId", Uuid().toString());
            o->setProperty("action", op);
            o->setProperty("layer", l.id);
            o->setProperty("pad", padIdentity(d.original, 1));
            o->setProperty("corner", 1);
            o->setProperty("entry", id);
            o->setProperty("overlap", overlap);
            o->setProperty("levelDb", 0.);
            return graph.sourceVoice(v, &cap);
        };
        graph.render(live, 0, 1200);
        auto unknown = issue("trigger", 0, "absent");
        check(!(bool)unknown["accepted"] && cap.eventCount() == 0,
              "Unprepared alternate rejects without changing audio or capture");
        auto trigger = issue("trigger", 0, entry);
        check((bool)trigger["accepted"] && (int)trigger["frame"] == 1200,
              "Applied trigger stamped at source clock");
        graph.render(live, 1200, 800);
        auto fade = rate / 10;
        auto sw = issue("switch", fade, entry);
        check((bool)sw["accepted"] && (int)sw["frame"] == 2000,
              "Applied source switch stamped independently of UI");
        graph.render(live, 2000, length - 2000);
        double maxError = 0;
        for (int n = 0; n < length; n++)
            for (int c = 0; c < 2; c++)
            {
                double original = c ? -.25 : .125, other = c ? .0625 : -.375;
                double phase = std::clamp(double(n - 2000) / fade, 0., 1.) * MathConstants<double>::halfPi;
                double expected =
                    original * std::cos(phase) + (n >= 1200 ? other : 0.) + other * std::sin(phase);
                maxError = std::max(maxError, std::abs(expected - live.getSample(c, n)));
            }
        std::cout << "Prepared source overlap oracle " << rate << " Hz max error " << maxError << "\n";
        check(maxError < 2e-5,
              "Every independently predicted role sample including crossfade and one-shot overlap");
        cap.finish(length, true, length);
        auto take = cap.snapshot();
        check(take.sourcePerformance.bindings.size() == 1 && take.sourcePerformance.events.size() == 2 &&
                  take.recipeHash != legacy.recipeHash,
              "Frozen typed events deduplicate resolved source and opt into new identity");
        d.takes = {take};
        d.activeTake = take.id;
        auto save = root.getChildFile("live-" + String(rate) + ".soundplus");
        saveDesignAtomic(save, d);
        auto reopened = loadDesign(save);
        check(reopened.takes[0].eventHash == take.eventHash, "Saved take retains immutable typed event hash");
        DesignReader replay(reopened, rate, 0, length, 0);
        AudioBuffer<float> block(2, 257);
        for (int n = 0; n < length;)
        {
            int got = replay.read(block, std::min(257, length - n));
            check(got > 0, "Replay progressed");
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < got; k++)
                    if (std::abs(block.getSample(c, k) - live.getSample(c, n + k)) > 2e-6)
                        throw std::runtime_error("Frozen source replay mismatch at " + std::to_string(n + k));
            n += got;
        }
        DesignReader seek(reopened, rate, 2777, 3400, 0);
        seek.read(block, 127);
        check(std::abs(block.getSample(0, 49) - live.getSample(0, 2826)) < 2e-6,
              "Accurate seek rebuilds both source voices within crossfade");
        auto altered = take;
        altered.sourcePerformance.events[0].frame++;
        bool rejected = false;
        try
        {
            validateTake(altered);
        }
        catch (...)
        {
            rejected = true;
        }
        check(rejected, "Changed event cannot inherit existing take checksum");
    }
}
void recordedPoolService(const juce::File &root, const sp::Asset &a, const sp::Asset &b)
{
    using namespace sp;
    using namespace juce;
    root.createDirectory();
    auto d = document(a);
    makePool(d, {b});
    auto &l = d.original.layers[0];
    l.pool->live = true;
    assignBlendLayer(d.original, 0, l.id);
    auto t = freezeTake(d, "helper pool checkpoint");
    t.checkpoint = root.getChildFile("checkpoint.soundplus").getFullPathName();
    d.takes = {t};
    auto file = root.getChildFile("draft.soundplus");
    saveDesignAtomic(file, d);
    auto capFile = root.getChildFile("producer.f32");
    {
        AudioPlaybackService helper;
        auto args = sp::message("play_design");
        auto *o = args.getDynamicObject();
        o->setProperty("path", file.getFullPathName());
        o->setProperty("id", "design:" + d.id);
        o->setProperty("mode", "record");
        o->setProperty("playGeneration", 17);
        o->setProperty("positionSample", 0);
        o->setProperty("capture", capFile.getFullPathName());
        helper.request(args, d.sampleRate);
        auto wait = [&](auto ready)
        {
            for (int i = 0; i < 4000; i++)
            {
                auto s = helper.status();
                if (ready(s))
                    return s;
                if (s["error"].toString().isNotEmpty())
                    throw std::runtime_error(s["error"].toString().toStdString());
                Thread::sleep(2);
            }
            throw std::runtime_error("Timed out waiting for helper source event");
        };
        auto running = wait([](auto &s) { return (bool)s["playing"]; });
        auto event = sp::message("source_voice");
        auto *e = event.getDynamicObject();
        e->setProperty("id", "design:" + d.id);
        e->setProperty("eventId", Uuid().toString());
        e->setProperty("graphEpoch", running["graphEpoch"]);
        e->setProperty("playGeneration", 17);
        e->setProperty("layer", l.id);
        e->setProperty("pad", padIdentity(d.original, 0));
        e->setProperty("corner", 0);
        e->setProperty("entry", l.pool->entries[1].id);
        e->setProperty("action", "switch");
        e->setProperty("overlap", 480);
        helper.squareTouch(event);
        auto applied = wait([&](auto &s) { return s["sourceVoice"]["id"] == event["eventId"]; });
        check((bool)applied["sourceVoice"]["accepted"],
              "Helper acknowledges applied source event during take recording");
        AudioBuffer<float> output(2, 128);
        for (int i = 0; i < 70; i++)
        {
            helper.render(output.getArrayOfWritePointers(), 2, 128);
            Thread::sleep(3);
        }
        helper.finishCapture(17);
        auto done = wait([](auto &s)
                         { return (bool)s["checkpointReady"] || (int)s["takePhase"] == TakeCapture::ready; });
        (void)done;
    }
    auto saved = loadDesign(File(t.checkpoint));
    saved.activeTake = saved.takes[0].id;
    auto &take = saved.takes[0];
    check(take.complete && take.sourcePerformance.events.size() == 1,
          "Helper durably checkpoints only the consumed source performance");
    MemoryBlock produced;
    check(capFile.loadFileAsData(produced) &&
              produced.getSize() >= size_t(take.outputFrames) * 2 * sizeof(float),
          "Saved pre-monitor PCM covers every consumed frame");
    DesignReader reader(saved, d.sampleRate, 0, take.bodyEnd, 0);
    AudioBuffer<float> block(2, 127);
    const auto *samples = static_cast<const float *>(produced.getData());
    for (int n = 0; n < take.outputFrames;)
    {
        const int got = reader.read(block, std::min<int64_t>(127, take.outputFrames - n));
        check(got > 0, "Checkpoint replay progressed");
        for (int i = 0; i < got; i++)
            for (int c = 0; c < 2; c++)
                if (std::abs(samples[(n + i) * 2 + c] - block.getSample(c, i)) > 2e-6)
                    throw std::runtime_error("Checkpoint differs from consumed live source PCM");
        n += got;
    }
    auto expired = root.getChildFile("changed.wav");
    check(File(b.path).copyFileTo(expired), "Isolate alternate to change");
    auto changedDoc = document(a);
    makePool(changedDoc, {inspectDesignSource(expired)});
    auto &layer = changedDoc.original.layers[0];
    layer.pool->live = true;
    assignBlendLayer(changedDoc.original, 0, layer.id);
    DesignGraph old(changedDoc);
    check(expired.appendText("changed"), "Change isolated fixture after preparation");
    auto cmd = sp::message("source_voice");
    auto *o = cmd.getDynamicObject();
    o->setProperty("eventId", Uuid().toString());
    o->setProperty("layer", layer.id);
    o->setProperty("pad", "perform:a");
    o->setProperty("corner", 0);
    o->setProperty("entry", layer.pool->entries[1].id);
    o->setProperty("action", "trigger");
    auto refused = old.sourceVoice(cmd, nullptr);
    check(!(bool)refused["accepted"] && refused["error"].toString().containsIgnoreCase("changed"),
          "Changed alternate refused before trigger");
}
struct Harness
{
    std::deque<std::function<void()>> jobs, posts;
    int stops = 0, plays = 0;
    uint64_t generation = 0;
    juce::var state = sp::message("status");
    sp::DesignSessionServices services()
    {
        sp::DesignSessionServices s;
        s.timers = s.automaticRecovery = false;
        s.submit = [this](auto job)
        {
            jobs.push_back(std::move(job));
            return true;
        };
        s.post = [this](auto job) { posts.push_back(std::move(job)); };
        s.now = [] { return 1000.; };
        s.resolve = [](auto &) {};
        s.retain = [](const auto &, const auto &, bool) {};
        s.write = [](const auto &f, const auto &d) { sp::saveDesignAtomic(f, d); };
        s.status = [this] { return state.clone(); };
        s.stop = [this]
        {
            ++stops;
            state.getDynamicObject()->setProperty("playing", false);
        };
        s.play = [this](const auto &id, const auto &, int64_t, const auto &)
        {
            ++plays;
            auto *o = state.getDynamicObject();
            o->setProperty("id", id);
            o->setProperty("playing", true);
            o->setProperty("playGeneration", (juce::int64)++generation);
            return generation;
        };
        s.parameters = [](const auto &, uint64_t, double) {};
        s.reserveTake = [](bool) {};
        s.finishTake = [](bool) {};
        return s;
    }
    void io()
    {
        check(!jobs.empty(), "Controlled worker job exists");
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
    }
    void ui()
    {
        check(!posts.empty(), "Controlled completion exists");
        auto job = std::move(posts.front());
        posts.pop_front();
        job();
    }
    void drain()
    {
        while (!jobs.empty())
            io();
        while (!posts.empty())
            ui();
    }
};
juce::AudioBuffer<float> read(const sp::DesignDocument &d, int frames = 4096)
{
    sp::DesignGraph graph(d);
    juce::AudioBuffer<float> b(sp::designChannels(d), frames);
    graph.render(b, 0, frames);
    return b;
}
double difference(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b)
{
    check(a.getNumChannels() == b.getNumChannels() && a.getNumSamples() == b.getNumSamples(),
          "PCM dimensions agree");
    double error = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < a.getNumSamples(); ++n)
            error = std::max(error, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
    return error;
}
void determinism()
{
    sp::SourcePool p;
    p.id = "pool-fixed";
    for (auto id : {"entry-c", "entry-a", "entry-b"})
    {
        sp::SourcePoolEntry e;
        e.id = id;
        p.entries.push_back(e);
    }
    const char *expected[][5] = {{"a", "a", "b", "b", "b"},
                                 {"b", "c", "b", "b", "a"},
                                 {"c", "a", "a", "b", "c"},
                                 {"c", "c", "c", "c", "a"}};
    uint64_t seeds[] = {0, 1, 42, UINT64_MAX};
    // Literal expected vectors were computed by an independent Python byte-loop,
    // not another invocation of the implementation under test.
    for (int seed = 0; seed < 4; ++seed)
        for (int n = 0; n < 5; ++n)
            check(sp::choosePoolEntry(p, seeds[seed], "layer-fixed", n) ==
                      juce::String("entry-") + expected[seed][n],
                  "Versioned deterministic source vector");
    auto choice = sp::choosePoolEntry(p, 42, "layer-fixed", 0);
    std::reverse(p.entries.begin(), p.entries.end());
    check(sp::choosePoolEntry(p, 42, "layer-fixed", 0) == choice,
          "Stable ID order is independent of UI row order");
    for (auto &e : p.entries)
        e.source.error = "Offline";
    check(sp::choosePoolEntry(p, 42, "layer-fixed", 0) == choice,
          "Availability cannot silently reroll a deterministic choice");
    for (auto &e : p.entries)
        e.enabled = false;
    rejects([&] { sp::choosePoolEntry(p, 0, "layer-fixed", 0); },
            "Disabled candidates do not enter seeded choice");
}
void model(const juce::File &root, const sp::Asset &a, const sp::Asset &b)
{
    for (bool aware : {false, true})
    {
        auto d = document(a, aware);
        auto old = json(sp::designMixToJson(d.original));
        auto before = read(d);
        auto disk = root.getChildFile(aware ? "layout.soundplus" : "legacy.soundplus");
        sp::saveDesignAtomic(disk, d);
        check(json(sp::designMixToJson(sp::loadDesign(disk).original)) == old,
              "No-pool canonical identity survives save/reopen");
        makePool(d, {b});
        check(d.original.layers[0].pool->entries.size() == 2, "Optional pool includes original source");
        check(difference(before, read(d)) == 0, "Creating optional pool does not change single-source PCM");
        check(sp::designStructure(d) == sp::designStructure(sp::audioSourceProjection(d)),
              "Inactive alternatives do not alter graph structure");
        auto source = d.original.layers[0].source.id;
        auto &l = d.original.layers[0];
        auto entry = l.pool->entries[1].id;
        l.sync = 12000;
        l.fadeIn = 48000;
        l.fadeOut = 72000;
        l.placement = 111;
        l.reverse = true;
        l.gainDb = -2;
        l.rate = 1.5;
        sp::syncPoolSelection(d.original);
        auto snapshot = l;
        sp::activatePoolEntry(l, entry, d.original.layout);
        sp::validateDesign(d);
        check(l.source.id == b.id && l.pool->selected == entry && l.reverse && l.rate == 1.5 &&
                  l.placement == 111 && l.gainDb == -2,
              "Source activation preserves independent layer controls");
        check(l.sync == (int64_t)std::llround(.25 * b.sampleRate) && l.fadeIn == 48000 && l.fadeOut == 72000,
              "Source-rate conversion preserves relative sync time and existing long fades");
        auto value = sp::designToJson(d);
        check((int)value["version"] == 7 && (int)value["sourcePoolMetadataVersion"] == 1,
              "Pools have a distinct metadata gate");
        auto oldTag = sp::designToJson(sp::audioSourceProjection(d))["dsp"].toString();
        check(value["dsp"].toString() == oldTag, "Pool metadata does not rename DSP");
        sp::saveDesignAtomic(disk, d);
        auto loaded = sp::loadDesign(disk);
        check(loaded.original.layers[0].pool->selected == entry &&
                  loaded.original.layers[0].source.id == b.id,
              "Selected stable entry/source survives save/reopen");
        auto bad = value.clone();
        bad.getDynamicObject()->setProperty("version", aware ? 6 : 1);
        rejects([&] { sp::designFromJson(bad); }, "Old manifests cannot conceal source-pool metadata");
        auto overflow = loaded;
        for (int i = 0; i < 63; ++i)
            overflow.original.layers[0].pool->entries.push_back(snapshot.pool->entries.front());
        rejects([&] { sp::validateDesign(overflow); }, "Pool ceiling and duplicate identities are guarded");
        auto sourceChanged = loaded;
        sourceChanged.original.layers[0].source.id = source;
        rejects([&] { sp::validateDesign(sourceChanged); },
                "Selected entry and displayed/played source cannot disagree");
        loaded.variationSettings.count = 5;
        loaded.variationSettings.enabled = sp::varySource;
        auto variations = sp::generateVariations(loaded, 42);
        for (auto &v : variations)
            check(v.mix.layers[0].pool->policy == sp::poolChoicePolicy,
                  "Variations persist resolved source choice policy");
        loaded.original.layers[0].pool->choiceLocked = true;
        loaded.variationSettings.enabled |= sp::varyGain;
        variations = sp::generateVariations(loaded, 42);
        bool changedGain = false;
        for (auto &v : variations)
        {
            check(v.mix.layers[0].pool->selected == entry, "Source choice lock is independent");
            changedGain |= v.mix.layers[0].gainDb != l.gainDb;
        }
        check(changedGain, "Source choice lock does not lock gain");
    }
}
void session(const juce::File &root, const sp::Asset &a, const sp::Asset &b, const sp::Asset &c)
{
    Harness h;
    sp::DesignSession s(root.getChildFile("catalog/library.sqlite"), h.services());
    s.document = document(a);
    auto layer = s.document.selectedLayer;
    s.addPoolSource(layer, b, 0, b.samples, true);
    s.addPoolSource(layer, c, 0, c.samples, true);
    h.drain();
    auto &p = *s.document.activeMix().layers[0].pool;
    check(p.entries.size() == 3 && !s.working,
          "Separate accepted alternate imports each complete exactly once");
    auto first = p.entries[0].id, second = p.entries[1].id, third = p.entries[2].id;
    auto approve = [&](const juce::String &id)
    {
        auto e = *std::find_if(s.document.activeMix().layers[0].pool->entries.begin(),
                               s.document.activeMix().layers[0].pool->entries.end(),
                               [&](auto &e) { return e.id == id; });
        auto l = sp::makeDesignLayer(e.source, e.region.startSample, e.region.endSample, true);
        sp::initialiseRouting(l, s.document.activeMix().layout);
        s.reviewAndSelectPoolEntry(layer, id, l.routing, 0);
        h.drain();
    };
    s.selectPoolEntry(layer, second);
    h.drain();
    check(s.document.activeMix().layers[0].pool->selected == first,
          "Unreviewed alternate activation is atomic");
    approve(second);
    approve(third);
    s.selectPoolEntry(layer, first);
    h.drain();
    s.selectPoolEntry(layer, second);
    s.selectPoolEntry(layer, first);
    s.selectPoolEntry(layer, third);
    h.io();
    h.io();
    h.io();
    auto latest = std::move(h.posts.back());
    h.posts.pop_back();
    latest();
    while (!h.posts.empty())
        h.ui();
    check(s.document.activeMix().layers[0].pool->selected == third && !s.working,
          "A→B→C completion cannot restore an obsolete source or strand working state");
    s.undo();
    check(s.document.activeMix().layers[0].pool->selected == first, "Undo restores selected source identity");
    s.redo();
    check(s.document.activeMix().layers[0].pool->selected == third, "Redo restores selected source identity");
    s.selectPoolEntry(layer, second);
    h.io();
    s.undo();
    h.ui();
    check(s.document.activeMix().layers[0].pool->selected == first, "Undo invalidates pending activation");
    s.redo();
    s.play();
    h.drain();
    check(h.plays == 1 && s.ownsPlayback(), "Controlled Designer audition begins");
    auto stopped = h.stops;
    s.selectPoolEntry(layer, second);
    h.drain();
    check(h.stops == stopped + 1 && !s.ownsPlayback(),
          "Valid source change stops owned Live audition and never hot-swaps");
    auto &now = *s.document.activeMix().layers[0].pool;
    auto previous = now.selected;
    auto bad = now.entries[2];
    auto path = juce::File(bad.source.path);
    auto moved = path.getSiblingFile("temporarily-offline.wav");
    check(path.moveFileTo(moved), "Simulate absent selected media on owned fixture");
    s.selectPoolEntry(layer, third);
    h.drain();
    check(s.document.activeMix().layers[0].pool->selected == previous && h.stops == stopped + 1,
          "Missing chosen alternate does not reroll or steal audition");
    check(moved.moveFileTo(path), "Reconnect owned fixture");
    auto restored = json(sp::designToJson(s.document));
    for (auto state : {sp::DesignSession::TakeState::arming, sp::DesignSession::TakeState::recording,
                       sp::DesignSession::TakeState::finalizing})
    {
        s.takeState = state;
        s.selectPoolEntry(layer, first);
        s.removePoolEntry(layer, second, first);
        s.lockPoolChoice(layer, true);
        check(json(sp::designToJson(s.document)) == restored,
              "Pool/source mutations are blocked for every protected capture phase");
    }
    s.takeState = sp::DesignSession::TakeState::idle;
    s.removePoolEntry(layer, previous);
    check(s.document.activeMix().layers[0].pool->selected == previous,
          "Removing active entry requires an explicit replacement");
    s.removePoolEntry(layer, previous, first);
    h.drain();
    check(s.document.activeMix().layers[0].pool->selected == first &&
              s.document.activeMix().layers[0].pool->entries.size() == 2,
          "Removal and explicit replacement form one operation");
    s.undo();
    check(s.document.activeMix().layers[0].pool->entries.size() == 3 &&
              s.document.activeMix().layers[0].pool->selected == previous,
          "Undo restores both removed entry and active source");
    auto preferencesFile = root.getChildFile("interface.settings");
    juce::PropertiesFile preferences(preferencesFile, sp::ui::preferenceOptions());
    auto previews = std::make_shared<sp::WaveformPool>(root.getChildFile("peaks"));
    previews->demand(6, {b});
    previews->demand(7, {c});
    auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000;
    while (juce::Time::getMillisecondCounterHiRes() < deadline && (!previews->get(b) || !previews->get(c)))
    {
        previews->tick();
        juce::Thread::sleep(2);
    }
    check(previews->get(b) && previews->get(c),
          "Both Alternatives surfaces admit real background previews through shared scheduler");
    previews->demand(6, {});
    previews->demand(7, {});
    sp::SourcePoolView view(s, previews, preferences);
    view.setSize(306, 274);
    view.expandForPopup();
    sp::SourcePoolViewTestAccess::previous(view);
    h.drain();
    view.bind();
    check(s.document.activeMix().layers[0].pool->selected == first,
          "Actual Previous view callback selects stable entry");
    sp::SourcePoolViewTestAccess::next(view);
    h.drain();
    view.bind();
    check(s.document.activeMix().layers[0].pool->selected == second,
          "Actual Next view callback selects stable entry");
    sp::SourcePoolViewTestAccess::focus(view, 2);
    sp::SourcePoolViewTestAccess::enter(view, 2);
    h.drain();
    view.bind();
    check(s.document.activeMix().layers[0].pool->selected == third,
          "Actual Return command commits focused alternative");
    sp::SourcePoolViewTestAccess::lock(view);
    view.bind();
    check(s.document.activeMix().layers[0].pool->choiceLocked, "Source lock view command follows model");
    sp::SourcePoolViewTestAccess::lock(view);
    view.bind();
    check(!s.document.activeMix().layers[0].pool->choiceLocked,
          "Source lock view command turns off without sticky state");
    auto payload = document(a);
    payload.original.layers[0].region.startSample = 120;
    payload.original.layers[0].sync = 120;
    payload.original.layers[0].region.endSample = 2400;
    auto description = json(sp::designToJson(payload));
    juce::DragAndDropTarget::SourceDetails drag(description, &view, {});
    check(view.isInterestedInDragSource(drag), "Pool accepts captured Library region payload");
    view.itemDropped(drag);
    h.drain();
    view.bind();
    check(s.document.activeMix().layers[0].pool->entries.back().region.startSample == 120,
          "Actual Library drop retains source-frame region");
    s.undo();
    view.bind();
    auto snapshot = s.document;
    s.selectPoolEntry(layer, first);
    h.io();
    s.document = document(b);
    h.ui();
    check(s.document.original.layers[0].source.id == b.id, "Document switch rejects stale pool completion");
    s.document = snapshot;
    sp::SourcePoolViewTestAccess::enter(view, 1);
    h.drain();
    view.bind();
    s.reorderPoolEntry(layer, first, 1);
    check(s.document.activeMix().layers[0].pool->selected == previous,
          "Row reordering cannot retarget source identity");
    auto relocated = root.getChildFile("relocated/copy.wav");
    relocated.getParentDirectory().createDirectory();
    auto e = *std::find_if(s.document.activeMix().layers[0].pool->entries.begin(),
                           s.document.activeMix().layers[0].pool->entries.end(),
                           [&](auto &e) { return e.id == first; });
    check(juce::File(e.source.path).copyFileTo(relocated),
          "Copy identical content to a distinct inode/location");
    s.relinkPoolEntry(layer, first, relocated);
    h.drain();
    auto &entry = *std::find_if(s.document.activeMix().layers[0].pool->entries.begin(),
                                s.document.activeMix().layers[0].pool->entries.end(),
                                [&](auto &e) { return e.id == first; });
    check(entry.source.id == a.id && entry.fingerprint == e.fingerprint &&
              entry.source.path == relocated.getFullPathName(),
          "Content-verified relocation preserves asset/entry identity");
    s.relinkPoolEntry(layer, first, juce::File(b.path));
    h.drain();
    check(entry.source.path == relocated.getFullPathName(),
          "Different content cannot impersonate a relocated entry");
}
juce::AudioBuffer<float> filePcm(const juce::File &file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(file));
    check(r != nullptr, "Finalized output can be opened");
    juce::AudioBuffer<float> b((int)r->numChannels, (int)r->lengthInSamples);
    std::vector<float *> channels;
    for (int c = 0; c < b.getNumChannels(); ++c)
        channels.push_back(b.getWritePointer(c));
    check(r->read(channels.data(), b.getNumChannels(), 0, b.getNumSamples()),
          "Read every physical output channel");
    return b;
}
juce::AudioBuffer<float> replayPcm(const sp::DesignDocument &d)
{
    sp::DesignReader r(d, d.sampleRate, 0, -1, 0);
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
void multichannel(const juce::File &root, const sp::Asset &a)
{
    const auto bus = sp::channelLayout("5.1-side-v1");
    for (auto name : {"mono-v1", "stereo-v1", "5.1-side-v1", "5.1-rear-v1", "quad-v1", "discrete-4-v1"})
    {
        auto input =
            juce::String(name).startsWith("discrete") ? sp::discreteLayout(4) : sp::channelLayout(name);
        auto source =
            fixture(root.getChildFile(juce::String(name) + ".wav"), input.size(), 48000, 24000, input);
        auto d = document(a);
        sp::setMixLayout(d.original, bus);
        makePool(d, {});
        auto &l = d.original.layers[0];
        auto e = sp::inspectPoolEntry(source, 100, 20100, true);
        sp::LayerRouting routing;
        routing.interpretation = input;
        for (int c = 0; c < input.size(); ++c)
        {
            routing.inputs.push_back(c);
            routing.routes.push_back({c, c, 0, false, false});
        }
        sp::reviewPoolEntry(e, routing, bus);
        l.pool->entries.push_back(e);
        sp::activatePoolEntry(l, e.id, bus);
        sp::syncPoolSelection(d.original);
        sp::validateDesign(d);
        auto pcm = read(d);
        for (int c = 0; c < 6; ++c)
            check(std::abs(pcm.getSample(c, 100) - (c < input.size() ? .01f * (c + 1) : 0.f)) < 1e-6,
                  "Selected alternate maps independent channel tags in exact physical order, with no "
                  "implicit LFE");
        auto out = sp::renderDesign(
            d, root.getChildFile("renders"), d.renderSettings, [] { return false; }, [](double) {});
        auto disk = filePcm(juce::File(out.path));
        check(disk.getNumChannels() == 6 && std::abs(disk.getSample(0, 100) - .01f) < 1e-6,
              "Selected multichannel alternate reaches finalized interleaved output");
    }
    auto d = document(a);
    sp::setMixLayout(d.original, bus);
    makePool(d, {});
    auto &l = d.original.layers[0];
    auto unknown = fixture(root.getChildFile("unknown6.wav"), 6, 48000, 24000, sp::discreteLayout(6, true));
    auto e = sp::inspectPoolEntry(unknown, 0, unknown.samples, true);
    auto temporary = sp::makeDesignLayer(unknown, 0, -1, true);
    sp::initialiseRouting(temporary, bus);
    rejects([&] { sp::reviewPoolEntry(e, temporary.routing, bus); },
            "Six untagged channels do not imply surround");
    temporary.routing.interpretation = sp::channelLayout("5.1-side-v1");
    temporary.routing.inputs = {0, 1, 2, 3, 4, 5};
    temporary.routing.needsSelection = false;
    temporary.routing.routes.clear();
    for (int c = 0; c < 6; ++c)
        temporary.routing.routes.push_back({c, c, 0, false, false});
    sp::reviewPoolEntry(e, temporary.routing, bus);
    l.pool->entries.push_back(e);
    sp::activatePoolEntry(l, e.id, bus);
    check(read(d).getSample(3, 100) > .039f, "Explicit unknown-source roles and LFE route become audible");
    for (int width : {63, 64})
    {
        auto wide = fixture(root.getChildFile("wide-" + juce::String(width) + ".wav"), width, 48000, 24000);
        auto entry = sp::inspectPoolEntry(wide, 0, wide.samples, true);
        auto candidate = sp::makeDesignLayer(wide, 0, -1, true);
        sp::initialiseRouting(candidate, bus);
        rejects([&] { sp::reviewPoolEntry(entry, candidate.routing, bus); },
                "Wide physical input requires deliberate selection");
        sp::LayerRouting subset;
        subset.inputs = {width - 1};
        subset.interpretation = sp::channelLayout("mono-v1");
        subset.routes = {{0, 2, 0, false, false}};
        sp::reviewPoolEntry(entry, subset, bus);
        l.pool->entries.push_back(entry);
        sp::activatePoolEntry(l, entry.id, bus);
        auto selected = read(d);
        check(std::abs(selected.getSample(2, 100) - width * .01f) < 1e-6 && selected.getSample(0, 100) == 0,
              "Explicit wide final-channel subset is not truncated");
        auto mean = entry;
        mean.id = juce::Uuid().toString();
        subset.legacyAnalysisMean = true;
        subset.inputs.clear();
        for (int c = 0; c < width; ++c)
            subset.inputs.push_back(c);
        sp::reviewPoolEntry(mean, subset, bus);
        l.pool->entries.push_back(mean);
        sp::activatePoolEntry(l, mean.id, bus);
        check(std::abs(read(d).getSample(2, 100) - (width + 1) * .005f) < 1e-6,
              "v2 mean averages all 63/64 actual physical channels");
    }
    // Freeze one exact selected source; the mutable alternatives are absent from the recipe.
    sp::activatePoolEntry(l, e.id, bus);
    sp::syncPoolSelection(d.original);
    auto draft = sp::freezeTake(d, "Pool chosen capture");
    draft.generation = 7;
    sp::hashTake(draft);
    check(!draft.recipe->layers[0].pool && draft.poolOrigins.isNotEmpty(),
          "Take freezes exact entry provenance, never mutable pool");
    auto cap = std::make_shared<sp::TakeCapture>(draft);
    auto live = d;
    live.takeProcessing = true;
    sp::PcmStream stream(live, 48000, 0);
    stream.startTake(cap);
    std::array<std::atomic<int>, 16> identity;
    for (int c = 0; c < 16; ++c)
        identity[c] = c;
    juce::AudioBuffer<float> consumed(6, 8192), block(6, 512);
    float gain = 1;
    for (int n = 0; n < 16; ++n)
    {
        stream.fill(512);
        check(stream.read(block.getArrayOfWritePointers(), 6, 512, gain, 1, &identity) == 512,
              "Pool take callback consumes all six channels");
        for (int c = 0; c < 6; ++c)
            consumed.copyFrom(c, n * 512, block, c, 0, 512);
    }
    cap->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
    auto take = cap->snapshot();
    check(take.complete, "Pooled take finalized from consumed frames");
    d.takes = {take};
    d.activeTake = take.id;
    auto replay = replayPcm(d);
    auto error = difference(consumed, replay);
    check(error < 1e-6, "Pooled consumed capture equals immutable replay on six channels");
    auto output = sp::renderDesign(
        d, root.getChildFile("take-outputs"), d.renderSettings, [] { return false; }, [](double) {});
    check(difference(filePcm(juce::File(output.path)), replay) < 1e-6,
          "Pooled frozen offline render equals consumed capture");
    check(output.sourceProvenance[0]["poolOrigins"].isArray(),
          "Finalized receipt carries versioned selected pool origin");
    auto recipe = take.recipeHash, events = take.eventHash, origins = take.poolOrigins;
    l.pool.reset();
    l.gainDb = -44;
    sp::setMixLayout(d.original, sp::channelLayout("stereo-v1"));
    sp::validateDesign(d);
    check(d.takes[0].recipeHash == recipe && d.takes[0].eventHash == events &&
              d.takes[0].poolOrigins == origins && difference(replay, replayPcm(d)) == 0,
          "Deleting pools and editing Live bus cannot alter take or PCM");
    auto splitSettings = d.renderSettings;
    splitSettings.splitMono = true;
    auto split = sp::renderDesign(
        d, root.getChildFile("take-outputs"), splitSettings, [] { return false; }, [](double) {});
    check(split.files.size() == 6, "Selected pooled take split export retains all six members");
    for (int c = 0; c < 6; ++c)
    {
        auto member = split.files[c];
        auto mono = filePcm(juce::File(member["path"].toString()));
        check(member["role"].toString() == bus.roles[c] && member["sha256"].toString().length() == 64,
              "Split receipt has ordered roles and verified hashes");
        for (int n = 0; n < mono.getNumSamples(); ++n)
            check(std::abs(mono.getSample(0, n) - replay.getSample(c, n)) < 1e-6,
                  "Split PCM follows immutable selected source");
    }
    auto stereoSettings = d.renderSettings;
    stereoSettings.layout = "stereo-v1";
    auto stereo = sp::renderDesign(
        d, root.getChildFile("take-outputs"), stereoSettings, [] { return false; }, [](double) {});
    check(filePcm(juce::File(stereo.path)).getNumChannels() == 2,
          "Pooled take explicit downmix is a distinct finalized output");
    d.outputs = {output, split, stereo};
    Harness h;
    sp::DesignSession s(root.getChildFile("output-catalog/library.sqlite"), h.services());
    s.document = d;
    s.selectTake(take.id);
    s.chooseOutput(split.path);
    check(s.readyFiles().size() == 6, "Selected pooled split set is ready");
    auto member = juce::File(split.files[0]["path"].toString()),
         held = member.getSiblingFile("held-stem.wav");
    check(member.moveFileTo(held), "Remove owned selected stem temporarily");
    check(s.readyFiles().isEmpty(), "Absent selected set never falls back to another receipt");
    check(held.moveFileTo(member), "Restore durable output");
    auto saved = root.getChildFile("immutable.soundplus");
    sp::saveDesignAtomic(saved, s.document);
    auto previous = saved.loadFileAsString();
    auto invalid = s.document;
    invalid.id.clear();
    rejects([&] { sp::saveDesignAtomic(saved, invalid); }, "Invalid save rejected");
    check(saved.loadFileAsString() == previous, "Failed save preserves previous document exactly");
    auto loaded = sp::loadDesign(saved);
    check(loaded.takes[0].poolOrigins == origins && loaded.takes[0].recipeHash == recipe,
          "Pool origin and old hash dispatch survive reopen");
    auto changed = take;
    auto origin = juce::JSON::parse(changed.poolOrigins);
    origin["entries"][0]["entry"]["source"].getDynamicObject()->setProperty("channels", 2);
    changed.poolOrigins = json(origin);
    rejects([&] { sp::validateTake(changed); }, "Tampered frozen pool source rejected");
    int reads = 0;
    auto candidate = sp::inspectPoolEntry(a, 0, a.samples, true);
    rejects([&] { sp::verifyPoolEntry(candidate, false, [&] { return ++reads >= 3; }); },
            "Obsolete hashing cancels during stream reads");
    check(reads >= 3, "Cancellation observed after admission");
    std::cout << "Pool six-channel capture/replay/offline maximum error " << error << "\n";
}

} // namespace
void runSourcePoolTests(const juce::File &parent, const juce::File &)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto a = fixture(root.getChildFile("a.wav")), b = fixture(root.getChildFile("b.wav"), 2, 44100, 44100),
         c = fixture(root.getChildFile("c.wav"), 1, 48000, 24000);
    recordedPoolService(root.getChildFile("pool-transport"), a, b);
    liveSourceOracle(root.getChildFile("live-performance"));
    determinism();
    model(root, a, b);
    multichannel(root.getChildFile("multichannel"), a);
    session(root.getChildFile("session"), a, b, c);
    std::cout << "Source pool model/session scenarios passed (" << checks << " assertions)\n";
}
void prepareSourcePoolUI(const juce::File &root)
{
    root.createDirectory();
    auto media = root.getChildFile("media");
    auto a =
        fixture(media.getChildFile("Pool Mono.wav"), 1, 48000, 240000, sp::channelLayout("mono-v1"), true);
    auto b = fixture(media.getChildFile("Pool Stereo.wav"), 2, 44100, 220500, sp::channelLayout("stereo-v1"),
                     true);
    auto side = fixture(media.getChildFile("Pool Side.wav"), 6, 48000, 240000,
                        sp::channelLayout("5.1-side-v1"), true);
    auto rear = fixture(media.getChildFile("Pool Rear.wav"), 6, 48000, 240000,
                        sp::channelLayout("5.1-rear-v1"), true);
    auto unknown = fixture(media.getChildFile("Pool Unknown six.wav"), 6, 48000, 240000,
                           sp::discreteLayout(6, true), true);
    auto wide = fixture(media.getChildFile("Pool Wide 63.wav"), 63, 48000, 48000, {}, true);
    auto d = document(a);
    sp::setMixLayout(d.original, sp::channelLayout("5.1-side-v1"));
    d.name = "Source Pool QA";
    d.outputFolder = root.getChildFile("renders").getFullPathName();
    d.renderSettings.layout = "same";
    d.renderSettings.channels = 6;
    makePool(d, {b});
    auto &l = d.original.layers[0];
    for (auto &source : {side, rear, unknown, wide})
        l.pool->entries.push_back(sp::inspectPoolEntry(source, 0, source.samples, true));
    d.original.performance.blendLaw = 2;
    d.original.performance.assignments[0].layer = l.id;
    auto saved = root.getChildFile("Pool QA.soundplus");
    sp::saveDesignAtomic(saved, d);
    auto catalog = root.getChildFile("catalog");
    catalog.createDirectory();
    {
        sp::Catalog initialise(catalog.getChildFile("library.sqlite"));
    }
    sp::DesignRecoveryStore recovery(catalog.getChildFile("designer-autosave"));
    recovery.retain(d, saved, false);
    recovery.activate(d.id);
    juce::PropertiesFile prefs(catalog.getChildFile("interface.settings"), sp::ui::preferenceOptions());
    prefs.setValue("designerActive", true);
    prefs.setValue("performWorkspace", false);
    prefs.setValue("browserOpen", true);
    prefs.setValue("browserWidth", 300);
    prefs.setValue("inspectorOpen", true);
    prefs.setValue("alternativesOpen", true);
    prefs.setValue("monitorVolume", .08);
    prefs.saveIfNeeded();
    std::cout << saved.getFullPathName() << "\n";
}
