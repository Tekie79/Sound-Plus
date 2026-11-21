#include "DesignAudio.h"
#include "DesignRecovery.h"
#include "PerformanceState.h"
#include "OrderedInsertList.h"
#include <iostream>
#include <CoreFoundation/CoreFoundation.h>
namespace
{
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
std::string json(const juce::var &v)
{
    return juce::JSON::toString(v, true).toStdString();
}
juce::AudioBuffer<float> read(const juce::File &f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    check(r != nullptr, "Read baseline PCM");
    juce::AudioBuffer<float> b((int)r->numChannels, (int)r->lengthInSamples);
    r->read(&b, 0, b.getNumSamples(), 0, true, true);
    return b;
}
void samePcm(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b)
{
    double error = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < a.getNumSamples(); ++n)
            error = std::max(error, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
    check(error < 1e-7, "Baseline-generated v7 and migrated graph PCM agree");
}
auto values(const std::vector<sp::DesignVariation> &vs)
{
    std::vector<std::map<juce::String, std::array<double, 4>>> out;
    for (auto &v : vs)
    {
        std::map<juce::String, std::array<double, 4>> row;
        for (auto &l : v.mix.layers)
            for (auto &e : l.inserts.stock)
                row[e.id] = e.parameters;
        out.push_back(row);
    }
    return out;
}
} // namespace
void runOrderedCorrectionTests(const juce::File &output, const juce::File &fixtures)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = output.getChildFile(Uuid().toString());
    root.createDirectory();
    for (auto kind : {"track", "master", "variation"})
    {
        auto input = fixtures.getChildFile(String(kind) + ".soundplus");
        auto old = JSON::parse(input)["payload"];
        check((int)old["version"] == 7, "Genuine unmodified baseline manifest 7");
        auto d = loadDesign(input);
        check(json(designMixToJson(d.original, true)) == json(old["original"]),
              "All source choices/routing/effect identities/flags/locks preserved");
        if (!d.variations.empty())
            check(json(designMixToJson(d.variations[0].mix, true)) == json(old["variations"][0]["mix"]),
                  "Variation-only legacy inserts preserved");
        check(d.takes[0].recipeHash == old["takes"][0]["recipeHash"].toString() &&
                  d.takes[0].eventHash == old["takes"][0]["eventHash"].toString(),
              "Historical take hashes remain exact");
        auto forgedCanonical = designToJson(d);
        forgedCanonical.getDynamicObject()->setProperty("version", 7);
        forgedCanonical.getDynamicObject()->setProperty("dsp", old["dsp"]);
        bool modernRejected = false;
        try
        {
            designFromJson(forgedCanonical);
        }
        catch (...)
        {
            modernRejected = true;
        }
        check(modernRejected, "Manifest 7 cannot claim canonical ordered-insert fields");
        auto f = root.getChildFile(String(kind) + ".soundplus");
        saveDesignAtomic(f, d);
        auto reopened = loadDesign(f);
        check((int)designToJson(reopened)["version"] == 9, "Valid pooled inserts upgrade to manifest 9");
        check(reopened.takes[0].recipeHash == d.takes[0].recipeHash,
              "Save/reopen never rehashes historical recipe");
        auto relocate = [&](DesignMix &m)
        {
            auto source = [&](Asset &a)
            {
                auto path = File(a.path).existsAsFile() ? File(a.path) : fixtures.getChildFile(a.name);
                auto fresh = inspectDesignSource(path);
                fresh.id = a.id;
                fresh.revision = a.revision;
                fresh.detectedChannels = a.detectedChannels;
                fresh.layoutOverride = a.layoutOverride;
                fresh.layoutConfidence = a.layoutConfidence;
                a = fresh;
            };
            for (auto &l : m.layers)
            {
                source(l.source);
                if (l.pool)
                    for (auto &e : l.pool->entries)
                        source(e.source);
            }
        };
        relocate(reopened.original);
        for (auto &v : reopened.variations)
            relocate(v.mix);
        DesignGraph graph(reopened);
        AudioBuffer<float> actual(2, 8192);
        graph.render(actual, 0, 8192);
        samePcm(read(fixtures.getChildFile(String(kind) + "-baseline.wav")), actual);
        auto &take = reopened.takes[0];
        for (auto &stamp : take.sources)
            for (auto &layer : take.recipe->layers)
                if (stamp.id == layer.source.id)
                {
                    auto path = File(layer.source.path).existsAsFile()
                                    ? File(layer.source.path)
                                    : fixtures.getChildFile(layer.source.name);
                    auto fresh = inspectDesignSource(path);
                    stamp.overridePath = fresh.path;
                    stamp.overrideSize = fresh.size;
                    stamp.overrideModified = fresh.modified;
                    stamp.overrideInode = fresh.inode;
                }
        reopened.activeTake = take.id;
        DesignReader replay(reopened, 48000, 0, -1, 0);
        AudioBuffer<float> body(2, 4096);
        check(replay.read(body, 4096) == 4096, "Historical take body preserved");
        samePcm(read(fixtures.getChildFile(String(kind) + "-take-baseline.wav")), body);
        DesignRecoveryStore store(root.getChildFile(String(kind) + "-recovery"));
        auto legacyRecovery = root.getChildFile(String(kind) + "-recovery").getChildFile(d.id + ".soundplus");
        legacyRecovery.getParentDirectory().createDirectory();
        check(input.copyFileTo(legacyRecovery), "Copy genuine v7 recovery snapshot");
        auto recovered = store.entries();
        check(recovered.size() == 1 && recovered[0].document.id == d.id,
              "Recovery loads the migrated legacy recipe");
        for (auto tag : {"stock-sinc32-v1", "stock-layout-v1", "hosted-racks-v1", "ordered-inserts-v1"})
        {
            auto forged = old.clone();
            forged.getDynamicObject()->setProperty("dsp", tag);
            bool rejected = false;
            try
            {
                designFromJson(forged);
            }
            catch (...)
            {
                rejected = true;
            }
            check(rejected, "Forged historical DSP tag still rejected");
        }
    }
    auto legacyGenerator = loadDesign(fixtures.getChildFile("generation.soundplus"));
    auto expected = JSON::parse(fixtures.getChildFile("generation-expected.json"));
    auto generated = generateVariations(legacyGenerator, 0x1234);
    for (size_t i = 0; i < generated.size(); ++i)
        check(json(designMixToJson(generated[i].mix, true)) == json(expected[(int)i]),
              "Unchanged legacy canonical order retains baseline RNG behavior exactly");
    auto d = loadDesign(fixtures.getChildFile("track.soundplus"));
    d.takes.clear();
    auto &l = d.original.layers[0];
    l.locked = false;
    l.locks = 0;
    l.inserts.stock = {makeStockEffect("gain"), makeStockEffect("filter"), makeStockEffect("delay")};
    l.inserts.order = {{InsertKind::stock, l.inserts.stock[2].id},
                       {InsertKind::stock, l.inserts.stock[1].id},
                       {InsertKind::stock, l.inserts.stock[0].id}};
    d.variationSettings.enabled = varyEffects;
    for (bool locks : {false, true})
    {
        if (locks)
        {
            l.inserts.stock[0].locked = true;
            l.inserts.stock[1].parameterLocks = 1;
            addMacroMapping(d.original, 0,
                            {performanceEffectTarget(l.id, l.inserts.stock[2], 2), .1, .8, "linear", false});
            d.original.performance.macros[0].enabled = true;
        }
        auto before = values(generateVariations(d, 0x1234));
        auto saved = root.getChildFile("variation.soundplus");
        saveDesignAtomic(saved, d);
        auto after = values(generateVariations(loadDesign(saved), 0x1234));
        check(before == after,
              "Seeded variation draws belong to canonical stable effect IDs across round trip");
        if (locks)
            for (auto &v : before)
            {
                check(v.at(l.inserts.stock[0].id) == l.inserts.stock[0].parameters,
                      "Whole-effect lock retained");
                check(v.at(l.inserts.stock[1].id)[0] == l.inserts.stock[1].parameters[0],
                      "Parameter lock retained");
                check(v.at(l.inserts.stock[2].id)[2] == l.inserts.stock[2].parameters[2],
                      "Macro-owned parameter retained");
            }
    }
    // A genuine peer routes keys to its currently focused child, rather than calling a row directly.
    Component host;
    host.setBounds(100, 100, 480, 220);
    OrderedInsertList list;
    host.addAndMakeVisible(list);
    list.setBounds(0, 0, 460, 160);
    host.addToDesktop(ComponentPeer::windowIsTemporary);
    host.setVisible(true);
    std::vector<OrderedInsertList::Item> items;
    for (int i = 0; i < 16; ++i)
        items.push_back({{i < 8 ? InsertKind::stock : InsertKind::hosted, Uuid().toString()},
                         "Effect " + String(i + 1),
                         false,
                         false});
    auto target = items[0].ref;
    InsertRef opened;
    list.select = [&](auto ref) { list.setItems(items, ref, true); };
    list.move = [&](auto ref, int to)
    {
        auto at = std::find_if(items.begin(), items.end(), [&](auto &x) { return x.ref == ref; });
        auto v = *at;
        items.erase(at);
        items.insert(items.begin() + to, v);
        list.setItems(items, ref, true);
    };
    list.open = [&](auto ref) { opened = ref; };
    list.setItems(items, target, true);
    auto *vp = dynamic_cast<Viewport *>(list.getChildComponent(0));
    auto *row = vp->getViewedComponent()->getChildComponent(0);
    row->grabKeyboardFocus();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, .02, false);
    check(Component::getCurrentlyFocusedComponent() == row, "Actual row acquired native peer focus");
    auto key = [&](int code)
    {
        check(host.getPeer()->handleKeyPress(KeyPress(code, ModifierKeys::altModifier, 0)),
              "Peer delivers reorder shortcut to focused row");
        check(Component::getCurrentlyFocusedComponent() == row, "Stable row preserves actual keyboard focus");
    };
    key(KeyPress::downKey);
    key(KeyPress::downKey);
    check(items[2].ref == target, "Two consecutive Down moves address same slot");
    key(KeyPress::upKey);
    check(items[1].ref == target, "Up moves that same slot");
    check(host.getPeer()->handleKeyPress(KeyPress(KeyPress::returnKey)) && opened == target,
          "Return opens same focused slot");
    for (int i = 0; i < 14; ++i)
        key(KeyPress::downKey);
    check(items.back().ref == target && vp->getViewPositionY() > 0,
          "All sixteen rows reachable with scrolling");
    auto *button = row->getChildComponent(0);
    button->grabKeyboardFocus();
    list.setItems(items, target, true);
    check(Component::getCurrentlyFocusedComponent() == button, "Refresh retains focused subcontrol");
    TextButton outside;
    host.addAndMakeVisible(outside);
    outside.setBounds(0, 170, 100, 30);
    outside.setWantsKeyboardFocus(true);
    outside.grabKeyboardFocus();
    list.setItems(items, target, true);
    check(Component::getCurrentlyFocusedComponent() == &outside,
          "Background refresh does not steal outside focus");
    host.removeFromDesktop();
    std::cout << "Ordered corrections passed: " << checks << "; " << root.getFullPathName() << "\n";
}
