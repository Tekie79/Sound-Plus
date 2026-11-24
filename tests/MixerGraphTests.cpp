#include "DesignAudio.h"
#include "AutomationWriter.h"
#include "DesignLive.h"
#include "TrackModel.h"
#include "NativeEffects.h"
#include <iostream>

namespace
{
using namespace sp;
void require(bool okay, const char *message)
{
    if (!okay)
        throw std::runtime_error(message);
}
void reject(const std::function<void()> &test, const char *message)
{
    try { test(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}
juce::AudioBuffer<float> render(const DesignDocument &doc, int block = 512)
{
    DesignGraph graph(doc);
    juce::AudioBuffer<float> result(designLayout(doc).size(), 4096);
    for (int offset = 0; offset < result.getNumSamples(); offset += block)
        graph.render(result, offset, std::min(block, result.getNumSamples() - offset));
    return result;
}
void compare(const juce::AudioBuffer<float> &base, const juce::AudioBuffer<float> &actual,
             const std::function<double(int)> &scale, const char *message)
{
    double error = 0, peak = 0;
    for (int c = 0; c < base.getNumChannels(); ++c)
        for (int n = 0; n < base.getNumSamples(); ++n)
        {
            auto expected = base.getSample(c, n) * scale(n);
            require(std::isfinite(actual.getSample(c, n)), "Graph output must be finite");
            error = std::max(error, std::abs(actual.getSample(c, n) - expected));
            peak = std::max(peak, std::abs((double)base.getSample(c, n)));
        }
    if (error > 2.4e-7 || peak <= .025)
    {
        std::cerr << "error " << error << ", peak " << peak << "\n";
        throw std::runtime_error(message);
    }
}
} // namespace

void runMixerGraphTests(const juce::File &root)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    auto work = root.getChildFile(juce::Uuid().toString());
    work.createDirectory();
    for (auto rate : {44100, 48000, 96000})
    {
        const auto source = work.getChildFile("impulse-" + juce::String(rate) + ".wav");
        {
            juce::WavAudioFormat format;
            auto stream = source.createOutputStream();
            std::unique_ptr<juce::AudioFormatWriter> writer(
                format.createWriterFor(stream.release(), rate, 2, 24, {}, 0));
            juce::AudioBuffer<float> pcm(2, 8192);
            pcm.clear();
            pcm.setSample(0, 900, .25f);
            pcm.setSample(1, 907, -.125f);
            pcm.setSample(0, 1400, .2f);
            pcm.setSample(0, 3500, -.2f);
            pcm.setSample(1, 3800, .18f);
            require(writer && writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()),
                    "Independent PCM fixture creation");
        }
        DesignDocument dry;
        dry.sampleRate = rate;
        dry.original.layout = channelLayout("stereo-v1");
        dry.original.masterDb = 0;
        const auto track = addTrack(dry.original, TrackKind::audio);
        auto clip = makeDesignLayer(inspectDesignSource(source), 0, 8192, true);
        initialiseRouting(clip, dry.original.layout);
        clip.fadeIn = clip.fadeOut = 0;
        dry.original.layers.push_back(std::move(clip));
        attachClip(dry, dry.original.layers.back(), track);
        const auto oracle = render(dry);
        require(oracle.getMagnitude(0, 900, 1) > .24f && oracle.getMagnitude(1, 907, 1) > .12f,
                "Dry impulses maintain exact independent sample origin and channels");
        const auto historical = juce::JSON::toString(designToJson(dry), true);

        {
            DesignGraph graph(dry);
            auto command=juce::var(new juce::DynamicObject());
            auto *o=command.getDynamicObject();
            o->setProperty("owner",track);o->setProperty("parameter","volume");
            o->setProperty("pass",juce::Uuid().toString());o->setProperty("phase","begin");
            o->setProperty("value",-6.);o->setProperty("first",100);o->setProperty("last",1500);
            require(graph.automationCommand(command),"Graph accepted scalar writer");
            juce::AudioBuffer<float> applied(2,4096);
            graph.render(applied,0,1000);
            require(graph.automationReceipt(false).isVoid(),"Unfinished pass has no receipt");
            o->setProperty("value",-12.);o->setProperty("phase","move");
            require(graph.automationCommand(command),"Graph accepted move");
            for(int n=1000;n<4096;n+=61)graph.render(applied,n,std::min(61,4096-n));
            require(graph.automationReceipt(false, 999).isVoid(), "Lookahead cannot certify unheard points");
            auto receipt=graph.automationReceipt(false);
            require((int)receipt["pointCount"]==2 && (int)receipt["end"]==1500,"Exact bounded applied receipt");
            auto scale=[](int n){return n<100 || n>=1500 ? 1. : std::pow(10.,(n<1000 ? -6. : -12.)/20.);};
            compare(oracle, applied,scale,"Live Latch uses applied value");
            auto frozen=dry;
            retainAutomationWrite(frozen.original,track,{{},"volume"},receipt,false);
            frozen=designFromJson(designToJson(frozen));
            compare(oracle,render(frozen,31),scale,"Reopened Write matches independent live PCM");
            auto *curve=automationCurve(*automationLane(frozen.original,track),{{},"volume"});
            require(curve && curve->points.front().frame==99 && curve->points.back().frame==1500,
                    "Exact guard and restore at punch boundaries");
            auto prefix = graph.automationReceipt(true, 999);
            require((int)prefix["pointCount"] == 1 && (int)prefix["end"] == 999,
                    "Stop trims queued second event and unheard hold");
            // Prearmed reader receives the pass before graph warmup and first FIFO fill.
            o->setProperty("value", -6.);
            o->setProperty("phase", "begin");
            o->setProperty("first", 0);
            DesignReader primed(dry, rate, 0, 1500, 0, [] { return false; }, -1, command);
            juce::AudioBuffer<float> early(2, 1500);
            require(primed.read(early, 1500) == 1500, "Prearmed full interval");
            require(std::abs(early.getSample(0, 900) - oracle.getSample(0, 900) * std::pow(10., -.3)) < 1e-6,
                    "First producer buffer contains armed pass");
            auto primedReceipt = primed.automationReceipt(true, 1500);
            auto primedMix = dry.original;
            retainAutomationWrite(primedMix, track, {{}, "volume"}, primedReceipt, false);
            require(
                automationCurve(*automationLane(primedMix, track), {{}, "volume"})->points.front().frame == 0,
                "Prearmed pass begins at exact frame zero");
            auto original=juce::JSON::toString(designToJson(frozen),true);
            receipt.getDynamicObject()->setProperty("discard",true);
            retainAutomationWrite(frozen.original,track,{{},"volume"},receipt,false);
            require(original==juce::JSON::toString(designToJson(frozen),true),"Canceled preview cannot mutate saved state");
        }
        {
            auto edited=dry;
            auto target=AutomationTarget{{},"volume"};
            auto rack=automationTargets(edited.original,track);
            auto gain=*std::find_if(rack.begin(),rack.end(),[&](auto &p){return p.target==target;});
            auto *curve=automationCurve(*automationLane(edited.original,track),target,true);
            curve->points={{juce::Uuid().toString(),0,-18},{juce::Uuid().toString(),3000,-3}};
            auto &originalPoints=curve->points;
            const auto firstId=originalPoints[0].id;
            setAutomationTrim(*curve,gain,900,1400,3);
            for(int n=0;n<4096;++n){
                double expected=-18+std::min(n,3000)*.005+((n>=900&&n<1400)?3:0);
                require(std::abs(automationValue(*curve,gain,n)-expected)<1e-10,"Gain Trim follows underlying slope");
            }
            require(curve->points[0].id==firstId && curve->points[0].value==-18,"Trim preserves editable base");
            const auto json=designToJson(edited);
            require((int)json["version"]==21,"Relative trim opts into manifest 21");
            const auto reopened=designFromJson(json);
            compare(oracle,render(reopened,61),[](int n){
                return std::pow(10.,(-18+std::min(n,3000)*.005+((n>=900&&n<1400)?3:0))/20.);
            },"Reopened gain trim passes independent PCM oracle");
            auto followed = reopened;
            auto *linked = automationCurve(*automationLane(followed.original, track), target);
            linked->followClip = followed.original.layers[0].id;
            followed.original.layers[0].placement += 20;
            followAutomationClips(reopened.original, followed.original);
            require(linked->points[0].frame == 20 && linked->trims[0].first == 920 &&
                        designFromJson(designToJson(followed))
                                .original.tracks[0]
                                .automation.curves[0]
                                .followClip == linked->followClip,
                    "Explicit clip follow shifts underlying points and offsets together");
            auto take=freezeTake(reopened,"trim");
            require((int)takeToJson(take)["recipe"]["recipeVersion"]==12 &&
                takeFromJson(takeToJson(take)).recipeHash==take.recipeHash,"Frozen relative trim round trip");
            auto downgraded=json.clone();downgraded.getDynamicObject()->setProperty("version",11);
            downgraded.getDynamicObject()->setProperty("dsp","multitrack-automation-v1");
            reject([&]{designFromJson(downgraded);},"Historical processing version cannot claim trim");
            AutomationCurve freq; AutomationParameter hz;hz.minimum=20;hz.maximum=20000;hz.base=256;hz.logarithmic=true;hz.units="Hz";
            setAutomationTrim(freq,hz,13,94,2);
            require(automationValue(freq,hz,12)==256 && automationValue(freq,hz,13)==1024 &&
                automationValue(freq,hz,94)==256,"Frequency trim in octaves");
            hz.discrete=true;reject([&]{setAutomationTrim(freq,hz,0,1,.25);},"Discrete trim rejected");
            AutomationCurve dense;
            for(int n=0;n<=500;++n)dense.points.push_back({juce::Uuid().toString(),n,-30+n*.02+std::sin(n*.03)*.35});
            auto original=dense;thinAutomation(dense,gain,.001);
            require(dense.points.size()<30,"Curve thinning reduces redundant points");
            for(int n=0;n<=500;++n)require(std::abs(automationValue(original,gain,n)-automationValue(dense,gain,n))<=.11400001,
                "Thinning bounds all original frame errors");
        }
        auto wet = dry;
        const auto group = addTrack(wet.original, TrackKind::groupBus);
        const auto aux = addTrack(wet.original, TrackKind::fxReturn);
        auto *sender = findTrack(wet.original, track);
        sender->volumeDb = -6;
        sender->sends.emplace_back().destination = aux;
        auto &send = sender->sends.front();
        send.levelDb = -12;
        const double dryGain = std::pow(10., -6. / 20.), sendGain = std::pow(10., -12. / 20.);
        routeTrack(wet.original, track, group);
        compare(oracle, render(wet), [&](int) { return dryGain; }, "Disabled send remains silent");
        send.enabled = true;
        auto &rack = findTrack(wet.original, aux)->inserts;
        rack.stock.push_back(makeStockEffect("peak-limiter"));
        normaliseInsertChain(rack);
        for (const auto block : {1, 32, 61, 257, 512})
            compare(oracle, render(wet, block), [&](int) { return dryGain * (1. + sendGain); },
                    "Post-fader branch sums once and stays sample-aligned for every partition");
        send.postFader = false;
        for (const auto block : {1, 32, 61, 257, 512})
            compare(oracle, render(wet, block), [&](int) { return dryGain + sendGain; },
                    "Pre-fader branch ignores source volume at every partition");
        sender->solo = true;
        compare(oracle, render(wet), [&](int) { return dryGain + sendGain; }, "Solo source retains send return");
        sender->solo = false;
        findTrack(wet.original, aux)->solo = true;
        compare(oracle, render(wet), [&](int) { return sendGain; }, "Solo return isolates wet with feeder");
        findTrack(wet.original, aux)->solo = false;
        findTrack(wet.original, aux)->mute = true;
        compare(oracle, render(wet), [&](int) { return dryGain; }, "Muting return retains dry route");
        findTrack(wet.original, aux)->mute = false;
        sender->mute = true;
        require(render(wet).getMagnitude(0, 4096) == 0.f, "Mute silences the pre tap and main edge");
        sender->mute = false;

        auto cyclic = wet;
        findTrack(cyclic.original, aux)->sends.emplace_back().destination = group;
        findTrack(cyclic.original, group)->sends.emplace_back().destination = aux;
        reject([&] { validateDesign(cyclic); }, "Reciprocal sends rejected");
        auto mixedCycle = wet;
        routeTrack(mixedCycle.original, aux, group);
        findTrack(mixedCycle.original, group)->sends.emplace_back().destination = aux;
        reject([&] { validateDesign(mixedCycle); }, "Combined main and send cycle rejected");
        auto dangling = wet;
        findTrack(dangling.original, track)->sends.front().destination = "missing";
        reject([&] { validateDesign(dangling); }, "Dangling send rejected");
        auto duplicates = wet;
        findTrack(duplicates.original, track)->sends.emplace_back().destination = aux;
        reject([&] { validateDesign(duplicates); }, "Duplicate destination rejected");
        DesignTrack side;
        side.layout = channelLayout("5.1-side-v1");
        DesignTrack rear;
        rear.layout = channelLayout("5.1-rear-v1");
        reject([&] { (void)sendMatrix(side, send, rear.layout, 0, 0, 0, {}); },
               "Side to Rear requires explicit named map");
        reject([&] { (void)sendMatrix(rear, send, side.layout, 0, 0, 0, {}); },
               "Rear to Side requires explicit named map");
        auto mapped = send;
        mapped.roles = {{"Lb", "Ls", 0}, {"Rb", "Rs", -6}};
        const auto matrix = sendMatrix(rear, mapped, side.layout, 0, 0, 0, {});
        for (int output = 0; output < side.layout.size(); ++output)
            for (int input = 0; input < rear.layout.size(); ++input)
            {
                const float expected = output == 4 && input == 4 ? 1.f
                                     : output == 5 && input == 5 ? (float)dryGain
                                                                 : 0.f;
                require(std::abs(matrix[output][input] - expected) < 1e-7f,
                        "Explicit Rear-to-Side conversion isolates the requested named channels");
            }

        const auto sendId = send.id;
        addAutomationPoint(wet.original, track, {sendId, "send-enable"}, 0, 0, rate);
        addAutomationPoint(wet.original, track, {sendId, "send-enable"}, 1200, 1, rate);
        addAutomationPoint(wet.original, track, {sendId, "send-level"}, 0, -12, rate);
        compare(oracle, render(wet, 61), [&](int n) { return dryGain + (n >= 1200 ? sendGain : 0.); },
                "Hidden send automation is applied at the exact frame");
        require((int)designToJson(wet)["version"] == 20, "Explicit new mixer manifest");
        auto file = work.getChildFile("mixer-" + juce::String(rate) + ".soundplus");
        saveDesignAtomic(file, wet);
        auto restored = loadDesign(file);
        compare(render(wet), render(restored, 61), [](int) { return 1.; }, "Project reopen preserves mixer PCM");
        require(findTrack(restored.original, track)->sends[0].id == sendId, "Stable send ID survives reload");
        auto take = freezeTake(wet, "Mixer take");
        require((int)takeToJson(take)["recipe"]["recipeVersion"] == 11, "Explicit mixer take version");
        auto recalled = takeFromJson(takeToJson(take));
        require(recalled.recipeHash == take.recipeHash && hasMixerRouting(recalled.recipe),
                "Frozen mixer recipe authority survives decode");
        const auto beforeDeletion = juce::JSON::toString(designToJson(restored), true);
        reject([&] { deleteTrack(restored.original, aux); }, "Incoming sends require confirmed deletion");
        require(juce::JSON::toString(designToJson(restored), true) == beforeDeletion,
                "Refusing deletion preserves bus, sends and automation");
        deleteTrack(restored.original, aux, true);
        require(findTrack(restored.original, track)->sends.empty() &&
                    automationCurve(findTrack(restored.original, track)->automation, {sendId, "send-level"}),
                "Return deletion detaches routes and preserves orphan curve identity");
        require(juce::JSON::toString(designToJson(dry), true) == historical, "Legacy no-send JSON unchanged");
        std::cout << rate << " Hz: five partitions, exact gain/tap/solo/role/automation, save/reopen and frozen recipe\n";
    }
}

