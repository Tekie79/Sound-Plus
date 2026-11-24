#include "AcousticMetadata.h"
#include "ModelPack.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
namespace
{
void require(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}
} // namespace
void testAcousticMetadata(const juce::File &base)
{
    using namespace juce;
    using namespace sp;
    auto dir = base.getChildFile(Uuid().toString());
    dir.createDirectory();
    struct Cleanup
    {
        File f;
        ~Cleanup()
        {
            f.deleteRecursively();
        }
    } cleanup{dir};
    StoredRegion strong;
    strong.vector[0] = 1;
    strong.first = 25;
    strong.last = 40;
    StoredRegion quiet;
    quiet.vector[2] = 1;
    AcousticPrototype p{"A", strong.vector}, q{"B", {}};
    q.vector[1] = 1;
    auto active = [] { return false; };
    auto r = rankAcoustic({quiet, strong}, {q, p}, quiet.vector, active);
    require(r.choices.size() == 1 && r.choices[0].category == "A" && r.choices[0].moment.startSample == 25 &&
                r.choices[0].similarity == 1,
            "Rank and match strongest audible interval");
    require(rankAcoustic({strong}, {p, p}, quiet.vector, active).choices.empty(),
            "Abstain on identical acoustic support");
    require(rankAcoustic({quiet}, {q, p}, quiet.vector, active).choices.empty(),
            "Silence does not get a category");
    require(rankAcoustic({strong}, {q, p}, quiet.vector, [] { return true; }).status == "Cancelled",
            "Cancel ranking");
    auto wav = dir.getChildFile("unlabelled.wav");
    {
        WavAudioFormat f;
        auto w = std::unique_ptr<AudioFormatWriter>(
            f.createWriterFor(wav.createOutputStream().release(), 48000, 6, 16, {}, 0));
        AudioBuffer<float> b(6, 4800);
        b.clear();
        b.setSample(5, 75, 0.75f);
        require(w && w->writeFromAudioSampleBuffer(b, 0, 4800), "Write last-channel-only transient");
    }
    auto hash = SHA256(wav).toHexString();
    auto file = dir.getChildFile("test.sqlite");
    Catalog db(file);
    auto root = db.addRootRecord(dir);
    FileStamp st;
    require(fileStamp(wav, st), "Fixture stamp");
    db.discover(root, wav.getFileName(), st, 1);
    auto a = db.existing(root.id, wav.getFileName());
    db.metadata(a, .1, 6, 48000, {}, 4800);
    a = db.asset(a.id);
    db.snapshotMetadata(a, var(new DynamicObject));
    require(acousticSuggestions(file, a, false, active).status == "Local model unavailable", "No model");
    require(acousticSuggestions(file, a, true, active).status == "Audio analysis incomplete",
            "Incomplete index");
    auto offline = a;
    offline.online = false;
    require(acousticSuggestions(file, offline, true, active).choices.empty(), "Offline");
    AnalysisUpdate u;
    u.asset = a;
    u.model = modelId();
    u.preprocessing = preprocessingId();
    u.embedding.assign(strong.vector.begin(), strong.vector.end());
    for (int i = 0; i < 80; ++i)
    {
        u.start = i * 60;
        u.end = u.start + 60;
        u.nextStart = u.end;
        u.done = i == 79;
        require(db.commitAnalysis(u), "Populate cached windows");
    }
    auto sampled = db.acousticWindows(a, u.model, u.preprocessing, active);
    require(sampled.size() == 32 && sampled.front().first == 0 && sampled.back().last == 4800,
            "Bounded sample from both ends");
    int checks = 0;
    auto cancelled = db.acousticWindows(a, u.model, u.preprocessing, [&] { return ++checks > 3; });
    require(cancelled.size() == 3, "Cancel mid-query");
    require(db.acousticWindows(a, "obsolete", u.preprocessing, active).empty(), "Model isolation");
    CollectionEdit e;
    e.id = "category";
    e.name = "TOOLS";
    e.parent = acousticPolicy();
    e.items = {{a}};
    e.action = "meta-acoustic-reject";
    db.editMetadata(e);
    require(db.dismissedAcoustic(a, e.parent).contains("TOOLS"), "Explicit rejection");
    e.action = "meta-undo";
    db.editMetadata(e);
    require(db.dismissedAcoustic(a, e.parent).isEmpty(), "Undo rejection");
    e.action = "meta-redo";
    db.editMetadata(e);
    require(db.dismissedAcoustic(a, e.parent).size() == 1, "Redo rejection");
    e.name = "WATER";
    e.action = "meta-acoustic-apply";
    db.editMetadata(e);
    require(db.describeMetadata(a.id)["origins"]["category"].toString() == "user",
            "Review creates user lock");
    bool refused = false;
    try
    {
        db.editMetadata(e);
    }
    catch (...)
    {
        refused = true;
    }
    require(refused, "Stale suggestion cannot replace an assigned category");
    e.action = "meta-undo";
    db.editMetadata(e);
    require(!db.describeMetadata(a.id)["fields"].hasProperty("category"), "Undo accepted tag");
    auto renamed = dir.getChildFile("elsewhere.wav");
    require(wav.moveFileTo(renamed), "Rename fixture");
    require(acousticSuggestions(file, a, true, active).status == "Recording offline or changed",
            "Rename invalidates old request");
    e.action = "meta-acoustic-apply";
    refused = false;
    try
    {
        db.editMetadata(e);
    }
    catch (...)
    {
        refused = true;
    }
    require(refused, "Changed path rejected at writer");
    auto newRev = a;
    ++newRev.revision;
    require(db.dismissedAcoustic(newRev, e.parent).isEmpty(), "Dismissals scoped to source revision");
    require(SHA256(renamed).toHexString() == hash, "WAV byte preservation");
    std::cout << "Acoustic ranking, bounds, cancellation, identity, review and undo passed\n";
}
void inspectAcousticFixture(const juce::File &file)
{
    using namespace juce;
    using namespace sp;
    Catalog db(file);
    juce::Array<var> rows;
    for (auto &a : db.search("", {}, 200))
    {
        auto r = acousticSuggestions(file, a, true, [] { return false; });
        var row(new DynamicObject);
        row.getDynamicObject()->setProperty("file", a.name);
        row.getDynamicObject()->setProperty("status", r.status);
        Array<var> choices;
        for (auto &c : r.choices)
        {
            var item(new DynamicObject);
            item.getDynamicObject()->setProperty("category", c.category);
            item.getDynamicObject()->setProperty("similarity", c.similarity);
            item.getDynamicObject()->setProperty("first", (int64)c.moment.startSample);
            item.getDynamicObject()->setProperty("last", (int64)c.moment.endSample);
            choices.add(item);
        }
        row.getDynamicObject()->setProperty("choices", choices);
        rows.add(row);
    }
    std::cout << JSON::toString(rows).toStdString() << "\n";
}
