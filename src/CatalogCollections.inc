// Included inside namespace sp, alongside Catalog's owned SQLite statement helper.
namespace
{
juce::String quoted(const juce::String &s)
{
    return "'" + s.replace("'", "''") + "'";
}
juce::var fields(std::initializer_list<std::pair<juce::Identifier, juce::var>> values)
{
    auto *o = new juce::DynamicObject();
    for (auto &[k, v] : values)
        o->setProperty(k, v);
    return juce::var(o);
}
juce::var referenceSnapshot(const Asset &a)
{
    return fields({{"id", a.id},
                   {"root", a.rootId},
                   {"relative", a.relative},
                   {"name", a.name},
                   {"path", a.path},
                   {"revision", (juce::int64)a.revision},
                   {"samples", (juce::int64)a.samples},
                   {"rate", a.sampleRate},
                   {"channels", a.channels},
                   {"duration", a.duration},
                   {"size", (juce::int64)a.size},
                   {"modified", (juce::int64)a.modified},
                   {"inode", (juce::int64)a.inode},
                   {"layout", layoutJson(a.detectedChannels)},
                   {"override", layoutJson(a.layoutOverride)},
                   {"confidence", a.layoutConfidence}});
}
Asset readReference(Statement &s)
{
    Asset a;
    if (s.str(0).isNotEmpty())
        a = readAsset(s);
    else
    {
        auto v = juce::JSON::parse(s.str(24));
        a.id = v["id"].toString();
        a.rootId = v["root"].toString();
        a.relative = v["relative"].toString();
        a.name = v["name"].toString();
        a.path = v["path"].toString();
        a.revision = (int64_t)v["revision"];
        a.samples = (int64_t)v["samples"];
        a.sampleRate = (double)v["rate"];
        a.channels = (int)v["channels"];
        a.duration = (double)v["duration"];
        a.size = (int64_t)v["size"];
        a.modified = (int64_t)v["modified"];
        a.inode = (int64_t)v["inode"];
        if (v["layout"]["id"].toString().isNotEmpty())
            a.detectedChannels = layoutFromJson(v["layout"]);
        if (v["override"]["id"].toString().isNotEmpty())
            a.layoutOverride = layoutFromJson(v["override"]);
        a.layoutConfidence = v["confidence"].toString();
        a.unresolved = true;
        a.online = false;
        a.state = "unresolved";
    }
    a.libraryReference = s.str(18);
    a.bookmarkFirst = s.integer(19);
    a.bookmarkLast = s.integer(20);
    a.favorite = s.integer(25) != 0;
    a.needsReview =
        a.bookmarkFirst >= 0 && (a.unresolved || a.revision != s.integer(21) || a.samples != s.integer(22) ||
                                 a.sampleRate != s.real(23) || a.bookmarkLast > a.samples);
    if (a.needsReview)
        a.error = juce::String::fromUTF8("Needs Review — source changed; select current audio and save a new region");
    else if (a.unresolved)
        a.error = juce::String::fromUTF8("Unresolved source reference — source record is missing");
    return a;
}
constexpr auto referenceColumns =
    ",lr.id,lr.first,lr.last,lr.revision,lr.samples,lr.rate,lr.snapshot,lr.favorite";
} // namespace
juce::String Catalog::referenceWhere(const juce::String &q, const juce::String &root)
{
    juce::String where;
    if (activeScope == "favorites")
        where = "lr.favorite=1";
    else if (activeScope == "recent")
        where = "lr.recent>0";
    else
        where =
            "EXISTS(SELECT 1 FROM collection_members cm WHERE cm.reference=lr.id AND cm.collection IN "
            "(WITH RECURSIVE tree(id) AS(SELECT id FROM collections WHERE id=" +
            quoted(activeScope) +
            " UNION ALL SELECT c.id FROM collections c JOIN tree t ON c.parent=t.id) SELECT id FROM tree))";
    if (root.isNotEmpty())
        where += " AND COALESCE(a.root_id,lr.root)=" + quoted(root);
    if (metadataSchema && isMetadataQuery(q))
        where += " AND " + metadataWhere(q, {});
    else if (q.trim().isNotEmpty())
    {
        if (uuidQuery(q))
            where += " AND lr.asset=" + quoted(q);
        else
            where +=
                " AND (a.rowid IN(SELECT rowid FROM names WHERE names MATCH " + quoted(matchText(q, {})) +
                ")" +
                (metadataSchema ? " OR a.rowid IN(SELECT rowid FROM metadata_fts WHERE metadata_fts MATCH " +
                                      quoted(matchText(q, {})) + ")"
                                : juce::String{}) +
                " OR (a.id IS NULL AND lower(lr.name) LIKE " +
                quoted("%" + q.toLowerCase().replace("%", "\\%").replace("_", "\\_") + "%") +
                " ESCAPE '\\'))";
    }
    return where;
}
std::vector<Asset> Catalog::scopedRows(const juce::String &q, const juce::String &root, int limit, int offset)
{
    Statement s(db, ("SELECT " + juce::String(columns) + referenceColumns +
                     " FROM library_refs lr LEFT JOIN assets a ON a.id=lr.asset LEFT JOIN roots r ON "
                     "r.id=a.root_id WHERE " +
                     referenceWhere(q, root) +
                     (activeScope == "recent"
                          ? " ORDER BY " +
                                (q.trim().isEmpty() ? juce::String()
                                                    : "CASE WHEN COALESCE(a.name,lr.name)=" + quoted(q) +
                                                          " COLLATE NOCASE THEN 0 ELSE 1 END,") +
                                "lr.recent DESC,lr.id"
                          : " ORDER BY " +
                                (q.trim().isEmpty() ? juce::String()
                                                    : "CASE WHEN COALESCE(a.name,lr.name)=" + quoted(q) +
                                                          " COLLATE NOCASE THEN 0 ELSE 1 END,") +
                                "COALESCE(a.name,lr.name) COLLATE NOCASE,lr.asset,lr.first,lr.id") +
                     " LIMIT ? OFFSET ?")
                        .toRawUTF8());
    s.bind(1, (int64_t)juce::jlimit(1, 512, limit));
    s.bind(2, (int64_t)std::max(0, offset));
    std::vector<Asset> result;
    while (s.step())
        result.push_back(readReference(s));
    return result;
}
void Catalog::scopedMaterialize(const juce::String &q, const juce::String &root, bool empty)
{
    exec("DROP TABLE IF EXISTS temp.browse_refs;CREATE TEMP TABLE browse_refs(position INTEGER PRIMARY "
         "KEY,reference TEXT NOT NULL UNIQUE)");
    if (!empty)
        exec(("INSERT INTO browse_refs(reference) SELECT lr.id FROM library_refs lr LEFT JOIN assets a ON "
              "a.id=lr.asset WHERE " +
              referenceWhere(q, root) +
              (activeScope == "recent"
                   ? " ORDER BY " +
                         (q.trim().isEmpty() ? juce::String()
                                             : "CASE WHEN COALESCE(a.name,lr.name)=" + quoted(q) +
                                                   " COLLATE NOCASE THEN 0 ELSE 1 END,") +
                         "lr.recent DESC,lr.id"
                   : " ORDER BY " +
                         (q.trim().isEmpty() ? juce::String()
                                             : "CASE WHEN COALESCE(a.name,lr.name)=" + quoted(q) +
                                                   " COLLATE NOCASE THEN 0 ELSE 1 END,") +
                         "COALESCE(a.name,lr.name) COLLATE NOCASE,lr.asset,lr.first,lr.id"))
                 .toRawUTF8());
}
Asset Catalog::referenceAsset(const juce::String &id)
{
    Statement s(db, ("SELECT " + juce::String(columns) + referenceColumns +
                     " FROM library_refs lr LEFT JOIN assets a ON a.id=lr.asset LEFT JOIN roots r ON "
                     "r.id=a.root_id WHERE lr.id=?")
                        .toRawUTF8());
    s.bind(1, id);
    return s.step() ? readReference(s) : Asset{};
}
ScopeRanges Catalog::scopeRanges(const juce::String &root)
{
    ScopeRanges ranges;
    if (activeScope.isEmpty())
    {
        if (semanticFilter.isNotEmpty())
        {
            Statement s(
                db, ("SELECT a.id FROM assets a WHERE " + metadataWhere(semanticFilter, root)).toRawUTF8());
            while (s.step())
                ranges[s.str(0)].push_back({0, INT64_MAX, 0, 1});
        }
        return ranges;
    }
    Statement s(
        db, ("SELECT lr.asset,lr.first,lr.last FROM library_refs lr JOIN assets a ON a.id=lr.asset WHERE " +
             referenceWhere(semanticFilter, root) +
             " AND a.state='ready' AND (lr.first<0 OR (lr.revision=a.revision AND lr.samples=a.samples AND "
             "lr.rate=a.sample_rate AND lr.last<=a.samples)) ORDER BY lr.asset,lr.first")
                .toRawUTF8());
    while (s.step())
        ranges[s.str(0)].push_back(
            {s.integer(1) < 0 ? 0 : s.integer(1), s.integer(1) < 0 ? INT64_MAX : s.integer(2), 0, 1});
    return ranges;
}
std::vector<Asset> Catalog::expandScope(const std::vector<Asset> &hits)
{
    if (activeScope.isEmpty())
        return hits;
    // One batched reference query for the ranked prefix, rather than one query per result.
    juce::StringArray ids;
    for (auto &a : hits)
        ids.add(quoted(a.id));
    if (ids.isEmpty())
        return {};
    Statement s(
        db, ("SELECT " + juce::String(columns) + referenceColumns +
             " FROM library_refs lr JOIN assets a ON a.id=lr.asset JOIN roots r ON r.id=a.root_id WHERE " +
             referenceWhere({}, {}) + " AND lr.asset IN(" + ids.joinIntoString(",") + ") ORDER BY lr.id")
                .toRawUTF8());
    std::map<juce::String, std::vector<Asset>> refs;
    while (s.step())
    {
        auto a = readReference(s);
        if (a.deliverable())
            refs[a.id].push_back(a);
    }
    std::vector<Asset> result;
    for (auto &hit : hits)
        for (auto a : refs[hit.id])
        {
            for (auto m : hit.matches)
            {
                if (a.bookmarkFirst >= 0)
                {
                    m.startSample = std::max(m.startSample, a.bookmarkFirst);
                    m.endSample = std::min(m.endSample, a.bookmarkLast);
                }
                if (m.endSample > m.startSample)
                    a.matches.push_back(m);
            }
            if (!a.matches.empty())
            {
                a.matchReason = hit.matchReason;
                result.push_back(std::move(a));
            }
        }
    return result;
}
bool Catalog::bookmarkFavorite(const BookmarkRequest &item)
{
    if (item.asset.libraryReference.isNotEmpty() && item.first == item.asset.bookmarkFirst &&
        item.last == item.asset.bookmarkLast)
    {
        Statement s(db, "SELECT favorite FROM library_refs WHERE id=?");
        s.bind(1, item.asset.libraryReference);
        return s.step() && s.integer(0) != 0;
    }
    Statement s(db, "SELECT favorite FROM library_refs WHERE asset=? AND first=? AND last=? AND revision=?");
    s.bind(1, item.asset.id);
    s.bind(2, item.first);
    s.bind(3, item.last);
    s.bind(4, item.first < 0 ? 0 : item.asset.revision);
    return s.step() && s.integer(0) != 0;
}
CollectionState Catalog::collections()
{
    CollectionState state;
    Statement s(db, "SELECT c.id,c.parent,c.name,c.position,count(m.reference) FROM collections c LEFT JOIN "
                    "collection_members m ON m.collection=c.id GROUP BY c.id ORDER BY c.position,c.id");
    while (s.step())
        state.nodes.push_back({s.str(0), s.str(1), s.str(2), (int)s.integer(3), (int)s.integer(4)});
    Statement counts(db, "SELECT (SELECT count(*) FROM library_refs WHERE favorite=1),(SELECT count(*) FROM "
                         "library_refs WHERE recent>0),EXISTS(SELECT 1 FROM collection_history WHERE "
                         "sequence<=(SELECT value FROM collection_cursor)),EXISTS(SELECT 1 FROM "
                         "collection_history WHERE sequence>(SELECT value FROM collection_cursor))");
    counts.step();
    state.favorites = (int)counts.integer(0);
    state.recent = (int)counts.integer(1);
    state.canUndo = counts.integer(2) != 0;
    state.canRedo = counts.integer(3) != 0;
    return state;
}
juce::String Catalog::ensureReference(const BookmarkRequest &item)
{
    auto a = asset(item.asset.id);
    if (a.id.isEmpty())
    {
        if (item.asset.libraryReference.isNotEmpty())
            return item.asset.libraryReference;
        throw std::runtime_error("Source record disappeared; refresh the selection");
    }
    const bool region = item.first >= 0;
    if (region && (a.revision != item.asset.revision || a.samples != item.asset.samples ||
                   a.sampleRate != item.asset.sampleRate || item.first < 0 || item.last <= item.first ||
                   item.last > a.samples))
        throw std::runtime_error("Needs Review: source or region changed");
    auto first = region ? item.first : -1, last = region ? item.last : -1, revision = region ? a.revision : 0;
    Statement find(db, "SELECT id FROM library_refs WHERE asset=? AND first=? AND last=? AND revision=?");
    find.bind(1, a.id);
    find.bind(2, first);
    find.bind(3, last);
    find.bind(4, revision);
    if (find.step())
        return find.str(0);
    auto id = juce::Uuid().toString().removeCharacters("-");
    Statement add(
        db, "INSERT INTO library_refs(id,asset,root,name,first,last,revision,samples,rate,snapshot,catalog) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    add.bind(1, id);
    add.bind(2, a.id);
    add.bind(3, a.rootId);
    add.bind(4, a.name);
    add.bind(5, first);
    add.bind(6, last);
    add.bind(7, revision);
    add.bind(8, a.samples);
    sqlite3_bind_double(add.s, 9, a.sampleRate);
    add.bind(10, juce::JSON::toString(referenceSnapshot(a), true));
    add.bind(11, indexState().catalog);
    add.step();
    return id;
}
void Catalog::applyCollectionPatch(const juce::var &patch)
{
    if (!patch.isArray())
        throw std::runtime_error("Invalid collection history patch");
    for (auto &v : *patch.getArray())
    {
        auto kind = v["kind"].toString();
        if (kind == "collection" && (bool)v["deleted"])
        {
            Statement s(db, "DELETE FROM collections WHERE id=?");
            s.bind(1, v["id"].toString());
            s.step();
        }
        else if (kind == "collection")
        {
            Statement s(db, "INSERT INTO collections(id,parent,name,folded,position) VALUES(?,?,?,?,?) ON "
                            "CONFLICT(id) DO UPDATE SET "
                            "parent=excluded.parent,name=excluded.name,folded=excluded.folded,position="
                            "excluded.position");
            s.bind(1, v["id"].toString());
            s.bind(2, v["parent"].toString());
            s.bind(3, v["name"].toString());
            s.bind(4, v["name"].toString().toLowerCase());
            s.bind(5, (int64_t)v["position"]);
            s.step();
        }
        else if (kind == "member")
        {
            Statement s(db, (bool)v["present"]
                                ? "INSERT OR IGNORE INTO collection_members(collection,reference) VALUES(?,?)"
                                : "DELETE FROM collection_members WHERE collection=? AND reference=?");
            s.bind(1, v["id"].toString());
            s.bind(2, v["ref"].toString());
            s.step();
        }
        else if (kind == "favorite")
        {
            Statement s(db, "UPDATE library_refs SET favorite=? WHERE id=?");
            s.bind(1, (int64_t)(bool)v["present"]);
            s.bind(2, v["ref"].toString());
            s.step();
        }
    }
}
void Catalog::editCollections(const CollectionEdit &edit)
{
    if (edit.items.size() > 4096)
        throw std::runtime_error("Choose at most 4096 items per metadata action");
    begin();
    try
    {
        if (edit.action == "undo" || edit.action == "redo")
        {
            bool undo = edit.action == "undo";
            Statement s(db, undo ? "SELECT sequence,inverse FROM collection_history WHERE sequence<=(SELECT "
                                   "value FROM collection_cursor) ORDER BY sequence DESC LIMIT 1"
                                 : "SELECT sequence,forward FROM collection_history WHERE sequence>(SELECT "
                                   "value FROM collection_cursor) ORDER BY sequence LIMIT 1");
            if (s.step())
            {
                auto seq = s.integer(0);
                auto patch = juce::JSON::parse(s.str(1));
                applyCollectionPatch(patch);
                if (undo)
                {
                    Statement c(db, "UPDATE collection_cursor SET value=COALESCE((SELECT max(sequence) FROM "
                                    "collection_history WHERE sequence<?),0)");
                    c.bind(1, seq);
                    c.step();
                }
                else
                {
                    Statement c(db, "UPDATE collection_cursor SET value=?");
                    c.bind(1, seq);
                    c.step();
                }
            }
            commit();
            return;
        }
        juce::Array<juce::var> before, after;
        auto state = collections();
        auto find = [&](const juce::String &id) -> LibraryCollection
        {
            for (auto &c : state.nodes)
                if (c.id == id)
                    return c;
            throw std::runtime_error("Collection no longer exists");
        };
        auto row = [](const LibraryCollection &c, bool deleted = false)
        {
            return fields({{"kind", "collection"},
                           {"id", c.id},
                           {"parent", c.parent},
                           {"name", c.name},
                           {"position", c.position},
                           {"deleted", deleted}});
        };
        const auto referenceFor = [&](const BookmarkRequest &item)
        {
            return item.asset.libraryReference.isNotEmpty() && item.first == item.asset.bookmarkFirst &&
                           item.last == item.asset.bookmarkLast
                       ? item.asset.libraryReference : ensureReference(item);
        };
        if (edit.action == "create" || edit.action == "rename" || edit.action == "move")
        {
            auto c = edit.action == "create"
                         ? LibraryCollection{edit.id.isEmpty() ? juce::Uuid().toString().removeCharacters("-")
                                                               : edit.id,
                                             edit.parent, edit.name.trim(), edit.position, 0}
                         : find(edit.id);
            auto old = c;
            if (edit.action == "create" && std::any_of(state.nodes.begin(), state.nodes.end(),
                                                       [&](const auto &n) { return n.id == c.id; }))
                throw std::runtime_error("Collection identity already exists");
            if (edit.action == "rename")
                c.name = edit.name.trim();
            if (edit.action == "move")
            {
                c.parent = edit.parent;
                c.position = edit.position;
            }
            if (c.name.isEmpty() || c.name.length() > 96)
                throw std::runtime_error("Use a collection name of 1–96 characters");
            if (edit.action == "create" && state.nodes.size() >= 1024)
                throw std::runtime_error("Collection limit is 1024");
            for (auto &n : state.nodes)
                if (n.id != c.id && n.parent == c.parent && n.name.equalsIgnoreCase(c.name))
                    throw std::runtime_error("A sibling collection already uses that name");
            std::map<juce::String, juce::String> parents;
            for (auto &n : state.nodes)
                parents[n.id] = n.parent;
            if (c.parent.isNotEmpty() && !parents.contains(c.parent))
                throw std::runtime_error("Parent collection is missing");
            parents[c.id] = c.parent;
            for (auto &[id, parent] : parents)
            {
                auto cursor = id;
                std::set<juce::String> seen;
                while (cursor.isNotEmpty())
                {
                    if (!seen.insert(cursor).second)
                        throw std::runtime_error("Collections cannot contain a cycle");
                    if (seen.size() > 16)
                        throw std::runtime_error("Collection nesting limit is 16");
                    cursor = parents.at(cursor);
                }
            }
            before.add(row(old, edit.action == "create"));
            std::vector<LibraryCollection> siblings;
            for (auto &n : state.nodes)
                if (n.parent == c.parent && n.id != c.id)
                    siblings.push_back(n);
            auto at = juce::jlimit(0, (int)siblings.size(), c.position);
            siblings.insert(siblings.begin() + at, c);
            for (size_t i = 0; i < siblings.size(); ++i)
            {
                auto n = siblings[i];
                if (n.id == c.id)
                {
                    n.position = (int)i;
                    after.add(row(n));
                }
                else if (n.position != (int)i)
                {
                    before.add(row(n));
                    n.position = (int)i;
                    after.add(row(n));
                }
            }
            // Creating a destination and adding its selection is one history operation.
            if (edit.action == "create")
                for (const auto &item : edit.items)
                {
                    auto ref = referenceFor(item);
                    after.add(fields({{"kind", "member"}, {"id", c.id}, {"ref", ref}, {"present", true}}));
                    before.insert(0, fields({{"kind", "member"}, {"id", c.id}, {"ref", ref}, {"present", false}}));
                }
        }
        else if (edit.action == "delete")
        {
            find(edit.id);
            std::set<juce::String> removed{edit.id};
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (auto &c : state.nodes)
                    if (removed.contains(c.parent) && removed.insert(c.id).second)
                        changed = true;
            }
            for (auto &c : state.nodes)
                if (removed.contains(c.id))
                {
                    before.add(row(c));
                    after.add(row(c, true));
                }
            Statement members(db, ("SELECT collection,reference FROM collection_members WHERE collection IN "
                                   "(WITH RECURSIVE tree(id) AS(SELECT " +
                                   quoted(edit.id) +
                                   " UNION ALL SELECT c.id FROM collections c JOIN tree t ON c.parent=t.id) "
                                   "SELECT id FROM tree)")
                                      .toRawUTF8());
            int count = 0;
            while (members.step())
            {
                if (++count > 4096)
                    throw std::runtime_error(
                        "Remove members in batches before deleting this large collection");
                before.add(fields({{"kind", "member"},
                                   {"id", members.str(0)},
                                   {"ref", members.str(1)},
                                   {"present", true}}));
                after.add(fields({{"kind", "member"},
                                  {"id", members.str(0)},
                                  {"ref", members.str(1)},
                                  {"present", false}}));
            }
        }
        else if (edit.action == "member" || edit.action == "favorite" || edit.action == "used")
        {
            if (edit.action == "member")
                find(edit.id);
            for (auto &item : edit.items)
            {
                auto ref = referenceFor(item);
                if (edit.action == "used")
                {
                    Statement used(db, "UPDATE library_refs SET recent=? WHERE id=?");
                    used.bind(1, (int64_t)juce::Time::currentTimeMillis());
                    used.bind(2, ref);
                    used.step();
                    continue;
                }
                Statement old(db, edit.action == "favorite"
                                      ? "SELECT favorite FROM library_refs WHERE id=?"
                                      : "SELECT EXISTS(SELECT 1 FROM collection_members WHERE reference=? "
                                        "AND collection=?)");
                old.bind(1, ref);
                if (edit.action == "member")
                    old.bind(2, edit.id);
                old.step();
                auto b = fields({{"kind", edit.action == "member" ? "member" : "favorite"},
                                 {"id", edit.id},
                                 {"ref", ref},
                                 {"present", old.integer(0) != 0}});
                auto a = b.clone();
                a.getDynamicObject()->setProperty("present", edit.enabled);
                before.add(b);
                after.add(a);
            }
        }
        else
            throw std::runtime_error("Unknown collection action");
        if (!after.isEmpty())
        {
            auto forward = juce::JSON::toString(juce::var(after), true),
                 inverse = juce::JSON::toString(juce::var(before), true);
            if (forward.getNumBytesAsUTF8() + inverse.getNumBytesAsUTF8() > 8 * 1024 * 1024)
                throw std::runtime_error("Metadata action exceeds the 8 MiB history bound");
            applyCollectionPatch(juce::var(after));
            exec("DELETE FROM collection_history WHERE sequence>(SELECT value FROM collection_cursor)");
            Statement h(db, "INSERT INTO collection_history(forward,inverse) VALUES(?,?)");
            h.bind(1, forward);
            h.bind(2, inverse);
            h.step();
            exec("UPDATE collection_cursor SET value=last_insert_rowid();DELETE FROM collection_history "
                 "WHERE sequence<(SELECT max(sequence)-99 FROM collection_history)");
            // 100 actions of <=8 MiB each; additionally cap aggregate history at64 MiB.
            for (;;)
            {
                Statement size(
                    db, "SELECT COALESCE(sum(length(forward)+length(inverse)),0) FROM collection_history");
                size.step();
                if (size.integer(0) <= 64 * 1024 * 1024)
                    break;
                exec("DELETE FROM collection_history WHERE sequence=(SELECT min(sequence) FROM "
                     "collection_history)");
            }
        }
        commit();
    }
    catch (...)
    {
        rollback();
        throw;
    }
}
