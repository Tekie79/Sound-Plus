#include "FadeTests.h"
#include "DesignAudio.h"
#include "DesignLive.h"
#include "DesignSession.h"
#include "AudioStream.h"
#include "Performance.h"
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
template <class F> void until(F condition, const char *why)
{
    auto end = sp::monotonicMs() + 10000;
    while (!condition())
    {
        if (sp::monotonicMs() > end)
            throw std::runtime_error(why);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ++checks;
}
} // namespace
void runFadeTests(const juce::File &parent, const juce::File &application)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    auto directory = parent.getChildFile(juce::Uuid().toString());
    directory.createDirectory();
    auto source = directory.getChildFile("constant-stereo.wav");
    {
        juce::WavAudioFormat format;
        auto stream = source.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer(
            format.createWriterFor(stream.release(), 48000, 2, 32, {}, 0));
        juce::AudioBuffer<float> samples(2, 96000);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 96000; ++n)
                samples.setSample(c, n, .25f);
        check(writer && writer->writeFromAudioSampleBuffer(samples, 0, 96000),
              "Bounded two-second constant fixture written");
    }
    sp::DesignDocument d;
    d.original.masterDb = 0;
    d.renderSettings.tailSeconds = 0;
    d.renderSettings.bits = 32;
    auto layer = sp::makeDesignLayer(sp::inspectDesignSource(source), 0, 24000, true);
    layer.fadeIn = 6000;
    layer.fadeOut = 9000;
    d.original.layers = {layer};
    d.selectedLayer = layer.id;
    // Independent reference values at one quarter of the normalized fade.
    constexpr double quarter[]{.25, .3826834323650898, .0625, .5, .15625};
    for (size_t in = 0; in < sp::fadeShapes.size(); ++in)
        for (size_t out = 0; out < sp::fadeShapes.size(); ++out)
        {
            d.original.layers[0].fadeInShape = sp::fadeShapes[in];
            d.original.layers[0].fadeOutShape = sp::fadeShapes[out];
            sp::DesignGraph graph(d);
            juce::AudioBuffer<float> pcm(2, 24000);
            graph.render(pcm, 0, 24000);
            check(std::abs(pcm.getSample(0, 1500) - .25 * quarter[in]) < 1e-7,
                  "Fade-in DSP follows the selected amplitude shape");
            check(std::abs(pcm.getSample(1, 21749) - .25 * quarter[out]) < 1e-7,
                  "Fade-out DSP mirrors the chosen shape on the half-open clock");
            check(pcm.getSample(0, 0) == 0 && pcm.getSample(0, 23999) == 0 && pcm.getSample(0, 10000) == .25f,
                  "Shapes retain zero endpoints and unaffected interior samples");
            auto roundtrip = sp::designFromJson(sp::designToJson(d));
            check(roundtrip.original.layers[0].fadeInShape == sp::fadeShapes[in] &&
                      roundtrip.original.layers[0].fadeOutShape == sp::fadeShapes[out],
                  "Independent shapes survive document serialization");
        }
    auto legacy = sp::designToJson(d);
    const std::vector<sp::FadePoint> custom{{.2, .1}, {.4, .8}, {.6, .2}, {.8, .9}};
    sp::PreparedFade customCurve(sp::FadeShape::custom, custom);
    for (auto &point : custom)
        check(std::abs(customCurve.value(point.x) - point.y) < 1e-12,
              "Smooth custom curve passes through every point");
    for (int i = 0; i <= 1000; ++i)
        check(std::isfinite(customCurve.value(i / 1000.)) && customCurve.value(i / 1000.) >= 0 &&
                  customCurve.value(i / 1000.) <= 1,
              "Custom rises and dips remain finite and cannot overshoot unity");
    auto customDocument = d;
    customDocument.original.layers[0].fadeInShape = sp::FadeShape::custom;
    customDocument.original.layers[0].fadeInPoints = custom;
    auto customRoundtrip = sp::designFromJson(sp::designToJson(customDocument));
    check(customRoundtrip.original.layers[0].fadeInPoints == custom, "Custom points survive document recall");
    for (auto preset : sp::fadeShapes)
    {
        customDocument.original.layers[0].fadeInShape = preset;
        check(sp::designFromJson(sp::designToJson(customDocument)).original.layers[0].fadeInPoints == custom,
              "All five presets remain available without discarding the saved custom points");
    }
    for (auto invalidPoints :
         {std::vector<sp::FadePoint>{{.4, .5}, {.2, .5}}, std::vector<sp::FadePoint>{{.2, 1.1}},
          std::vector<sp::FadePoint>{{0, .2}}, std::vector<sp::FadePoint>{{.2, .5}, {.2, .9}}})
    {
        auto invalid = d;
        invalid.original.layers[0].fadeInPoints = invalidPoints;
        bool bad = false;
        try
        {
            sp::validateDesign(invalid);
        }
        catch (const std::exception &)
        {
            bad = true;
        }
        check(bad, "Malformed custom points are rejected before graph preparation");
    }
    auto value = (*legacy["original"]["layers"].getArray())[0];
    value.getDynamicObject()->removeProperty("fadeInShape");
    value.getDynamicObject()->removeProperty("fadeOutShape");
    legacy.getDynamicObject()->setProperty("dsp", "stock-sinc32-v1");
    auto linear = sp::designFromJson(legacy);
    check(linear.original.layers[0].fadeInShape == sp::FadeShape::linear &&
              linear.original.layers[0].fadeOutShape == sp::FadeShape::linear,
          "Legacy version-one documents retain the original linear fade law");
    value.getDynamicObject()->setProperty("fadeInShape", "unknown");
    bool rejected = false;
    try
    {
        sp::designFromJson(legacy);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Unsupported curve metadata is rejected");
    auto mixed = linear;
    mixed.variations = sp::generateVariations(linear, 71);
    mixed.variations[0].mix.layers[0].fadeOutShape = sp::FadeShape::equalPower;
    check(sp::designToJson(mixed)["dsp"].toString() == "stock-sinc32-v2",
          "Nonlinear variations also mark the saved DSP capability");
    auto mislabeled = sp::designToJson(d);
    mislabeled.getDynamicObject()->setProperty("dsp", "stock-sinc32-v1");
    rejected = false;
    try
    {
        sp::designFromJson(mislabeled);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Nonlinear recipes cannot silently claim legacy linear-only DSP");
    // Zero duration is bypass, even with a non-linear shape; overlapping fades multiply.
    auto overlap = d;
    auto &over = overlap.original.layers[0];
    over.fadeIn = over.fadeOut = 24000;
    over.fadeInShape = sp::FadeShape::slow;
    over.fadeOutShape = sp::FadeShape::fast;
    sp::DesignGraph overlapping(overlap);
    juce::AudioBuffer<float> result(2, 24000);
    overlapping.render(result, 0, 24000);
    check(std::abs(result.getSample(0, 12000) - .25 * .25 * std::sqrt(11999. / 24000)) < 1e-7,
          "Different overlapping shapes multiply without changing half-open endpoints");
    over.fadeIn = over.fadeOut = 0;
    sp::DesignGraph zero(overlap);
    zero.render(result, 0, 24000);
    check(result.getSample(0, 0) == .25f && result.getSample(0, 23999) == .25f,
          "Zero-duration shaped fades bypass their envelope");
    // A live shape change preserves the graph and reaches the new curve after its 5ms crossfade.
    auto live = linear;
    live.original.layers[0].fadeIn = 24000;
    live.original.layers[0].fadeOut = 0;
    auto structure = sp::designStructure(live);
    sp::DesignGraph running(live);
    running.seek(4800);
    live.original.layers[0].fadeInShape = sp::FadeShape::custom;
    live.original.layers[0].fadeInPoints = custom;
    check(structure == sp::designStructure(live), "Shape changes use the live parameter path");
    running.updateParameters(sp::designParameters(live));
    juce::AudioBuffer<float> updated(2, 960);
    running.render(updated, 0, 960);
    check(std::abs(updated.getSample(0, 480) - .25 * customCurve.value(5280. / 24000)) < 1e-7,
          "Prepared live voice adopts the selected curve without a replacement");
    {
        sp::DesignSessionServices services;
        services.automaticRecovery = services.timers = false;
        services.status = [] { return sp::message("status"); };
        services.stop = [] {};
        sp::DesignSession session(directory.getChildFile("session/library.sqlite"), services);
        session.document = linear;
        session.selectLayer(layer.id);
        session.edit("Shape", [](auto &doc) { doc.original.layers[0].fadeInShape = sp::FadeShape::sCurve; });
        session.undo();
        check(session.document.original.layers[0].fadeInShape == sp::FadeShape::linear,
              "Shape edit supports Undo");
        session.redo();
        check(session.document.original.layers[0].fadeInShape == sp::FadeShape::sCurve,
              "Shape edit supports Redo");
    }
    // Saved shape, exported float WAV, prepared PCM and native helper all share the same law.
    d.original.layers[0].region.endSample = 96000;
    d.original.layers[0].fadeIn = 24000;
    d.original.layers[0].fadeOut = 36000;
    d.original.layers[0].fadeInShape = sp::FadeShape::custom;
    d.original.layers[0].fadeInPoints = custom;
    d.original.layers[0].fadeOutShape = sp::FadeShape::equalPower;
    auto file = directory.getChildFile("shaped.soundplus");
    sp::saveDesignAtomic(file, d);
    auto recalled = sp::loadDesign(file);
    check(recalled.original.layers[0].fadeInShape == sp::FadeShape::custom &&
              recalled.original.layers[0].fadeInPoints == custom &&
              recalled.original.layers[0].fadeOutShape == sp::FadeShape::equalPower,
          "Saved shaped design recalls independent IN and OUT choices");
    sp::DesignReader reference(recalled, 48000);
    juce::AudioBuffer<float> expected(2, (int)reference.length());
    reference.read(expected, expected.getNumSamples());
    auto output = sp::renderDesign(
        recalled, directory.getChildFile("render"), recalled.renderSettings, [] { return false; },
        [](double) {});
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(output.path)));
    juce::AudioBuffer<float> rendered(2, 96000);
    check(reader && reader->read(&rendered, 0, 96000, 0, true, true), "Finalized shaped WAV reads back");
    double renderError = 0;
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 96000; ++n)
            renderError =
                std::max(renderError, (double)std::abs(rendered.getSample(c, n) - expected.getSample(c, n)));
    check(renderError < 1e-7, "Rendered WAV matches the exact shaped reference");
    sp::AudioClient audio(application);
    until([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native device ready");
    audio.gain(.02f);
    auto capture = directory.getChildFile("shaped-native.f32");
    auto generation = audio.playDesign("design:" + d.id, file, 0, capture);
    until(
        [&]
        {
            auto s = audio.status();
            return (uint64_t)(int64_t)s["playGeneration"] == generation && (bool)s["playing"];
        },
        "Native shaped audition begins");
    auto baseline = audio.status();
    until(
        [&]
        {
            auto s = audio.status();
            return !(bool)s["playing"] && !(bool)s["pendingPlayback"];
        },
        "Native shaped audition finishes");
    juce::MemoryBlock bytes;
    check(capture.loadFileAsData(bytes) && bytes.getSize() == 96000 * 8,
          "Native shaped capture has exact sample count");
    auto *samples = (const float *)bytes.getData();
    double nativeError = 0;
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 96000; ++n)
            nativeError =
                std::max(nativeError, (double)std::abs(samples[n * 2 + c] - expected.getSample(c, n)));
    check(nativeError < 1e-7 && audio.status()["underruns"] == baseline["underruns"],
          "Native shaped output matches offline without underruns");
    generation = audio.playDesign("design:" + d.id, file);
    until(
        [&]
        {
            auto s = audio.status();
            return (uint64_t)(int64_t)s["playGeneration"] == generation && (bool)s["playing"];
        },
        "Live shape test begins");
    d.original.layers[0].fadeInPoints[1].y = .35;
    auto sequence = audio.updateDesign(d, generation, sp::monotonicMs());
    until(
        [&]
        {
            auto s = audio.status();
            return (uint64_t)(int64_t)s["heardEdit"] >= sequence &&
                   (uint64_t)(int64_t)s["playGeneration"] == generation;
        },
        "Native callback hears live shape update in the same generation");
    auto final = audio.status();
    audio.stop();
    auto *report = new juce::DynamicObject();
    report->setProperty("checks", checks);
    report->setProperty("fixture", source.getFullPathName());
    report->setProperty("renderMaxError", renderError);
    report->setProperty("nativeMaxError", nativeError);
    report->setProperty("liveShapeLatencyMs", final["editLatencyMs"]);
    report->setProperty("audio", final);
    report->setProperty("timestamp", juce::Time::getCurrentTime().toISO8601(true));
    check(parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(juce::var(report), true)),
          "Fade verification evidence written");
    std::cout << "PASS independent fade shapes, legacy recall, overlap, Undo/Redo and native/render parity: "
              << checks << " checks\n";
}
