#include "Catalog.h"
#include "LibraryView.h"
#include "SourcePoolView.h"
#include "MultitrackEditor.h"
#include <deque>
#include <juce_cryptography/juce_cryptography.h>
#include "LibraryService.h"
#include "DesignModel.h"
#include "RetrievalIndex.h"
#include "ModelPack.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <thread>
#include <set>
namespace
{
void check(bool b, const char *s)
{
    if (!b)
        throw std::runtime_error(s);
}
template <class F> void wait(F f, const char *s)
{
    for (int i = 0; i < 2000; ++i)
    {
        if (f())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error(s);
}
sp::CollectionEdit action(const char *kind, juce::String id = {})
{
    sp::CollectionEdit e;
    e.action = kind;
    e.id = id;
    return e;
}
} // namespace
#include "CollectionViewTests.inc"
void runCollectionTests(const juce::File &base, const juce::File &app)
{
    juce::ScopedJuceInitialiser_GUI gui;
    using namespace sp;
    auto dir = base.getChildFile(juce::Uuid().toString().removeCharacters("-"));
    dir.createDirectory();
    auto media = dir.getChildFile("media"), file = media.getChildFile("Rain.wav");
    media.createDirectory();
    {
        juce::WavAudioFormat wav;
        auto stream = file.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> w(
            wav.createWriterFor(stream.release(), 48000, 1, 16, {}, 0));
        juce::AudioBuffer<float> b(1, 48000);
        for (int i = 0; i < 48000; ++i)
            b.setSample(0, i, .1f * std::sin(i * .07f));
        check(w && w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()), "Generated collection fixture");
    }
    auto sourceHash = juce::SHA256(file).toHexString();
    auto database = dir.getChildFile("library.sqlite");
    Asset a;
    Root root;
    IndexState identity;
    juce::String outsideId;
    {
        Catalog db(database);
        root = db.addRootRecord(media);
        FileStamp stamp;
        check(fileStamp(file, stamp), "Fixture stamp");
        db.discover(root, "Rain.wav", stamp, 1);
        a = db.search("")[0];
        db.metadata(a, 1, 1, 48000, {}, 48000);
        a = db.asset(a.id);
        db.seedAnalysis(modelId(), preprocessingId());
        AnalysisUpdate u;
        u.asset = a;
        u.model = modelId();
        u.preprocessing = preprocessingId();
        u.start = 0;
        u.end = 48000;
        u.nextStart = 48000;
        u.done = true;
        u.embedding.resize(512);
        u.embedding[0] = .5f;
        u.embedding[1] = std::sqrt(.75f);
        check(db.commitAnalysis(u), "Fixture embedding");
        auto outside = dir.getChildFile("outside");
        outside.createDirectory();
        auto outsideFile = outside.getChildFile("Outside.wav");
        check(file.copyFileTo(outsideFile), "Owned second-root fixture");
        auto outsideRoot = db.addRootRecord(outside);
        FileStamp outsideStamp;
        check(fileStamp(outsideFile, outsideStamp), "Outside stamp");
        db.discover(outsideRoot, "Outside.wav", outsideStamp, 1);
        auto b = db.existing(outsideRoot.id, "Outside.wav");
        db.metadata(b, 1, 1, 48000, {}, 48000);
        b = db.asset(b.id);
        outsideId = b.id;
        db.seedAnalysis(modelId(), preprocessingId());
        u.asset = b;
        u.embedding[0] = 1;
        u.embedding[1] = 0;
        check(db.commitAnalysis(u), "Stronger out-of-scope embedding");
        identity = db.indexState();
        db.exec("DROP TABLE collection_members;DROP TABLE collection_history;DROP TABLE "
                "collection_cursor;DROP TABLE library_refs;DROP TABLE collections;PRAGMA user_version=6");
    }
    bool failed = false;
    try
    {
        Catalog interrupted(database, false,
                            [] { throw std::runtime_error("controlled migration interruption"); });
    }
    catch (...)
    {
        failed = true;
    }
    check(failed, "Interrupted actual v7 migration rejects incomplete catalog");
    {
        sqlite3 *sql = nullptr;
        sqlite3_open(database.getFullPathName().toRawUTF8(), &sql);
        sqlite3_stmt *s = nullptr;
        sqlite3_prepare_v2(sql, "PRAGMA user_version", -1, &s, nullptr);
        sqlite3_step(s);
        check(sqlite3_column_int(s, 0) == 6, "Interrupted migration retains v6");
        sqlite3_finalize(s);
        sqlite3_prepare_v2(sql, "SELECT count(*) FROM sqlite_master WHERE name='collections'", -1, &s,
                           nullptr);
        sqlite3_step(s);
        check(sqlite3_column_int(s, 0) == 0, "Migration tables roll back");
        sqlite3_finalize(s);
        sqlite3_close(sql);
    }
    auto parent = juce::Uuid().toString().removeCharacters("-"),
         child = juce::Uuid().toString().removeCharacters("-"),
         other = juce::Uuid().toString().removeCharacters("-");
    juce::String regionRef;
    {
        Catalog db(database);
        check(db.asset(a.id).samples == 48000 && db.indexState().catalog == identity.catalog &&
                  db.indexState().sequence == identity.sequence,
              "v6 migration preserves sources and vector sequence");
        auto create = action("create", parent);
        create.name = "Weather";
        db.editCollections(create);
        create.id = child;
        create.parent = parent;
        create.name = "Rain";
        db.editCollections(create);
        create.id = other;
        create.parent = {};
        create.name = "Other";
        create.position = 1;
        db.editCollections(create);
        auto reject = [&](CollectionEdit e, const char *s)
        {
            bool thrown = false;
            try
            {
                db.editCollections(e);
            }
            catch (...)
            {
                thrown = true;
            }
            check(thrown, s);
        };
        auto duplicate = create;
        duplicate.id = juce::Uuid().toString().removeCharacters("-");
        duplicate.name = " weather ";
        reject(duplicate, "Duplicate sibling name is rejected transactionally");
        auto move = action("move", parent);
        move.parent = child;
        reject(move, "Cycle is rejected");
        // Depth and bulk boundaries exercise the same transactional command path.
        auto branch = juce::Uuid().toString().removeCharacters("-");
        auto deep = action("create", branch);
        deep.name = "Depth";
        db.editCollections(deep);
        auto parentAtDepth = branch;
        for (int level = 2; level <= 16; ++level)
        {
            deep.id = juce::Uuid().toString().removeCharacters("-");
            deep.parent = parentAtDepth;
            deep.name = "Level " + juce::String(level);
            db.editCollections(deep);
            parentAtDepth = deep.id;
        }
        deep.id = juce::Uuid().toString().removeCharacters("-");
        deep.parent = parentAtDepth;
        reject(deep, "Depth17 rejected atomically");
        auto tooDeep = action("move", parent);
        tooDeep.parent = parentAtDepth;
        reject(tooDeep, "Reparent includes descendant depths");
        db.editCollections(action("delete", branch));
        check(db.collections().nodes.size() == 3, "Bounded deep subtree deletion");
        auto reorder = action("move", other);
        reorder.position = 0;
        db.editCollections(reorder);
        auto ordered = db.collections().nodes;
        check(
            std::find_if(ordered.begin(), ordered.end(), [&](auto &c) { return c.id == other; })->position ==
                0,
            "Explicit sibling order");
        auto bulk = action("favorite");
        for (int n = 0; n < 256; ++n)
            bulk.items.push_back({a, n * 10, n * 10 + 50});
        db.editCollections(bulk);
        check(db.collections().favorites == 256, "Atomic bulk Favorite");
        db.editCollections(action("undo"));
        check(db.collections().favorites == 0, "Bulk metadata Undo");
        db.editCollections(action("redo"));
        check(db.collections().favorites == 256, "Bulk metadata Redo");
        db.editCollections(action("undo"));
        auto oversized = bulk;
        oversized.items.resize(4097, {a, 0, 50});
        reject(oversized, "Oversized operation rejected before mutation");
        auto add = action("member", child);
        add.items = {{a, -1, -1}, {a, 1000, 9000}, {a, 12000, 18000}};
        db.editCollections(add);
        add.id = other;
        db.editCollections(add);
        auto favorite = action("favorite");
        favorite.items = {{a, 1000, 9000}};
        db.editCollections(favorite);
        db.setScope(parent);
        check(db.countMatches("") == 3 && db.countMatches("   ") == 3 &&
                  db.countMatches("Rain", root.id) == 3 && db.countMatches("", "absent") == 0,
              "Descendant/root/FTS scope intersection");
        auto rows = db.search("");
        check(rows.size() == 3 && rows[0].id == a.id && rows[0].resultKey() != rows[1].resultKey(),
              "Full source and two original regions keep distinct reference IDs");
        for (auto &r : rows)
            if (r.bookmarkFirst == 1000)
            {
                regionRef = r.libraryReference;
                check(r.favorite, "Favorite region state");
            }
        db.materialize("", root.id);
        check(db.page(0, 2).size() == 2 && db.page(2, 2).size() == 1, "Bounded reference pagination");
        db.setScope("favorites");
        check(db.countMatches("") == 1, "Favorites independently scope exact regions");
        auto saved = db.search("")[0];
        check(saved.bookmarkFirst == 1000 && saved.bookmarkLast == 9000 && saved.deliverable(),
              "Half-open sample bookmark");
        DesignDocument beforeMetadata;
        beforeMetadata.original.layers = {makeDesignLayer(a, 1000, 9000)};
        auto afterMetadata = beforeMetadata;
        afterMetadata.original.layers[0].source = saved;
        check(juce::JSON::toString(designToJson(beforeMetadata), true) ==
                  juce::JSON::toString(designToJson(afterMetadata), true),
              "Collection/Favorite/region annotations do not change serialized design or recipe identity");
        auto layer = makeDesignLayer(saved, saved.bookmarkFirst, saved.bookmarkLast);
        check(layer.source.id == a.id && layer.region.startSample == 1000 && layer.region.endSample == 9000,
              "Design transfer preserves original identity and exact samples");
        std::vector<float> query(512);
        query[0] = 1;
        RetrievalMetrics metrics;
        auto hits = db.semanticSearch(query, modelId(), preprocessingId(), root.id, 100, &metrics);
        auto expanded = db.expandScope(hits);
        check(expanded.size() == 1 && expanded[0].matches[0].startSample == 1000 &&
                  expanded[0].matches[0].endSample == 9000,
              "Semantic eligibility and moments clip to saved region before ranking");
        db.setScope({});
        auto global = db.semanticSearch(query, modelId(), preprocessingId(), {}, 1);
        check(global.size() == 1 && global[0].id == outsideId, "Independent stronger global result");
        db.setScope("favorites");
        auto scoped = db.semanticSearch(query, modelId(), preprocessingId(), {}, 1);
        check(scoped.size() == 1 && scoped[0].id == a.id, "SQLite scope filters before top-one ranking");
        {
            RetrievalIndex index(database);
            wait([&] { return index.stats().ready; }, "Mapped fixture ready");
            Catalog reader(database, true);
            reader.setScope("favorites");
            auto mapped = index.search(reader, query, {}, {}, 1, nullptr, [] { return false; });
            check(mapped.size() == 1 && mapped[0].id == a.id, "Mapped scope filters before top-one ranking");
        }
        auto rename = action("rename", child);
        rename.name = "Drizzle";
        db.editCollections(rename);
        check(db.collections().nodes.size() == 3, "Rename keeps hierarchy");
        db.editCollections(action("undo"));
        db.setScope(child);
        check(db.countMatches("") == 3, "Undo rename retains memberships");
        db.editCollections(action("redo"));
        check(db.countMatches("") == 3, "Redo rename retains memberships");
        auto erase = action("delete", parent);
        db.editCollections(erase);
        check(db.collections().nodes.size() == 1 && db.asset(a.id).id == a.id, "Delete subtree is virtual");
        db.editCollections(action("undo"));
        db.setScope(parent);
        check(db.countMatches("") == 3, "Undo restores nested memberships");
        auto used = action("used");
        used.items = {{saved, 1000, 9000}};
        db.editCollections(used);
        db.setScope("recent");
        check(db.countMatches("") == 1, "Recent tracks actual reference use");
        root.online = false;
        db.rootState(root);
        db.setScope(child);
        check(!db.search("")[0].online, "Offline members retained");
        root.online = true;
        db.rootState(root);
        check(db.search("")[0].online, "Remount restores access");
        check(db.indexState().sequence == identity.sequence, "Metadata does not invalidate embeddings/index");
        // Stable-ID path relink retains exact coordinates; actual content revision invalidates regions.
        auto renamed = media.getChildFile("Renamed.wav");
        check(file.moveFileTo(renamed), "Rename fixture");
        FileStamp moved;
        check(fileStamp(renamed, moved), "Relink stamp");
        db.discover(root, "Renamed.wav", moved, 2, true, "Rain.wav");
        check(db.asset(a.id).id == a.id && db.referenceAsset(regionRef).deliverable(),
              "Stable-ID relink keeps region provenance");
        db.exec(("UPDATE assets SET revision=revision+1 WHERE id='" + a.id + "'").toRawUTF8());
        check(db.referenceAsset(regionRef).needsReview, "Changed content requires region review");
        db.setScope("favorites");
        check(db.scopeRanges().empty(), "Stale regions excluded from semantic scoring");
        db.exec(("DELETE FROM assets WHERE id='" + a.id + "'").toRawUTF8());
        auto unresolved = db.referenceAsset(regionRef);
        check(unresolved.unresolved && unresolved.needsReview && !unresolved.online,
              "Missing record remains explicit unresolved reference");
        check(db.countMatches("") == 1, "Unresolved Favorites remain visible");
        check(juce::SHA256(renamed).toHexString() == sourceHash,
              "Metadata and relink never modify original audio");
    }
    {
        Catalog db(database);
        db.setScope("favorites");
        auto rows = db.search("");
        check(rows.size() == 1 && rows[0].libraryReference == regionRef && rows[0].unresolved,
              "Reopen retains unresolved stable reference");
        db.editCollections(action("undo"));
    }
    {
        LibraryService service(database);
        service.pause(true);
        wait([&] { return service.ready() && service.snapshot().progress.paused; },
             "Paused service initializes");
        auto edit = action("create");
        edit.name = "Paused metadata";
        check(service.editCollections(edit), "Queue metadata while paused");
        wait(
            [&]
            {
                auto s = service.snapshot();
                return s.organization.nodes.size() == 4;
            },
            "Writer handles metadata independently of indexing pause");
        auto id = service.search("", {}, "favorites");
        wait([&] { return service.snapshot().request == id; }, "Scoped service query while paused");
        check(service.snapshot().total == 1, "Unresolved Favorites usable offline/paused");
    }
    CollectionViewTestAccess::run(dir, app, media);
    std::cout << "PASS transactional migration, hierarchy/history, region/scoped retrieval, "
                 "offline/relink/review and paused writer\n";
}
