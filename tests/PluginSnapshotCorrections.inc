// E: native publication is held while actual Snapshot/Generate/menu actions run.
auto nativePending = [&](int sequence, double x)
{
    captureHold.replaceWithText("hold native publication");
    gesture(trackEndpoint, sequence, x);
    pump(130);
};
auto gainOf = [](const HostedRack &r)
{ return (double)JSON::parse(r.slots[0].settings)["parameters"][0]["value"]; };
auto verifyFresh = [&](const HostedRack &r, double x)
{
    HostedRackClient fresh(r, channelLayout("mono-v1"), 48000, cancel);
    AudioBuffer<float> b(1, 512);
    for (int n = 0; n < 512; ++n)
        b.setSample(0, n, .2f);
    fresh.process(b, 512);
    require(std::abs(b.getSample(0, 511) - .4 * x) < 1e-6,
            "Captured command recipe restores intended native Gain in a fresh processor: actual=" +
                String(b.getSample(0, 511), 9) + " expected=" + String(.4 * x, 9));
};
const auto beforeSnapshots = session.document.variations.size();
nativePending(400, .38);
session.saveSnapshot("Held native Snapshot");
require(session.document.variations.size() == beforeSnapshots,
        "Snapshot cannot switch context before native capture completes");
captureHold.deleteFile();
wait(
    [&]
    {
        return session.document.variations.size() == beforeSnapshots + 1 &&
               session.document.activeVariation.isNotEmpty();
    },
    "Actual Save Snapshot completes after held native publication");
require(std::abs(gainOf(session.document.activeMix().layers[0].inserts.hosted) - .38) < 1e-6,
        "Snapshot contains latest native state, not the pre-capture recipe");
verifyFresh(session.document.activeMix().layers[0].inserts.hosted, .38);
session.selectVariation("");
wait([&] { return session.document.activeVariation.isEmpty() && endpointFor(trackId).isObject(); },
     "Return to Original prepares its correct live incarnation");
trackEndpoint = endpointFor(trackId);
session.openLiveEditor(trackId);
pump(100);
auto beforeGenerated = session.document.variations.size();
nativePending(401, .395);
session.generate(113);
require(session.document.variations.size() == beforeGenerated, "Generate waits for pending native capture");
captureHold.deleteFile();
wait([&] { return session.document.variations.size() > beforeGenerated; },
     "Actual Generate completes after coherent native capture");
for (size_t n = beforeGenerated; n < session.document.variations.size(); ++n)
    require(std::abs(gainOf(session.document.variations[n].mix.layers[0].inserts.hosted) - .395) < 1e-6,
            "Every generated candidate retains latest captured external state");
verifyFresh(session.document.variations.back().mix.layers[0].inserts.hosted, .395);
auto presetFile = work.getChildFile("Actual Effect actions menu.soundplus-insert");
nativePending(402, .415);
require(LiveCorrectionTestAccess::savePresetMenu(actualView, layer.id, presetFile),
        "Actual visible Effect actions menu supplies the Save preset action");
require(!presetFile.existsAsFile(), "Visible preset menu cannot write pre-capture settings");
captureHold.deleteFile();
wait([&] { return presetFile.existsAsFile(); },
     "Visible Save preset menu completes after held native capture");
auto preset = loadProcessingJson(presetFile)["settings"];
require(std::abs((double)preset["parameters"][0]["value"] - .415) < 1e-6,
        "Saved menu preset contains latest native settings");
HostedRack presetRack;
presetRack.slots = {{Uuid().toString(), JSON::toString(preset, true), false}};
verifyFresh(presetRack, .415);
