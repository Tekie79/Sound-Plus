#include "MetadataFields.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <sys/resource.h>
namespace
{
using namespace juce;
using namespace sp;
void check(bool b, const char *why)
{
    if (!b)
        throw std::runtime_error(why);
}
void measure(const File &root, int rowCount)
{
    Catalog db(root.getChildFile("scale-" + String(rowCount) + ".sqlite"));
    auto dir = db.addRootRecord(root);
    auto sql = "WITH RECURSIVE seq(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<" +
               String(rowCount) +
               ") INSERT INTO "
               "assets(id,root_id,relative,name,size,modified,inode,channels,duration,online,state,seen) "
               "SELECT printf('asset%d',x),'" +
               dir.id + "',printf('%d.wav',x),printf('%d.wav',x),100,x,x,2,3,1,'ready',1 FROM seq";
    db.begin();
    db.exec(sql.toRawUTF8());
    db.exec("INSERT INTO metadata_fields SELECT id,'category',CASE WHEN rowid%100=0 THEN 'WIND' ELSE 'OTHER' "
            "END FROM assets");
    db.exec("INSERT INTO metadata_fts(rowid,terms) SELECT rowid,CASE WHEN rowid%100=0 THEN 'wind air' ELSE "
            "'other' END FROM assets");
    db.commit();
    std::vector<double> times;
    for (int i = 0; i < 40; ++i)
    {
        auto start = Time::getMillisecondCounterHiRes();
        check(db.search("category:WIND channels:2", dir.id, 100, (i % 8) * 100).size() == 100,
              "Filtered catalog page");
        check(db.countMatches("meta:wind", dir.id) == rowCount / 100, "Filtered metadata count");
        times.push_back(Time::getMillisecondCounterHiRes() - start);
    }
    std::sort(times.begin(), times.end());
    struct rusage usage
    {
    };
    getrusage(RUSAGE_SELF, &usage);
    std::cout << rowCount << " catalog rows, " << rowCount / 100
              << " hits, 100-row deep page + FTS count: p50 " << times[20] << " ms, p95 " << times[37]
              << " ms, process peak RSS " << usage.ru_maxrss / 1048576. << " MiB\n";
}
} // namespace
void runCatalogMetadataTests(const juce::File &parent)
{
    using namespace juce;
    using namespace sp;
    auto dir = parent.getChildFile(Uuid().toString());
    auto media = dir.getChildFile("media");
    media.createDirectory();
    auto wav = media.getChildFile("AIRBlow_fan.wav");
    StringPairArray tags;
    tags.set(WavAudioFormat::bwavDescription, "Distant motor");
    tags.set(WavAudioFormat::bwavOriginator, "Studio");
    {
        WavAudioFormat f;
        auto w = std::unique_ptr<AudioFormatWriter>(
            f.createWriterFor(wav.createOutputStream().release(), 48000, 1, 16, tags, 0));
        AudioBuffer<float> b(1, 480);
        b.clear();
        check(w && w->writeFromAudioSampleBuffer(b, 0, 480), "Write tagged WAV");
    }
    auto bytes = SHA256(wav).toHexString();
    auto file = dir.getChildFile("metadata.sqlite");
    String id;
    {
        Catalog db(file);
        auto root = db.addRootRecord(media);
        FileStamp st;
        check(fileStamp(wav, st), "Stamp generated fixture");
        db.discover(root, wav.getFileName(), st, 1);
        auto a = db.existing(root.id, wav.getFileName());
        id = a.id;
        db.metadata(a, .01, 1, 48000, {}, 480);
        WavAudioFormat f;
        std::unique_ptr<AudioFormatReader> reader(f.createReaderFor(wav.createInputStream().release(), true));
        auto raw = reader->metadataValues;
        raw.set("private.録音", "風");
        addWaveTags(wav, raw);
        auto embedded = embeddedFields(raw);
        db.snapshotMetadata(a, embedded);
        check(db.describeMetadata(id)["fields"]["category"].toString() == "AIR" &&
                  db.describeMetadata(id)["fields"]["description"].toString() == "Distant motor",
              "Exact UCS prefix and real BWF description");
        check(!namedFields("almostAIRBlow_fan.wav").hasProperty("category"),
              "Unknown category left unassigned");
        check(db.countMatches("motor", root.id) == 1 &&
                  db.search("subcategory:BLOW channels:1 type:wav", root.id, 1).size() == 1,
              "Free text and combined typed filters");
        CollectionEdit alias;
        alias.action = "meta-alias";
        alias.id = "category";
        alias.name = "AIR";
        alias.parent = "atmos";
        db.editMetadata(alias);
        check(db.countMatches("category:atmos") == 1, "User-defined taxonomy alias");
        db.materialize("motor", root.id);
        check(db.page(0, 1).front().id == id, "Plain metadata and paged snapshot agree");
        CollectionEdit fav;
        fav.action = "favorite";
        fav.items = {{a}};
        db.editCollections(fav);
        db.setScope("favorites");
        check(db.countMatches("meta:motor", root.id) == 1 && db.countMatches("category:WIND", root.id) == 0,
              "Favorite scope intersects metadata");
        db.setSemanticFilter("category:WIND");
        check(db.scopeRanges(root.id).empty(), "Metadata limits vector candidates before scoring");
        db.setScope({});
        db.setSemanticFilter({});
        CollectionEdit e;
        e.items = {{a}};
        e.id = "description";
        e.name = "Wind tunnel";
        e.action = "meta-set";
        db.editMetadata(e);
        auto snapshot = db.metadataManifest(e.items);
        db.importMetadata(snapshot);
        db.importMetadata(snapshot);
        e.action = "meta-undo";
        db.editMetadata(e);
        check(db.describeMetadata(id)["fields"]["description"].toString() == "Distant motor",
              "Idempotent manifest adds no redundant Undo");
        e.action = "meta-redo";
        db.editMetadata(e);
        db.snapshotMetadata(a, embedded);
        check(db.describeMetadata(id)["origins"]["description"].toString() == "user",
              "Protected edit survives rescan");
        auto damaged = snapshot.clone();
        damaged["records"][0].getDynamicObject()->setProperty("sha256", "wrong");
        bool rejected = false;
        try
        {
            db.importMetadata(damaged);
        }
        catch (...)
        {
            rejected = true;
        }
        check(rejected, "Reject wrong fingerprint");
        auto receipt = var(new DynamicObject);
        receipt.getDynamicObject()->setProperty("provider", "QA");
        receipt.getDynamicObject()->setProperty("sha256", bytes);
        db.providerMetadata(a, receipt);
        check(db.countMatches("generated:true channels:1") == 1, "Filter immutable provider provenance");
        std::vector<BookmarkRequest> many;
        db.begin();
        for (int i = 0; i < 300; ++i)
        {
            auto n = String(i) + ".wav";
            auto stamp = st;
            stamp.inode += i + 1;
            db.discover(root, n, stamp, 1);
            many.push_back({db.existing(root.id, n)});
        }
        db.commit();
        e.action = "meta-set";
        e.id = "tags";
        e.name = "Foley, Steel";
        e.items = many;
        db.editMetadata(e);
        check(db.countMatches("tag:steel") == 300, "Bulk catalog tag");
        e.action = "meta-undo";
        db.editMetadata(e);
        check(db.countMatches("tag:steel") == 0, "One Undo restores 300 files");
        auto changed = st;
        changed.modified += 1;
        db.discover(root, wav.getFileName(), changed, 2);
        auto newer = db.asset(id);
        db.snapshotMetadata(newer, embedded);
        auto updated = db.describeMetadata(id);
        check((bool)updated["review"] && (bool)updated["attributes"]["description"]["lockedByUser"] &&
                  !(bool)updated["providerCurrent"] && db.countMatches("generated:true") == 0 &&
                  db.countMatches("review:true") == 1,
              "Source revision retains locks and marks previous receipt historical");
        e.action = "meta-review";
        e.items = {{newer}};
        e.id = "category";
        db.editMetadata(e);
        check(db.countMatches("review:true") == 0, "Review clears only acknowledged revision");
        check(SHA256(wav).toHexString() == bytes, "Metadata never rewrites the WAV");
    }
    check(media.moveFileTo(dir.getChildFile("offline")), "Disconnect fixture media");
    {
        Catalog reopened(file, true);
        check(reopened.describeMetadata(id)["fields"]["description"].toString() == "Wind tunnel",
              "Metadata survives offline reopen");
    }
    measure(dir, 100000);
    measure(dir, 500000);
    std::cout << "Catalog metadata development scenarios passed\n";
}
