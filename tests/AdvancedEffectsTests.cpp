#include "AdvancedEffects.h"
#include "NativeEffectsDSP.h"
#include "TrackModel.h"
#include "DesignLive.h"
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
void contracts(const juce::File &base)
{
    using namespace sp;
    for (auto type : {"advanced-delay", "algorithmic-reverb", "convolution-reverb", "granular-texture",
                      "spectral-freeze", "spectral-blur"})
    {
        auto e = makeStockEffect(type);
        auto original = nativeEffectJson(e);
        auto copy = e;
        nativeEffectFromJson(copy, original);
        check(juce::JSON::toString(original, true) == juce::JSON::toString(nativeEffectJson(copy), true),
              "Advanced exact keyed state round trip");
        auto future = original.clone();
        future.getDynamicObject()->setProperty("policy", "future");
        rejects([&] { nativeEffectFromJson(copy, future); }, "Future advanced policy rejects");
        check(juce::JSON::toString(original, true) == juce::JSON::toString(nativeEffectJson(copy), true),
              "Failed parse retains state");
        DesignDocument d;
        d.original.layout = channelLayout("stereo-v1");
        auto track = addTrack(d.original, TrackKind::audio);
        auto &chain = findTrack(d.original, track)->inserts;
        chain.stock = {e, e};
        chain.stock[1].id = juce::Uuid().toString();
        normaliseInsertChain(chain);
        d.original.midiTiming = {87.5, 7, 8};
        auto json = designToJson(d);
        check((int)json["version"] == 16, "Advanced document requires manifest16");
        auto decoded = designFromJson(json);
        check(decoded.original.midiTiming.bpm == 87.5 && decoded.original.midiTiming.numerator == 7,
              "Audio-only project effects clock persists");
        auto bad = json.clone();
        bad.getDynamicObject()->setProperty("version", 15);
        bad.getDynamicObject()->setProperty("dsp", "native-effects-v1");
        rejects([&] { designFromJson(bad); }, "Cannot label advanced state as manifest15");
        auto targets = automationTargets(d.original, track);
        for (auto &p : nativeDefinition(e).parameters)
        {
            auto at = std::find_if(targets.begin(), targets.end(),
                                   [&](auto &t) { return t.target == AutomationTarget{e.id, p.id}; });
            check(at != targets.end() && at->writable == !p.structural,
                  "One lane has every stable advanced target with explicit structural status");
        }
        auto key = designStructure(d);
        auto values = designParameters(d);
        auto changed = d;
        applyDesignParameters(changed, values);
        check(juce::JSON::toString(designParameters(changed), true) == juce::JSON::toString(values, true),
              "Advanced live controls preserve values");
        d.original.midiTiming.bpm = 90;
        check(designStructure(d) != key, "Constant tempo change safely reprepares graph");
        if (juce::String(type) == "convolution-reverb")
            rejects([&] { freezeTake(d, "Unresolved IR"); },
                    "Unresolved IR cannot be frozen as playable content");
        else
        {
            auto take = freezeTake(d, "Advanced frozen");
            auto t = takeToJson(take);
            check((int)t["recipe"]["recipeVersion"] == 7, "Advanced recipe7 boundary");
            check(takeFromJson(t).recipeHash == take.recipeHash, "Recipe7 hash survives reopen");
        }
        auto file = base.getChildFile(juce::String(type) + ".soundplus");
        saveDesignAtomic(file, d);
        check(loadDesign(file).original.midiTiming.bpm == 90, "Advanced session save/reopen");
        std::reverse(chain.order.begin(), chain.order.end());
        check(automationTargets(d.original, track).size() == targets.size(),
              "Reorder retains instance-addressed targets");
    }
    for (int i = 0; i < 15; ++i)
        check(delayBeats(i) > 0, "All straight/dotted/triplet divisions resolve");
    check(delayBeats(2) == 1 && delayBeats(7) == 1.5 && delayBeats(12) == 2. / 3.,
          "Independent quarter note subdivision values");
    rejects([] { delayBeats(15); }, "Invalid subdivision fails");
    auto e = makeStockEffect("advanced-delay");
    nativeValue(e, "feedback", .96);
    rejects([&] { validateNativeEffect(e); }, "Feedback stability cap enforced");
    auto ir = makeStockEffect("convolution-reverb");
    ImpulseReference ref;
    ref.source.id = "ir-test";
    ref.source.path = "/not/a/real/IR.wav";
    ref.source.sampleRate = 48000;
    ref.source.samples = 48000;
    ref.source.channels = 1;
    ref.source.size = 192044;
    ref.sha256 = juce::String::repeatedString("a", 64);
    ref.mapping = "mono-diagonal";
    ref.routes = {{"L", "L", 0}, {"R", "R", 0}};
    ir.impulse = ref;
    auto json = nativeEffectJson(ir);
    auto copy = ir;
    nativeEffectFromJson(copy, json);
    check(copy.impulse->sha256 == ref.sha256 && copy.impulse->routes == ref.routes,
          "IR metadata and explicit map persist without file substitution");
    auto oversized = json.clone();
    oversized["impulse"].getDynamicObject()->setProperty("channels", (juce::int64)4294967297LL);
    rejects([&] { nativeEffectFromJson(copy, oversized); },
            "Huge IR channel count cannot wrap to a valid integer");
    copy.impulse->routes.push_back({"L", "L", 0});
    rejects([&] { validateNativeEffect(copy); }, "Ambiguous IR map rejected");
    DesignDocument heavy;
    heavy.original.layout = discreteLayout(16);
    for (int i = 0; i < 8; ++i)
    {
        auto track = addTrack(heavy.original, TrackKind::audio);
        auto e = makeStockEffect("advanced-delay");
        nativeValue(e, "max-ms", 16000);
        findTrack(heavy.original, track)->inserts.stock = {e};
    }
    rejects([&] { validateDesign(heavy); }, "Advanced aggregate memory bounded before DSP preparation");
}
} // namespace

#include "AdvancedDelayTests.inc"
#include "CreativeEffectsTests.inc"
#include "AdvancedIntegrationTests.inc"
#include "AdvancedReverbTests.inc"
#include "AdvancedConvolutionTests.inc"
#include "AdvancedWorkflowTests.inc"
void runAdvancedEffectsTests(const juce::File &base)
{
    base.createDirectory();
    contracts(base);
    creativeTests();
    if (sp::nativeDSPAvailable("advanced-delay"))
    {
        delayTests(base);
        delayHistoryTests();
        delayGraphTests(base);
    }
    if (sp::nativeDSPAvailable("algorithmic-reverb"))
    {
        reverbTests(base);
        reverbGraphTests(base);
    }
    convolutionTests(base);
    convolutionGraphTests(base);
    advancedEndpointTests();
    advancedCaptureTests(base);
    advancedWorkflowTests(base);
    advancedExportTests(base);
    std::cout << "Advanced effects: " << checks << " assertions passed\n";
}
