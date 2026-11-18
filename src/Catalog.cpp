#include "Catalog.h"
#include "MacSources.h"
#include "Performance.h"
#include <sys/stat.h>
#include <stdexcept>
#include <filesystem>

namespace sp
{
namespace
{
struct Statement
{
    sqlite3 *db;
    sqlite3_stmt *s = nullptr;
    Statement(sqlite3 *d, const char *sql) : db(d)
    {
        int rc = sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
        if (rc == SQLITE_INTERRUPT)
            throw QueryCancelled();
        if (rc != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement()
    {
        sqlite3_finalize(s);
    }
    void bind(int n, const juce::String &v)
    {
        sqlite3_bind_text(s, n, v.toRawUTF8(), -1, SQLITE_TRANSIENT);
    }
    void bind(int n, int64_t v)
    {
        sqlite3_bind_int64(s, n, v);
    }
    bool step()
    {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_INTERRUPT)
            throw QueryCancelled();
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        return rc == SQLITE_ROW;
    }
    juce::String str(int n)
    {
        auto *t = sqlite3_column_text(s, n);
        return t ? juce::String::fromUTF8((const char *)t) : juce::String();
    }
    int64_t integer(int n)
    {
        return sqlite3_column_int64(s, n);
    }
    double real(int n)
    {
        return sqlite3_column_double(s, n);
    }
};
Asset readAsset(Statement &s)
{
    Asset a;
    a.id = s.str(0);
    a.rootId = s.str(1);
    a.relative = s.str(2);
    a.name = s.str(3);
    a.size = s.integer(4);
    a.modified = s.integer(5);
    a.inode = s.integer(6);
    a.revision = s.integer(7);
    a.duration = s.real(8);
    a.channels = (int)s.integer(9);
    a.sampleRate = s.real(10);
    a.state = s.str(11);
    a.error = s.str(12);
    a.online = s.integer(13) != 0;
    a.path = juce::File(s.str(14)).getChildFile(a.relative).getFullPathName();
    return a;
}
constexpr auto columns = "a.id,a.root_id,a.relative,a.name,a.size,a.modified,a.inode,a.revision,"
                         "a.duration,a.channels,a.sample_rate,a.state,a.error,(a.online AND r.online),r.path";
} // namespace

bool fileStamp(const juce::File &f, FileStamp &out)
{
    ++performance.fileStats;
    struct stat st{};
    if (lstat(f.getFullPathName().toRawUTF8(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    out = {st.st_size, st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec, (int64_t)st.st_ino};
    return true;
}
bool supportedExtension(const juce::File &f)
{
    return f.hasFileExtension("wav;bwf;aif;aiff;flac;ogg;mp3;m4a;mp4;caf;w64");
}
Catalog::Catalog(const juce::File &file, bool readOnly)
{
    const int flags = readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(file.getFullPathName().toRawUTF8(), &db, flags | SQLITE_OPEN_NOMUTEX, nullptr) !=
        SQLITE_OK)
    {
        juce::String err = db ? sqlite3_errmsg(db) : "Cannot open catalog";
        sqlite3_close(db);
        db = nullptr;
        throw std::runtime_error(err.toStdString());
    }
    sqlite3_busy_timeout(db, 3000);
    try
    {
        exec("PRAGMA foreign_keys=ON");
        exec("PRAGMA cache_size=-4096; PRAGMA temp_store=FILE; PRAGMA temp.cache_size=-2048;");
        if (readOnly)
            return;
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        int64_t schemaVersion = 0;
        {
            Statement version(db, "PRAGMA user_version");
            version.step();
            schemaVersion = version.integer(0);
        }
        if (schemaVersion > 2)
            throw std::runtime_error("Catalog was created by a newer SoundPlus version");
        exec(R"SQL(BEGIN IMMEDIATE;
          CREATE TABLE IF NOT EXISTS roots(id TEXT PRIMARY KEY,path TEXT UNIQUE NOT NULL,volume TEXT NOT NULL,
            volume_relative TEXT NOT NULL,online INTEGER NOT NULL DEFAULT 1,scanning INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT '');
          CREATE TABLE IF NOT EXISTS assets(id TEXT PRIMARY KEY,root_id TEXT NOT NULL REFERENCES roots(id),
            relative TEXT NOT NULL,name TEXT NOT NULL,size INTEGER NOT NULL,modified INTEGER NOT NULL,inode INTEGER NOT NULL,
            revision INTEGER NOT NULL DEFAULT 1,duration REAL,channels INTEGER,sample_rate REAL,
            state TEXT NOT NULL DEFAULT 'pending',error TEXT NOT NULL DEFAULT '',online INTEGER NOT NULL DEFAULT 1,
            seen INTEGER NOT NULL,UNIQUE(root_id,relative));
          CREATE INDEX IF NOT EXISTS asset_inode ON assets(root_id,inode);
          CREATE INDEX IF NOT EXISTS asset_state ON assets(state,online);
          CREATE INDEX IF NOT EXISTS asset_name ON assets(name COLLATE NOCASE);
          CREATE VIRTUAL TABLE IF NOT EXISTS names USING fts5(name,relative,content='assets',content_rowid='rowid',tokenize='unicode61',prefix='2 3 4');
          CREATE TRIGGER IF NOT EXISTS asset_insert AFTER INSERT ON assets BEGIN
            INSERT INTO names(rowid,name,relative) VALUES(new.rowid,new.name,new.relative); END;
          CREATE TRIGGER IF NOT EXISTS asset_delete AFTER DELETE ON assets BEGIN
            INSERT INTO names(names,rowid,name,relative) VALUES('delete',old.rowid,old.name,old.relative); END;
          CREATE TRIGGER IF NOT EXISTS asset_name_update AFTER UPDATE OF name,relative ON assets BEGIN
            INSERT INTO names(names,rowid,name,relative) VALUES('delete',old.rowid,old.name,old.relative);
            INSERT INTO names(rowid,name,relative) VALUES(new.rowid,new.name,new.relative); END;
          COMMIT;)SQL");
        if (schemaVersion < 2)
        {
            exec(R"SQL(BEGIN IMMEDIATE;
            ALTER TABLE roots ADD COLUMN discovered INTEGER NOT NULL DEFAULT 0;
            ALTER TABLE roots ADD COLUMN ready INTEGER NOT NULL DEFAULT 0;
            ALTER TABLE roots ADD COLUMN failed INTEGER NOT NULL DEFAULT 0;
            ALTER TABLE assets ADD COLUMN parent TEXT NOT NULL DEFAULT '';
            CREATE INDEX asset_parent ON assets(root_id,parent);
            CREATE INDEX asset_browse ON assets(name COLLATE NOCASE,id);
            CREATE INDEX asset_root_browse ON assets(root_id,name COLLATE NOCASE,id);
            CREATE TABLE directories(root_id TEXT NOT NULL REFERENCES roots(id),relative TEXT NOT NULL,
              modified INTEGER NOT NULL,inode INTEGER NOT NULL,children TEXT NOT NULL,PRIMARY KEY(root_id,relative));
            DROP TRIGGER asset_insert; DROP TRIGGER asset_delete; DROP TRIGGER asset_name_update;
            DROP TABLE names;
            CREATE VIRTUAL TABLE names USING fts5(name,relative,root_id,content='assets',content_rowid='rowid',tokenize='unicode61',prefix='2 3 4');
            INSERT INTO names(rowid,name,relative,root_id) SELECT rowid,name,relative,root_id FROM assets;
            CREATE TRIGGER asset_insert AFTER INSERT ON assets BEGIN
              INSERT INTO names(rowid,name,relative,root_id) VALUES(new.rowid,new.name,new.relative,new.root_id);
              UPDATE roots SET discovered=discovered+1,ready=ready+(new.state='ready'),failed=failed+(new.state='failed') WHERE id=new.root_id; END;
            CREATE TRIGGER asset_delete AFTER DELETE ON assets BEGIN
              INSERT INTO names(names,rowid,name,relative,root_id) VALUES('delete',old.rowid,old.name,old.relative,old.root_id);
              UPDATE roots SET discovered=discovered-1,ready=ready-(old.state='ready'),failed=failed-(old.state='failed') WHERE id=old.root_id; END;
            CREATE TRIGGER asset_name_update AFTER UPDATE OF name,relative ON assets BEGIN
              INSERT INTO names(names,rowid,name,relative,root_id) VALUES('delete',old.rowid,old.name,old.relative,old.root_id);
              INSERT INTO names(rowid,name,relative,root_id) VALUES(new.rowid,new.name,new.relative,new.root_id); END;
            CREATE TRIGGER asset_status AFTER UPDATE OF state ON assets WHEN old.state<>new.state BEGIN
              UPDATE roots SET ready=ready+(new.state='ready')-(old.state='ready'),failed=failed+(new.state='failed')-(old.state='failed') WHERE id=new.root_id; END;
            UPDATE roots SET discovered=(SELECT count(*) FROM assets WHERE root_id=roots.id),
              ready=(SELECT count(*) FROM assets WHERE root_id=roots.id AND state='ready'),failed=(SELECT count(*) FROM assets WHERE root_id=roots.id AND state='failed');
            )SQL");
            std::vector<std::pair<juce::String, juce::String>> parents;
            {
                Statement rows(db, "SELECT id,relative FROM assets");
                while (rows.step())
                {
                    auto relative = rows.str(1);
                    parents.emplace_back(rows.str(0), (relative.containsChar('/')
                                                           ? relative.upToLastOccurrenceOf("/", false, false)
                                                           : juce::String()));
                }
            }
            for (auto &[id, parent] : parents)
            {
                Statement update(db, "UPDATE assets SET parent=? WHERE id=?");
                update.bind(1, parent);
                update.bind(2, id);
                update.step();
            }
            exec("PRAGMA user_version=2; COMMIT;");
        }
        exec("CREATE INDEX IF NOT EXISTS asset_root_pending ON assets(root_id,state,online); UPDATE roots "
             "SET scanning=0;");
    }
    catch (...)
    {
        sqlite3_close(db);
        db = nullptr;
        throw;
    }
}
Catalog::~Catalog()
{
    if (db)
        sqlite3_close(db);
}
void Catalog::exec(const char *sql)
{
    char *msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK)
    {
        std::string e = msg ? msg : "SQLite error";
        sqlite3_free(msg);
        if (rc == SQLITE_INTERRUPT)
            throw QueryCancelled();
        throw std::runtime_error(e);
    }
}
void Catalog::begin()
{
    exec("BEGIN IMMEDIATE");
}
void Catalog::commit()
{
    exec("COMMIT");
    ++performance.commits;
}
void Catalog::rollback()
{
    if (!sqlite3_get_autocommit(db))
        exec("ROLLBACK");
}
std::vector<Root> Catalog::roots()
{
    Statement s(db, "SELECT id,path,volume,volume_relative,online,scanning,error,discovered,ready,failed "
                    "FROM roots ORDER BY path");
    std::vector<Root> v;
    while (s.step())
    {
        Root r;
        r.id = s.str(0);
        r.path = s.str(1);
        r.volume = s.str(2);
        r.volumeRelative = s.str(3);
        r.online = s.integer(4) != 0;
        r.scanning = s.integer(5) != 0;
        r.error = s.str(6);
        r.discovered = (int)s.integer(7);
        r.ready = (int)s.integer(8);
        r.failed = (int)s.integer(9);
        v.push_back(r);
    }
    return v;
}
Root Catalog::addRoot(const juce::File &path)
{
    auto canonical =
        juce::String::fromUTF8(std::filesystem::canonical(path.getFullPathName().toStdString()).c_str());
    for (auto r : roots())
        if (r.path == canonical)
            return r;
    Root r;
    r.id = juce::Uuid().toString();
    r.path = canonical;
    r.online = true;
    auto volume = volumeLocation(juce::File(canonical));
    r.volume = volume.identity;
    r.volumeRelative = volume.relative;
    Statement s(db, "INSERT INTO roots(id,path,volume,volume_relative) VALUES(?,?,?,?)");
    s.bind(1, r.id);
    s.bind(2, r.path);
    s.bind(3, r.volume);
    s.bind(4, r.volumeRelative);
    s.step();
    return r;
}
void Catalog::rootState(const Root &r)
{
    Statement s(db,
                "UPDATE roots SET path=?,online=?,scanning=?,error=?,volume=?,volume_relative=? WHERE id=?");
    s.bind(1, r.path);
    s.bind(2, (int64_t)r.online);
    s.bind(3, (int64_t)r.scanning);
    s.bind(4, r.error);
    s.bind(5, r.volume);
    s.bind(6, r.volumeRelative);
    s.bind(7, r.id);
    s.step();
}
bool Catalog::discover(const Root &r, const juce::String &relative, const FileStamp &st, int64_t scan,
                       bool legacyRelink, const juce::String &confirmedRename)
{
    juce::String id;
    bool changed = true;
    int64_t revision = 1;
    {
        Statement s(
            db, "SELECT id,size,modified,inode,revision,state FROM assets WHERE root_id=? AND relative=?");
        s.bind(1, r.id);
        s.bind(2, relative);
        if (s.step())
        {
            id = s.str(0);
            changed = s.integer(1) != st.size || s.integer(2) != st.modified || s.integer(3) != st.inode;
            revision = s.integer(4) + (changed && s.str(5) != "pending" ? 1 : 0);
        }
    }
    if (id.isEmpty() && confirmedRename.isNotEmpty())
    {
        Statement old(db, "SELECT id,revision,size,modified FROM assets WHERE id=? AND root_id=?");
        old.bind(1, confirmedRename);
        old.bind(2, r.id);
        if (old.step())
        {
            id = old.str(0);
            changed = old.integer(2) != st.size || old.integer(3) != st.modified;
            revision = old.integer(1) + (changed ? 1 : 0);
        }
    }
    if (id.isEmpty() && legacyRelink)
    {
        // Only a unique inode whose previous path disappeared is a confirmed rename.
        Statement s(db, "SELECT id,relative,revision,size,modified FROM assets WHERE root_id=? AND inode=?");
        s.bind(1, r.id);
        s.bind(2, st.inode);
        struct Candidate
        {
            juce::String id;
            int64_t rev;
            bool changed;
        };
        std::vector<Candidate> candidates;
        while (s.step())
            if (!juce::File(r.path).getChildFile(s.str(1)).exists())
                candidates.push_back(
                    {s.str(0), s.integer(2), s.integer(3) != st.size || s.integer(4) != st.modified});
        if (candidates.size() == 1)
        {
            id = candidates[0].id;
            changed = candidates[0].changed;
            revision = candidates[0].rev + (changed ? 1 : 0);
        }
    }
    bool inserted = id.isEmpty();
    if (id.isEmpty())
    {
        Statement s(db, "INSERT INTO assets(id,root_id,relative,name,size,modified,inode,seen,parent) "
                        "VALUES(?,?,?,?,?,?,?,?,?)");
        s.bind(1, juce::Uuid().toString());
        s.bind(2, r.id);
        s.bind(3, relative);
        s.bind(4, juce::File(r.path).getChildFile(relative).getFileName());
        s.bind(5, st.size);
        s.bind(6, st.modified);
        s.bind(7, st.inode);
        s.bind(8, scan);
        s.bind(9, relative.containsChar('/') ? relative.upToLastOccurrenceOf("/", false, false)
                                             : juce::String());
        s.step();
    }
    else
    {
        if (changed)
        {
            Statement s(db, "UPDATE assets SET "
                            "relative=?,name=?,size=?,modified=?,inode=?,seen=?,online=1,revision=?,state='"
                            "pending',duration=NULL,channels=NULL,sample_rate=NULL,error='' WHERE id=?");
            s.bind(1, relative);
            s.bind(2, juce::File(r.path).getChildFile(relative).getFileName());
            s.bind(3, st.size);
            s.bind(4, st.modified);
            s.bind(5, st.inode);
            s.bind(6, scan);
            s.bind(7, revision);
            s.bind(8, id);
            s.step();
        }
        else
        {
            Statement s(db, "UPDATE assets SET seen=?,online=1 WHERE id=?");
            s.bind(1, scan);
            s.bind(2, id);
            s.step();
            // The WHERE guard avoids touching FTS for every unchanged source on reopen.
            Statement move(db, "UPDATE assets SET relative=?,name=? WHERE id=? AND relative<>?");
            move.bind(1, relative);
            move.bind(2, juce::File(r.path).getChildFile(relative).getFileName());
            move.bind(3, id);
            move.bind(4, relative);
            move.step();
        }
    }
    {
        if (inserted)
            return changed;
        Statement parent(db, "UPDATE assets SET parent=? WHERE root_id=? AND relative=? AND parent<>?");
        auto value =
            (relative.containsChar('/') ? relative.upToLastOccurrenceOf("/", false, false) : juce::String());
        parent.bind(1, value);
        parent.bind(2, r.id);
        parent.bind(3, relative);
        parent.bind(4, value);
        parent.step();
    }
    return changed;
}
void Catalog::finishScan(const Root &r, int64_t scan)
{
    Statement s(db, "UPDATE assets SET online=0 WHERE root_id=? AND seen<>?");
    s.bind(1, r.id);
    s.bind(2, scan);
    s.step();
}
std::vector<Asset> Catalog::pending(int limit)
{
    juce::String sql = "SELECT " + juce::String(columns) +
                       " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.state='pending' AND a.online=1 "
                       "AND r.online=1 LIMIT ?";
    Statement s(db, sql.toRawUTF8());
    s.bind(1, (int64_t)limit);
    std::vector<Asset> v;
    while (s.step())
        v.push_back(readAsset(s));
    return v;
}
void Catalog::metadata(const Asset &a, double duration, int channels, double rate, const juce::String &error)
{
    Statement s(db, "UPDATE assets SET duration=?,channels=?,sample_rate=?,state=?,error=? WHERE id=? AND "
                    "revision=? AND modified=?");
    sqlite3_bind_double(s.s, 1, duration);
    s.bind(2, (int64_t)channels);
    sqlite3_bind_double(s.s, 3, rate);
    s.bind(4, error.isEmpty() ? "ready" : "failed");
    s.bind(5, error);
    s.bind(6, a.id);
    s.bind(7, a.revision);
    s.bind(8, a.modified);
    s.step();
}
namespace
{
juce::String matchText(const juce::String &query, const juce::String &root)
{
    auto tokens = juce::StringArray::fromTokens(query, " \t\r\n", "\"");
    juce::String match;
    for (auto token : tokens)
    {
        token = token.trim().unquoted().replace("\"", "\"\"");
        if (token.isNotEmpty())
        {
            if (match.isNotEmpty())
                match += " AND ";
            match += "\"" + token + "\"*";
        }
    }
    if (match.isNotEmpty() && root.isNotEmpty())
        match = "root_id : \"" + root.replace("\"", "\"\"") + "\" AND (" + match + ")";
    return match;
}
bool uuidQuery(const juce::String &q)
{
    return q.length() == 32 && q.containsOnly("0123456789abcdefABCDEF");
}
} // namespace
void Catalog::cancelWhen(const std::atomic<uint64_t> *generation, uint64_t expected)
{
    cancellation = generation;
    expectedGeneration = expected;
    sqlite3_progress_handler(
        db, generation ? 500 : 0,
        [](void *p)
        {
            auto &self = *static_cast<Catalog *>(p);
            return self.cancellation && self.cancellation->load() != self.expectedGeneration ? 1 : 0;
        },
        this);
}
int Catalog::countMatches(const juce::String &q, const juce::String &root)
{
    auto match = matchText(q, root);
    juce::String sql;
    if (uuidQuery(q))
        sql = "SELECT count(*) FROM assets WHERE id=?" + (root.isEmpty() ? juce::String() : " AND root_id=?");
    else if (match.isNotEmpty())
        sql = "SELECT count(*) FROM names WHERE names MATCH ?";
    else
        sql = "SELECT count(*) FROM assets" + (root.isEmpty() ? juce::String() : " WHERE root_id=?");
    Statement s(db, sql.toRawUTF8());
    int n = 1;
    if (uuidQuery(q))
    {
        s.bind(n++, q);
        if (root.isNotEmpty())
            s.bind(n++, root);
    }
    else if (match.isNotEmpty())
        s.bind(n++, match);
    else if (root.isNotEmpty())
        s.bind(n++, root);
    s.step();
    return (int)s.integer(0);
}
std::vector<Asset> Catalog::search(const juce::String &q, const juce::String &root, int limit, int offset)
{
    ++performance.queries;
    std::vector<Asset> result;
    auto collect = [&](Statement &s)
    {
        while (s.step())
            result.push_back(readAsset(s));
    };
    const auto base = "SELECT " + juce::String(columns) + " FROM assets a JOIN roots r ON r.id=a.root_id ";
    if (uuidQuery(q))
    {
        Statement s(
            db, (base + "WHERE a.id=?" + (root.isEmpty() ? juce::String() : " AND a.root_id=?")).toRawUTF8());
        s.bind(1, q);
        if (root.isNotEmpty())
            s.bind(2, root);
        if (offset == 0)
            collect(s);
        return result;
    }
    auto match = matchText(q, root);
    if (match.isEmpty())
    {
        auto sql = base + (root.isEmpty() ? juce::String() : "WHERE a.root_id=? ") +
                   "ORDER BY a.name COLLATE NOCASE,a.id LIMIT ? OFFSET ?";
        Statement s(db, sql.toRawUTF8());
        int n = 1;
        if (root.isNotEmpty())
            s.bind(n++, root);
        s.bind(n++, (int64_t)limit);
        s.bind(n++, (int64_t)offset);
        collect(s);
        return result;
    }
    int exact = 0;
    {
        Statement s(db, ("SELECT count(*) FROM assets WHERE name=? COLLATE NOCASE" +
                         (root.isEmpty() ? juce::String() : " AND root_id=?"))
                            .toRawUTF8());
        s.bind(1, q);
        if (root.isNotEmpty())
            s.bind(2, root);
        s.step();
        exact = (int)s.integer(0);
    }
    if (offset < exact)
    {
        Statement s(db, (base + "WHERE a.name=? COLLATE NOCASE " +
                         (root.isEmpty() ? juce::String() : "AND a.root_id=? ") +
                         "ORDER BY a.rowid LIMIT ? OFFSET ?")
                            .toRawUTF8());
        int n = 1;
        s.bind(n++, q);
        if (root.isNotEmpty())
            s.bind(n++, root);
        s.bind(n++, (int64_t)limit);
        s.bind(n++, (int64_t)offset);
        collect(s);
    }
    if ((int)result.size() < limit)
    {
        Statement s(db, (base + "JOIN names ON names.rowid=a.rowid WHERE names MATCH ? AND a.name<>? COLLATE "
                                "NOCASE ORDER BY names.rowid LIMIT ? OFFSET ?")
                            .toRawUTF8());
        s.bind(1, match);
        s.bind(2, q);
        s.bind(3, (int64_t)(limit - result.size()));
        s.bind(4, (int64_t)std::max(0, offset - exact));
        collect(s);
    }
    return result;
}
void Catalog::materialize(const juce::String &q, const juce::String &root)
{
    exec("DROP TABLE IF EXISTS temp.browse_ids; CREATE TEMP TABLE browse_ids(position INTEGER PRIMARY KEY,id "
         "TEXT NOT NULL);");
    auto match = matchText(q, root);
    juce::String sql;
    if (uuidQuery(q))
        sql = "INSERT INTO browse_ids(id) SELECT id FROM assets WHERE id=?" +
              (root.isEmpty() ? juce::String() : " AND root_id=?");
    else if (match.isEmpty())
        sql = "INSERT INTO browse_ids(id) SELECT id FROM assets " +
              (root.isEmpty() ? juce::String() : "WHERE root_id=? ") + "ORDER BY name COLLATE NOCASE,id";
    else
    {
        {
            Statement s(db, ("INSERT INTO browse_ids(id) SELECT id FROM assets WHERE name=? COLLATE NOCASE " +
                             (root.isEmpty() ? juce::String() : "AND root_id=? ") + "ORDER BY rowid")
                                .toRawUTF8());
            s.bind(1, q);
            if (root.isNotEmpty())
                s.bind(2, root);
            s.step();
        }
        sql = "INSERT INTO browse_ids(id) SELECT a.id FROM names JOIN assets a ON a.rowid=names.rowid WHERE "
              "names MATCH ? AND a.name<>? COLLATE NOCASE ORDER BY names.rowid";
    }
    Statement s(db, sql.toRawUTF8());
    int n = 1;
    if (uuidQuery(q))
    {
        s.bind(n++, q);
        if (root.isNotEmpty())
            s.bind(n++, root);
    }
    else if (match.isNotEmpty())
    {
        s.bind(n++, match);
        s.bind(n++, q);
    }
    else if (root.isNotEmpty())
        s.bind(n++, root);
    s.step();
}
std::vector<Asset> Catalog::page(int offset, int limit)
{
    Statement s(db, ("SELECT " + juce::String(columns) +
                     " FROM temp.browse_ids b JOIN assets a ON a.id=b.id JOIN roots r ON r.id=a.root_id "
                     "WHERE b.position>? ORDER BY b.position LIMIT ?")
                        .toRawUTF8());
    s.bind(1, (int64_t)offset);
    s.bind(2, (int64_t)limit);
    std::vector<Asset> rows;
    while (s.step())
        rows.push_back(readAsset(s));
    return rows;
}
Asset Catalog::existing(const juce::String &root, const juce::String &relative)
{
    Statement s(db, ("SELECT " + juce::String(columns) +
                     " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.root_id=? AND a.relative=?")
                        .toRawUTF8());
    s.bind(1, root);
    s.bind(2, relative);
    return s.step() ? readAsset(s) : Asset{};
}
std::vector<Asset> Catalog::inodeCandidates(const juce::String &root, int64_t inode)
{
    Statement s(db, ("SELECT " + juce::String(columns) +
                     " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.root_id=? AND a.inode=? LIMIT 3")
                        .toRawUTF8());
    s.bind(1, root);
    s.bind(2, inode);
    std::vector<Asset> rows;
    while (s.step())
        rows.push_back(readAsset(s));
    return rows;
}
std::vector<Asset> Catalog::pendingRoot(const juce::String &root, int limit)
{
    Statement s(db, ("SELECT " + juce::String(columns) +
                     " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.root_id=? AND a.state='pending' "
                     "AND a.online=1 LIMIT ?")
                        .toRawUTF8());
    s.bind(1, root);
    s.bind(2, (int64_t)limit);
    std::vector<Asset> rows;
    while (s.step())
        rows.push_back(readAsset(s));
    return rows;
}
DirectoryRecord Catalog::directory(const juce::String &root, const juce::String &relative)
{
    DirectoryRecord d;
    d.relative = relative;
    Statement s(db, "SELECT modified,inode,children FROM directories WHERE root_id=? AND relative=?");
    s.bind(1, root);
    s.bind(2, relative);
    if (s.step())
    {
        d.valid = true;
        d.modified = s.integer(0);
        d.inode = s.integer(1);
        auto a = juce::JSON::parse(s.str(2));
        if (auto *children = a.getArray())
            for (auto &v : *children)
                d.children.add(v.toString());
    }
    return d;
}
void Catalog::directoryDone(const Root &root, const DirectoryRecord &d, int64_t generation)
{
    {
        Statement s(db, "UPDATE assets SET online=0 WHERE root_id=? AND parent=? AND seen<>?");
        s.bind(1, root.id);
        s.bind(2, d.relative);
        s.bind(3, generation);
        s.step();
    }
    juce::Array<juce::var> children;
    for (auto &child : d.children)
        children.add(child);
    Statement s(db, "INSERT INTO directories(root_id,relative,modified,inode,children) VALUES(?,?,?,?,?) ON "
                    "CONFLICT(root_id,relative) DO UPDATE SET "
                    "modified=excluded.modified,inode=excluded.inode,children=excluded.children");
    s.bind(1, root.id);
    s.bind(2, d.relative);
    s.bind(3, d.modified);
    s.bind(4, d.inode);
    s.bind(5, juce::JSON::toString(children, true));
    s.step();
}
void Catalog::removeTree(const juce::String &root, const juce::String &relative)
{
    auto prefix = relative.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "/%";
    {
        Statement s(
            db, "UPDATE assets SET online=0 WHERE root_id=? AND (relative=? OR relative LIKE ? ESCAPE '\\')");
        s.bind(1, root);
        s.bind(2, relative);
        s.bind(3, prefix);
        s.step();
    }
    Statement s(db,
                "DELETE FROM directories WHERE root_id=? AND (relative=? OR relative LIKE ? ESCAPE '\\')");
    s.bind(1, root);
    s.bind(2, relative);
    s.bind(3, prefix);
    s.step();
}
Root Catalog::addRootRecord(const juce::File &path)
{
    for (auto r : roots())
        if (r.path == path.getFullPathName())
            return r;
    Root r;
    r.id = juce::Uuid().toString();
    r.path = path.getFullPathName();
    r.online = true;
    Statement s(db, "INSERT INTO roots(id,path,volume,volume_relative) VALUES(?,?,?,?)");
    s.bind(1, r.id);
    s.bind(2, r.path);
    s.bind(3, juce::String());
    s.bind(4, juce::String());
    s.step();
    return r;
}
int Catalog::count()
{
    Statement s(db, "SELECT count(*) FROM assets");
    s.step();
    return (int)s.integer(0);
}
void Catalog::removeEmptyRoot(const juce::String &id)
{
    Statement s(db, "DELETE FROM roots WHERE id=? AND discovered=0");
    s.bind(1, id);
    s.step();
}
} // namespace sp
