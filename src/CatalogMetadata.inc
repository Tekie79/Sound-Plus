// Catalog private implementation. Called on a worker-owned SQLite connection.
namespace
{
using juce::String;
using juce::var;
var metaObject()
{
    return var(new juce::DynamicObject);
}
String metaJson(const var &v)
{
    return juce::JSON::toString(v, true);
}
void metaRequire(bool ok, const String &why)
{
    if (!ok)
        throw std::runtime_error(why.toStdString());
}
void setMeta(var &v, const String &k, const var &x)
{
    v.getDynamicObject()->setProperty(k, x);
}
void ensureMeta(sqlite3 *db, const String &id)
{
    Statement insert(db, "INSERT OR IGNORE INTO metadata_assets(id) VALUES(?)");
    insert.bind(1, id);
    insert.step();
}
String mediaDigest(const Asset &a)
{
    FileStamp stamp;
    juce::File file(a.path);
    auto exact = [&]
    {
        return fileStamp(file, stamp) && stamp.size == a.size && stamp.modified == a.modified &&
               stamp.inode == a.inode;
    };
    metaRequire(exact(), "Online, unchanged media required for manifest exchange");
    auto in = file.createInputStream();
    metaRequire(in != nullptr, "Cannot open source");
    auto hash = juce::SHA256(*in).toHexString();
    metaRequire(exact(), "Source changed while verifying manifest");
    return hash;
}
} // namespace
juce::var Catalog::describeMetadata(const String &id)
{
    auto result = metaObject(), fields = metaObject(), origin = metaObject();
    setMeta(result, "fields", fields);
    setMeta(result, "origins", origin);
    auto attributes = metaObject();
    setMeta(result, "attributes", attributes);
    if (!metadataSchema)
        return result;
    Statement s(db, "SELECT derived,embedded,overrides,provider,review,rejected,revision,(SELECT revision "
                    "FROM assets WHERE id=metadata_assets.id) FROM metadata_assets WHERE id=?");
    s.bind(1, id);
    if (!s.step())
        return result;
    for (int i = 0; i < 3; ++i)
    {
        auto source = juce::JSON::parse(s.str(i));
        if (auto *o = source.getDynamicObject())
            for (auto &p : o->getProperties())
            {
                if (p.name.toString() == "raw")
                {
                    setMeta(result, "raw", p.value);
                    continue;
                }
                auto attr = metaObject();
                setMeta(attr, "source", i == 0 ? "ucs-8.2.1" : i == 1 ? "embedded" : "user");
                setMeta(attr, "lockedByUser", i == 2);
                setMeta(attr, "confidence", juce::var());
                setMeta(attr, "revision", (juce::int64)s.integer(6));
                setMeta(attributes, p.name.toString(), attr);
                setMeta(fields, p.name.toString(), p.value);
                setMeta(origin, p.name.toString(), i == 0 ? "ucs-8.2.1" : i == 1 ? "embedded" : "user");
            }
    }
    setMeta(result, "overrides", juce::JSON::parse(s.str(2)));
    setMeta(result, "provider", juce::JSON::parse(s.str(3)));
    setMeta(result, "review", s.integer(4) != 0);
    setMeta(result, "providerCurrent",
            result["provider"].hasProperty("sourceRevision") &&
                (juce::int64)result["provider"]["sourceRevision"] == s.integer(7));
    setMeta(result, "rejected", s.str(5));
    setMeta(result, "revision", (juce::int64)s.integer(6));
    return result;
}
void Catalog::indexMetadata(const Asset &a)
{
    auto m = describeMetadata(a.id);
    juce::StringArray terms;
    Statement clear(db, "DELETE FROM metadata_fields WHERE asset=?");
    clear.bind(1, a.id);
    clear.step();
    for (auto key : editableMetadataKeys())
    {
        auto value = m["fields"][juce::Identifier(key)].toString();
        if (value.isEmpty())
            continue;
        Statement ins(db, "INSERT INTO metadata_fields VALUES(?,?,?)");
        ins.bind(1, a.id);
        ins.bind(2, key);
        ins.bind(3, value);
        ins.step();
        terms.add(value);
    }
    terms.add(m["fields"]["aliases"].toString());
    if ((bool)m["providerCurrent"])
        for (auto key : {"provider", "model", "prompt"})
            terms.add(m["provider"][key].toString());
    if (auto *raw = m["raw"].getDynamicObject())
        for (auto &p : raw->getProperties())
            terms.add(p.value.toString());
    Statement f(db, "INSERT OR REPLACE INTO metadata_fts(rowid,terms) SELECT rowid,? FROM assets WHERE id=?");
    f.bind(1, terms.joinIntoString(" "));
    f.bind(2, a.id);
    f.step();
}
void Catalog::snapshotMetadata(const Asset &a, const var &embedded)
{
    ensureMeta(db, a.id);
    Statement update(db, "UPDATE metadata_assets SET review=CASE WHEN revision<>0 AND revision<>? THEN 1 "
                         "ELSE review END, revision=?,embedded=?,derived=? WHERE id=? AND EXISTS(SELECT 1 "
                         "FROM assets WHERE id=? AND revision=?)");
    update.bind(1, a.revision);
    update.bind(2, a.revision);
    update.bind(3, metaJson(embedded));
    update.bind(4, metaJson(namedFields(a.name)));
    update.bind(5, a.id);
    update.bind(6, a.id);
    update.bind(7, a.revision);
    update.step();
    if (sqlite3_changes(db) == 0)
        return;
    indexMetadata(a);
    Statement remove(db, "DELETE FROM metadata_backfill WHERE id=?");
    remove.bind(1, a.id);
    remove.step();
}
void Catalog::providerMetadata(const Asset &a, const var &receipt)
{
    if (!receipt.isObject())
        return;
    metaRequire(receipt["provider"].toString().isNotEmpty() && receipt["sha256"].toString() == mediaDigest(a),
                "Provider receipt does not match indexed WAV");
    auto pinned = receipt.clone();
    setMeta(pinned, "sourceRevision", (juce::int64)a.revision);
    begin();
    try
    {
        ensureMeta(db, a.id);
        auto current = describeMetadata(a.id)["provider"];
        metaRequire(!current.hasProperty("provider") || metaJson(current) == metaJson(pinned),
                    "Provider provenance is immutable");
        Statement s(db, "UPDATE metadata_assets SET provider=? WHERE id=?");
        s.bind(1, metaJson(pinned));
        s.bind(2, a.id);
        s.step();
        indexMetadata(a);
        commit();
    }
    catch (...)
    {
        rollback();
        throw;
    }
}
String Catalog::metadataWhere(const String &q, const String &root)
{
    String sql = root.isEmpty() ? "1" : "a.root_id=" + quoted(root);
    juce::StringArray words;
    auto contains = [&](const String &word)
    {
        return "a.rowid IN(SELECT rowid FROM metadata_fts WHERE metadata_fts MATCH " +
               quoted(matchText(word, {})) + ")";
    };
    for (auto token : juce::StringArray::fromTokens(q, " \t", "\""))
    {
        auto k = token.upToFirstOccurrenceOf(":", false, false).toLowerCase();
        auto value = token.fromFirstOccurrenceOf(":", false, false).unquoted().trim();
        if (!token.containsChar(':'))
        {
            words.add(token);
            continue;
        }
        if (k == "tag")
            k = "tags";
        if (value.isEmpty())
        {
            sql += " AND 0";
            continue;
        }
        if (k == "meta")
            sql += " AND " + contains(value);
        else if (k == "type")
            sql += " AND lower(substr(a.name,-" + String(value.length() + 1) +
                   "))=" + quoted("." + value.toLowerCase());
        else if (k == "min" || k == "max" || k == "channels")
        {
            if (!value.containsOnly("0123456789.") || value.containsOnly("."))
            {
                sql += " AND 0";
                continue;
            }
            sql += " AND " +
                   String(k == "channels" ? "a.channels="
                          : k == "min"    ? "a.duration>="
                                          : "a.duration<=") +
                   String(value.getDoubleValue(), 6);
        }
        else if (k == "generated" || k == "review")
            sql += " AND " + String(value == "0" || value.equalsIgnoreCase("false") ? "NOT " : "") +
                   "EXISTS(SELECT 1 FROM metadata_assets z WHERE z.id=a.id AND " +
                   String(k == "review" ? "z.review=1"
                                        : "json_extract(z.provider,'$.sourceRevision')=a.revision") +
                   ")";
        else if (k == "favorite")
            sql += " AND " + String(value == "0" || value.equalsIgnoreCase("false") ? "NOT " : "") +
                   "EXISTS(SELECT 1 FROM library_refs lr2 WHERE lr2.asset=a.id AND lr2.favorite=1)";
        else if (k == "missing" && editableMetadataKeys().contains(value.toLowerCase()))
            sql += " AND NOT EXISTS(SELECT 1 FROM metadata_fields f WHERE f.asset=a.id AND f.field=" +
                   quoted(value.toLowerCase()) + ")";
        else if (editableMetadataKeys().contains(k))
        {
            if (k == "keywords" || k == "tags")
                sql += " AND a.id IN(SELECT asset FROM metadata_fields WHERE field=" + quoted(k) +
                       " AND instr(','||lower(replace(value,', ',','))||','," +
                       quoted("," + value.toLowerCase() + ",") + ")>0)";
            else if (k == "category" || k == "subcategory" || k == "catid")
                sql += " AND a.id IN(SELECT asset FROM metadata_fields WHERE field=" + quoted(k) +
                       " AND value IN(" + quoted(value) +
                       ", (SELECT value FROM metadata_aliases WHERE field=" + quoted(k) +
                       " AND alias=" + quoted(value) + ")))";
            else
                sql += " AND a.id IN(SELECT asset FROM metadata_fields WHERE field=" + quoted(k) +
                       " AND value=" + quoted(value) + " COLLATE NOCASE)";
        }
        else
            words.add(token);
    }
    if (!words.isEmpty())
    {
        auto text = words.joinIntoString(" ");
        sql += " AND (" + contains(text) + " OR a.rowid IN(SELECT rowid FROM names WHERE names MATCH " +
               quoted(matchText(text, {})) + "))";
    }
    return sql;
}
namespace
{
void storeOverrides(sqlite3 *db, const String &id, const var &tags)
{
    ensureMeta(db, id);
    Statement s(db, "UPDATE metadata_assets SET overrides=? WHERE id=?");
    s.bind(1, metaJson(tags));
    s.bind(2, id);
    s.step();
}
void saveChanges(sqlite3 *db, const juce::Array<var> &rows)
{
    if (rows.isEmpty())
        return;
    auto log = metaJson(rows);
    metaRequire(log.getNumBytesAsUTF8() <= 8 * 1024 * 1024, "Metadata undo step too large");
    Statement redo(db, "DELETE FROM metadata_history WHERE applied=0");
    redo.step();
    Statement s(db, "INSERT INTO metadata_history(rows,applied) VALUES(?,1)");
    s.bind(1, log);
    s.step();
    Statement limit(db, "DELETE FROM metadata_history WHERE seq<(SELECT MAX(seq)-31 FROM metadata_history)");
    limit.step();
}
var changeRow(const Asset &a, var before, var after)
{
    auto r = metaObject();
    setMeta(r, "id", a.id);
    setMeta(r, "before", before);
    setMeta(r, "after", after);
    return r;
}
} // namespace
void Catalog::editMetadata(const CollectionEdit &edit)
{
    begin();
    try
    {
        if (edit.action == "meta-alias")
        {
            auto alias = edit.parent.trim();
            auto key = edit.id;
            metaRequire((key == "category" || key == "subcategory" || key == "catid") && alias.isNotEmpty() &&
                            alias.length() <= 128 && edit.name.length() <= 128,
                        "Choose a category, subcategory or CatID and enter an alias");
            if (edit.name.trim().isEmpty())
            {
                Statement remove(db, "DELETE FROM metadata_aliases WHERE field=? AND alias=?");
                remove.bind(1, key);
                remove.bind(2, alias);
                remove.step();
            }
            else
            {
                Statement add(db, "INSERT OR REPLACE INTO metadata_aliases(field,alias,value) VALUES(?,?,?)");
                add.bind(1, key);
                add.bind(2, alias);
                add.bind(3, edit.name.trim());
                add.step();
            }
        }
        else if (edit.action == "meta-undo" || edit.action == "meta-redo")
        {
            bool undo = edit.action == "meta-undo";
            Statement h(
                db, undo ? "SELECT seq,rows FROM metadata_history WHERE applied=1 ORDER BY seq DESC LIMIT 1"
                         : "SELECT seq,rows FROM metadata_history WHERE applied=0 ORDER BY seq LIMIT 1");
            if (h.step())
            {
                auto changes = juce::JSON::parse(h.str(1));
                for (auto &r : *changes.getArray())
                {
                    auto a = asset(r["id"]);
                    metaRequire(a.id.isNotEmpty() && metaJson(describeMetadata(a.id)["overrides"]) ==
                                                         metaJson(r[undo ? "after" : "before"]),
                                "Metadata history conflict");
                    storeOverrides(db, a.id, r[undo ? "before" : "after"]);
                    indexMetadata(a);
                }
                Statement u(db, "UPDATE metadata_history SET applied=? WHERE seq=?");
                u.bind(1, (int64_t)!undo);
                u.bind(2, h.integer(0));
                u.step();
            }
        }
        else
        {
            metaRequire(edit.items.size() <= 4096 && editableMetadataKeys().contains(edit.id) &&
                            edit.name.length() <= 4096,
                        "Invalid metadata edit");
            std::set<String> seen;
            juce::Array<var> rows;
            for (auto &item : edit.items)
            {
                auto a = asset(item.asset.id);
                metaRequire(a.id.isNotEmpty() && a.revision == item.asset.revision,
                            "Recording changed; review selection");
                if (!seen.insert(a.id).second)
                    continue;
                ensureMeta(db, a.id);
                auto m = describeMetadata(a.id);
                auto before = m["overrides"];
                auto after = before.clone();
                if (edit.action == "meta-review")
                {
                    Statement r(db, "UPDATE metadata_assets SET review=0 WHERE id=? AND revision=?");
                    r.bind(1, a.id);
                    r.bind(2, a.revision);
                    r.step();
                    continue;
                }
                if (edit.action == "meta-reject")
                {
                    Statement r(db, "UPDATE metadata_assets SET rejected=? WHERE id=?");
                    r.bind(1, edit.name);
                    r.bind(2, a.id);
                    r.step();
                    continue;
                }
                if (edit.action == "meta-unlock")
                    after.getDynamicObject()->removeProperty(edit.id);
                else
                    setMeta(after, edit.id, edit.name);
                if (metaJson(before) == metaJson(after))
                    continue;
                rows.add(changeRow(a, before, after));
                storeOverrides(db, a.id, after);
                indexMetadata(a);
            }
            saveChanges(db, rows);
        }
        commit();
    }
    catch (...)
    {
        rollback();
        throw;
    }
}
var Catalog::metadataManifest(const std::vector<BookmarkRequest> &selection)
{
    metaRequire(selection.size() <= 4096, "Metadata export limit");
    auto v = metaObject();
    setMeta(v, "version", 1);
    juce::Array<var> rows;
    std::set<String> ids;
    for (auto &i : selection)
    {
        auto a = asset(i.asset.id);
        metaRequire(a.id.isNotEmpty() && a.revision == i.asset.revision, "Source revision changed");
        if (!ids.insert(a.id).second)
            continue;
        auto r = metaObject();
        setMeta(r, "id", a.id);
        setMeta(r, "name", a.name);
        setMeta(r, "pathHint", a.path);
        setMeta(r, "sha256", mediaDigest(a));
        setMeta(r, "revision", (juce::int64)a.revision);
        auto m = describeMetadata(a.id);
        setMeta(r, "metadata", m);
        rows.add(r);
    }
    setMeta(v, "records", rows);
    return v;
}
void Catalog::importMetadata(const var &manifest)
{
    metaRequire((int)manifest["version"] == 1 && manifest["records"].isArray() &&
                    manifest["records"].size() <= 4096,
                "Unsupported metadata manifest");
    begin();
    try
    {
        juce::Array<var> rows;
        std::set<String> ids;
        for (auto &r : *manifest["records"].getArray())
        {
            auto a = asset(r["id"]);
            metaRequire(!a.id.isEmpty() && ids.insert(a.id).second && a.revision == (int64_t)r["revision"] &&
                            mediaDigest(a) == r["sha256"].toString(),
                        "Metadata import needs review: identity or content mismatch");
            auto incoming = r["metadata"]["overrides"];
            if (!incoming.isObject())
                continue;
            ensureMeta(db, a.id);
            auto before = describeMetadata(a.id)["overrides"], after = before.clone();
            for (auto &p : incoming.getDynamicObject()->getProperties())
            {
                auto key = p.name.toString();
                metaRequire(editableMetadataKeys().contains(key) && p.value.isString() &&
                                p.value.toString().length() <= 4096,
                            "Unknown or invalid catalog field");
                setMeta(after, key, p.value);
            }
            if (metaJson(before) != metaJson(after))
            {
                rows.add(changeRow(a, before, after));
                storeOverrides(db, a.id, after);
                indexMetadata(a);
            }
        }
        saveChanges(db, rows);
        commit();
    }
    catch (...)
    {
        rollback();
        throw;
    }
}
