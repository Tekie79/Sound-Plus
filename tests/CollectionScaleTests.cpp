#include "Catalog.h"
#include "RetrievalIndex.h"
#include "ModelPack.h"
#include "Performance.h"
#include <sys/resource.h>
#include <iostream>
#include <thread>
#include <algorithm>
namespace
{
void require(bool b, const char *s)
{
    if (!b)
        throw std::runtime_error(s);
}
juce::var object()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *k, juce::var v)
{
    o.getDynamicObject()->setProperty(k, v);
}
juce::var quantiles(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    auto o = object();
    put(o, "samples", (int)v.size());
    put(o, "p50Ms", v[(v.size() - 1) / 2]);
    put(o, "p95Ms", v[(size_t)std::ceil(.95 * v.size()) - 1]);
    put(o, "maximumMs", v.back());
    return o;
}
int64_t scalar(const juce::File &f, const char *sql)
{
    sqlite3 *d = nullptr;
    require(sqlite3_open_v2(f.getFullPathName().toRawUTF8(), &d, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Scale owned clone open");
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(d, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("Scale scalar ") + sql + ": " + sqlite3_errmsg(d));
    require(sqlite3_step(s) == SQLITE_ROW, "Scale scalar row");
    auto n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(d);
    return n;
}
} // namespace
void runCollectionScale(const juce::File &base, const juce::File &source)
{
    using namespace sp;
    require(source.existsAsFile(), "Existing scale fixture required");
    auto dir = base.getChildFile(juce::Uuid().toString().removeCharacters("-"));
    dir.createDirectory();
    auto destination = dir.getChildFile("library.sqlite");
    sqlite3 *in = nullptr, *out = nullptr;
    require(sqlite3_open_v2(source.getFullPathName().toRawUTF8(), &in, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "Scale source open");
    require(sqlite3_open(destination.getFullPathName().toRawUTF8(), &out) == SQLITE_OK, "Scale clone open");
    auto *backup = sqlite3_backup_init(out, "main", in, "main");
    require(backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE, "Isolated SQLite backup");
    sqlite3_backup_finish(backup);
    sqlite3_close(out);
    sqlite3_close(in);
    auto result = object();
    put(result, "source", source.getFullPathName());
    put(result, "clone", destination.getFullPathName());
    put(result, "scope",
        "Fresh isolated catalog copy; warm worker/database timings, no native-paint or inference "
        "equivalence");
    put(result, "assets", (juce::int64)scalar(destination, "SELECT count(*) FROM assets"));
    put(result, "sourceSchema", (int)scalar(destination, "PRAGMA user_version"));
    put(result, "sourceBytes", (juce::int64)destination.getSize());
    auto started = monotonicMs();
    Catalog db(destination);
    put(result, "migrationMs", monotonicMs() - started);
    db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    put(result, "migratedBytes", (juce::int64)destination.getSize());
    auto initial = db.indexState();
    auto rows = db.search("", {}, 512);
    require(!rows.empty(), "Scale has reference candidates");
    CollectionEdit create;
    create.action = "create";
    create.id = juce::Uuid().toString().removeCharacters("-");
    create.name = "Scope benchmark";
    db.editCollections(create);
    auto node = create.id;
    CollectionEdit add;
    add.action = "member";
    add.id = node;
    for (int i = 0; i < std::min(256, (int)rows.size()); ++i)
        if (rows[(size_t)i].state == "ready" && rows[(size_t)i].samples > 1)
            add.items.push_back({rows[(size_t)i], -1, -1});
    require(!add.items.empty(), "Ready scale members");
    started = monotonicMs();
    db.editCollections(add);
    put(result, "bulkMembers", (int)add.items.size());
    put(result, "bulkCommitMs", monotonicMs() - started);
    auto fav = add;
    fav.action = "favorite";
    fav.items.resize(std::min<size_t>(64, fav.items.size()));
    db.editCollections(fav);
    db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    put(result, "metadataBytes", (juce::int64)destination.getSize());
    juce::Array<juce::var> cases;
    for (auto scope : {juce::String(), node, juce::String("favorites")})
    {
        db.setScope(scope);
        std::vector<double> timings;
        for (int n = 0; n < 30; ++n)
        {
            auto query = n % 3 == 0   ? juce::String()
                         : n % 3 == 1 ? add.items[0].asset.name
                                      : add.items[0].asset.name.substring(0, 3);
            auto root = n % 2 == 0 ? juce::String() : add.items[0].asset.rootId;
            auto begin = monotonicMs();
            auto page = db.search(query, root, 20);
            auto count = db.countMatches(query, root);
            db.materialize(query, root);
            auto tail = db.page(std::max(0, count - 20), 20);
            require(page.size() <= 20 && tail.size() <= 20, "Bounded scale pages");
            timings.push_back(monotonicMs() - begin);
        }
        auto c = quantiles(timings);
        put(c, "scope", scope.isEmpty() ? "all" : scope == node ? "collection" : "favorites");
        put(c, "operation",
            "search20 + exact count + disk sequence + last page20; mix empty/exact/prefix and root "
            "intersection");
        cases.add(c);
    }
    put(result, "filename", juce::var(cases));
    auto regions = scalar(destination, "SELECT count(*) FROM embeddings");
    put(result, "regions", (juce::int64)regions);
    if (regions > 0)
    {
        RetrievalIndex index(destination);
        auto deadline = monotonicMs() + 180000;
        while (!index.stats().ready && monotonicMs() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(index.stats().ready, "Mapped retrieval index becomes ready");
        auto stats = index.stats();
        put(result, "indexBytes", (juce::int64)stats.bytes);
        put(result, "indexBuildMs", stats.buildMs);
        put(result, "indexReused", stats.reused);
        std::vector<float> query(512);
        query[0] = 1;
        juce::Array<juce::var> semantic;
        for (auto scope : {juce::String(), node, juce::String("favorites")})
        {
            db.setScope(scope);
            std::vector<double> timings;
            int vectors = 0;
            for (int n = 0; n < 20; ++n)
            {
                RetrievalMetrics m;
                auto begin = monotonicMs();
                auto hits = index.search(db, query, {}, {}, 100, &m, [] { return false; });
                auto expanded = db.expandScope(hits);
                vectors = m.vectors;
                timings.push_back(monotonicMs() - begin);
                if (scope.isNotEmpty())
                    for (auto &a : expanded)
                        require(a.libraryReference.isNotEmpty(), "Scoped scale hit has reference identity");
            }
            auto c = quantiles(timings);
            put(c, "scope", scope.isEmpty() ? "all" : scope == node ? "collection" : "favorites");
            put(c, "vectorsScored", vectors);
            put(c, "operation", "Mapped exact retrieval + batched reference expansion; no model inference");
            semantic.add(c);
        }
        put(result, "semantic", juce::var(semantic));
        require(db.indexState().sequence == initial.sequence,
                "Collection metadata leaves vector sequence unchanged");
    }
    require(db.indexState().catalog == initial.catalog, "Catalog identity preserved");
    struct rusage usage
    {
    };
    getrusage(RUSAGE_SELF, &usage);
    put(result, "processPeakRssBytes", (juce::int64)usage.ru_maxrss);
    auto file = dir.getChildFile("result.json");
    file.replaceWithText(juce::JSON::toString(result, true));
    base.getChildFile("latest.json").replaceWithText(juce::JSON::toString(result, true));
    std::cout << juce::JSON::toString(result, false) << "\n";
}
