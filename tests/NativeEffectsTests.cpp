#include "NativeEffects.h"
#include "NativeEffectsDSP.h"
#include "TrackModel.h"
#include "DesignLive.h"
#include <iostream>
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include "StockRackView.h"
#include "AudioStream.h"
#include <deque>
#include <juce_gui_basics/juce_gui_basics.h>
namespace
{
void check(bool ok, const char *why)
{
    if (!ok)
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
} // namespace
#include "NativeEqTests.inc"
#include "NativeDynamicsTests.inc"
#include "NativeIntegrationTests.inc"
void runNativeEffectsTests(const juce::File &base)
{
    using namespace sp;
    juce::ScopedJuceInitialiser_GUI gui;
    base.createDirectory();
    for (auto &def : nativeEffectDefinitions())
    {
        auto e = makeStockEffect(def.type);
        check(e.nativeVersion == 1, "Native epoch");
        validateNativeEffect(e);
        auto json = nativeEffectJson(e);
        auto changed = e;
        changed.nativeValues[0] = def.parameters[0].minimum;
        nativeEffectFromJson(changed, json);
        check(changed.nativeValues == e.nativeValues, "Canonical keyed round trip");
        auto factory = nativeFactoryPresets(def.type);
        check(factory.size() > 0, "Native factory presets are editable metadata");
        for (int n = 0; n < factory.size(); ++n)
        {
            auto selected = e;
            applyNativePreset(selected, factory[n]);
            validateNativeEffect(selected);
        }
        auto preset = nativePresetJson(e, "User editable");
        auto id = changed.id;
        applyNativePreset(changed, preset);
        check(changed.id == id, "Preset never replaces instance identity");
        auto future = json.clone();
        future.getDynamicObject()->setProperty("version", 2);
        rejects([&] { nativeEffectFromJson(changed, future); }, "Future version rejected non-destructively");
        check(changed.nativeValues == e.nativeValues, "Failed parse retains old state");
        auto unknown = json.clone();
        unknown["parameters"][0].getDynamicObject()->setProperty("id", "unknown");
        rejects([&] { nativeEffectFromJson(changed, unknown); }, "Unknown ID rejected");
        DesignDocument d;
        d.original.layout = channelLayout("stereo-v1");
        auto track = addTrack(d.original, TrackKind::audio);
        findTrack(d.original, track)->inserts.stock = {e, e};
        findTrack(d.original, track)->inserts.stock[1].id = juce::Uuid().toString();
        auto targets = automationTargets(d.original, track);
        for (size_t n = 0; n < def.parameters.size(); ++n)
        {
            auto &p = def.parameters[n];
            auto at = std::find_if(targets.begin(), targets.end(),
                                   [&](auto &t) { return t.target == AutomationTarget{e.id, p.id}; });
            check(at != targets.end() && at->writable == !p.structural,
                  "All supported parameters have stable explicit automation targets");
        }
        auto encoded = designToJson(d);
        check((int)encoded["version"] == 15, "New manifest gate");
        auto decoded = designFromJson(encoded);
        check(juce::JSON::toString(designToJson(decoded), true) == juce::JSON::toString(encoded, true),
              "Native document exact canonical round trip");
        auto take = freezeTake(d, "Native frozen");
        auto t = takeToJson(take);
        check((int)t["recipe"]["recipeVersion"] == 6, "New take epoch");
        auto reopened = takeFromJson(t);
        check(reopened.recipeHash == take.recipeHash, "New recipe preserves hash on reopen");
        auto live = designParameters(d);
        auto key = designStructure(d);
        auto &insert = findTrack(d.original, track)->inserts.stock[0];
        auto p = std::find_if(def.parameters.begin(), def.parameters.end(),
                              [](auto &p) { return !p.structural && !p.discrete; });
        auto ix = (int)(p - def.parameters.begin());
        insert.nativeValues[(size_t)ix] = p->minimum;
        check(designStructure(d) == key, "Ordinary native edit does not starve graph adoption");
        (void)live;
    }
    for (auto type : {"gain", "filter", "delay", "reverb"})
    {
        DesignDocument d;
        auto e = makeStockEffect(type);
        d.original.inserts.stock = {e};
        auto json = designToJson(d);
        check(!json["original"]["insertChain"]["slots"][0]["settings"].hasProperty("native"),
              "Legacy encoding untouched");
        check((int)json["version"] < 15, "Legacy never acquires new epoch");
    }
    if (nativeDSPAvailable("parametric-eq"))
        eqTests();
    if (nativeDSPAvailable("compressor"))
    {
        dynamicsTests(base);
        nativeIntegration(base);
        sp::NativeEffectTestAccess::view(base);
    }
    std::cout << "Native effect foundation checks passed\n";
}
void prepareNativeEffectsUI(const juce::File &dir)
{
    using namespace sp;
    dir.createDirectory();
    auto file = dir.getChildFile("Native source.wav");
    if (file.existsAsFile())
        throw std::runtime_error("Use a fresh native QA fixture directory");
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), 48000, 1, 24, {}, 0));
    juce::AudioBuffer<float> b(1, 48000 * 40);
    for (int n = 0; n < b.getNumSamples(); ++n)
        b.setSample(0, n, .4f * std::sin(n * .07f) * (n % 24000 < 1000 ? 1.f : .3f));
    check(writer && writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()),
          "Native UI source generated");
    writer.reset();
    DesignDocument d;
    d.name = "Native Effects QA";
    d.original.layout = channelLayout("stereo-v1");
    d.original.masterDb = 0;
    d.original.layers = {makeDesignLayer(inspectDesignSource(file), 0, -1, true)};
    initialiseRouting(d.original.layers[0], d.original.layout);
    enableTrackSchema(d.original);
    auto &t = d.original.tracks[0];
    t.name = "Impact texture";
    for (auto &def : nativeEffectDefinitions())
        t.inserts.stock.push_back(makeStockEffect(def.type));
    normaliseInsertChain(t.inserts);
    d.selectedLayer = d.original.layers[0].id;
    d.selectedTrack = t.id;
    d.outputFolder = dir.getChildFile("Renders").getFullPathName();
    saveDesignAtomic(dir.getChildFile("Native Effects QA.soundplus"), d);
    auto plan = juce::var(new juce::DynamicObject());
    plan.getDynamicObject()->setProperty("mode", "native-effects");
    plan.getDynamicObject()->setProperty("designFile",
                                         dir.getChildFile("Native Effects QA.soundplus").getFullPathName());
    dir.getChildFile("plan.json").replaceWithText(juce::JSON::toString(plan, true));
}