void runMixerStemTests(const juce::File &root)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    auto work = root.getChildFile(juce::Uuid().toString());
    work.createDirectory();
    DesignDocument doc;
    doc.original.layout = channelLayout("stereo-v1");
    doc.renderSettings.layout = "same";
    doc.renderSettings.tailSeconds = .125;
    doc.original.masterDb = -18;
    doc.original.inserts.stock.push_back(makeStockEffect("peak-limiter"));
    auto sourceTrack = addTrack(doc.original, TrackKind::audio);
    auto group = addTrack(doc.original, TrackKind::groupBus);
    auto aux = addTrack(doc.original, TrackKind::fxReturn);
    auto &source = *findTrack(doc.original, sourceTrack);
    source.output = group;
    source.sends.emplace_back().destination = aux;
    source.sends.front().levelDb = -6;
    source.sends.front().enabled = true;
    findTrack(doc.original, group)->name = findTrack(doc.original, aux)->name = "Repeated name.WAV";
    auto file = work.getChildFile("stereo.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(
            file.createOutputStream().release(), 48000, 2, 24, {}, 0));
        juce::AudioBuffer<float> pcm(2, 8192);
        pcm.clear();
        pcm.setSample(0, 900, .25f);
        pcm.setSample(1, 1500, -.125f);
        require(writer && writer->writeFromAudioSampleBuffer(pcm, 0, 8192), "Publish input fixture");
    }
    auto clip = makeDesignLayer(inspectDesignSource(file), 0, 8192, true);
    initialiseRouting(clip, doc.original.layout);
    clip.fadeIn = clip.fadeOut = 0;
    doc.original.layers.push_back(std::move(clip));
    attachClip(doc, doc.original.layers.back(), sourceTrack);
    const auto outputFolder = work.getChildFile("Outputs");
    const auto quit = [] { return false; };
    const auto progress = [](double) {};
    reject([&] { renderMixerStems(doc, outputFolder, {group, sourceTrack}, quit, progress); },
           "Group and child cannot claim an overlapping batch");
    auto outputs = renderMixerStems(doc, outputFolder, {group, aux}, quit, progress);
    require(outputs.size() == 2 && outputs[0].path != outputs[1].path, "Unique files for duplicate strip names");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    constexpr double wet = .5011872336272722;
    for (int i = 0; i < 2; ++i)
    {
        auto path = juce::File(outputs[(size_t)i].path);
        require(path.getFileName() == juce::String(i + 1).paddedLeft('0', 2) + " Repeated name.wav",
                "Stem names use one WAV suffix even when the track label is a filename");
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(path));
        require(reader && reader->lengthInSamples == 8192 + 6000 && reader->sampleRate == 48000 &&
                    reader->numChannels == 2 && reader->bitsPerSample == 24,
                "Common sample origin, duration, tail, layout and depth");
        juce::AudioBuffer<float> pcm(2, 8192);
        reader->read(&pcm, 0, 8192, 0, true, true);
        const auto gain = i == 0 ? 1. : wet;
        for (int n = 0; n < 8192; ++n)
            require(std::abs(pcm.getSample(0, n) - (n == 900 ? .25 * gain : 0.)) < 2e-7 &&
                        std::abs(pcm.getSample(1, n) - (n == 1500 ? -.125 * gain : 0.)) < 2e-7,
                    "Pre-Master PCM, exact impulse alignment and independent route gain");
    }
    const auto linear = [&]
    {
        auto copy = doc;
        copy.original.masterDb = 0;
        copy.original.inserts = {};
        return renderDesign(copy, work.getChildFile("Linear"), copy.renderSettings, quit, progress);
    }();
    std::unique_ptr<juce::AudioFormatReader> masterReader(formats.createReaderFor(juce::File(linear.path)));
    require(masterReader && masterReader->lengthInSamples == 14192, "Linear Master shares the stem origin");
    juce::AudioBuffer<float> sum(2, 14192), stem(2, 14192), reference(2, 14192);
    sum.clear();
    masterReader->read(&reference, 0, 14192, 0, true, true);
    for (auto &entry : outputs)
    {
        std::unique_ptr<juce::AudioFormatReader> stemReader(formats.createReaderFor(juce::File(entry.path)));
        require(stemReader && stemReader->read(&stem, 0, 14192, 0, true, true), "Read complete stem PCM");
        for (int channel = 0; channel < sum.getNumChannels(); ++channel)
            sum.addFrom(channel, 0, stem, channel, 0, stem.getNumSamples());
    }
    compare(reference, sum, [](int) { return 1.; }, "Linear Master equals the sum of non-overlapping stems");
    auto master = renderDesign(doc, work.getChildFile("Master"), doc.renderSettings, quit, progress);
    require(master.samples == linear.samples && master.peak < linear.peak * .2,
            "Master chain changes the Mix WAV and never changes pre-Master stems");
    auto receipt = juce::File(outputs.front().sourceProvenance["receipt"].toString());
    auto json = juce::JSON::parse(receipt);
    require(json["files"].size() == 2 && json["files"][0]["sha256"] == juce::var(outputs[0].sha256),
            "Complete directory receipt commits both WAV fingerprints");
    auto before = receipt.loadFileAsString();
    bool cancel = false;
    reject([&] { renderMixerStems(doc, outputFolder, {group, aux}, [&] { return cancel; },
                                [&](double) { cancel = true; }); },
           "Cancelled batch does not publish");
    require(receipt.loadFileAsString() == before && outputFolder.getNumberOfChildFiles(
                    juce::File::findDirectories, "*") == 1,
            "Failed batch cleans staging and preserves the earlier output");
    doc.outputs = outputs;
    doc.outputs.push_back(master);
    auto saved = work.getChildFile("stem-receipt.soundplus");
    saveDesignAtomic(saved, doc);
    require(loadDesign(saved).outputs.size() == 3, "Every stem persists in the project output receipt list");
    std::cout << "Aligned pre-Master stems: exact impulse PCM, common tail, additive reference, Master isolation, receipt, cancellation and reopen\n";
}
