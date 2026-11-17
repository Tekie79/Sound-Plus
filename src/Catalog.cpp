#include "Catalog.h"
#include "MacSources.h"
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
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
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
        if (readOnly)
            return;
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        Statement version(db, "PRAGMA user_version");
        version.step();
        if (version.integer(0) > 1)
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
          PRAGMA user_version=1; COMMIT;)SQL");
        exec("UPDATE roots SET scanning=0");
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
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK)
    {
        std::string e = msg ? msg : "SQLite error";
        sqlite3_free(msg);
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
}
std::vector<Root> Catalog::roots()
{
    Statement s(db, "SELECT r.id,r.path,r.volume,r.volume_relative,r.online,r.scanning,r.error,"
                    "COUNT(a.id),COALESCE(SUM(a.state='ready'),0),COALESCE(SUM(a.state='failed'),0) "
                    "FROM roots r LEFT JOIN assets a ON a.root_id=r.id GROUP BY r.id ORDER BY r.path");
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
    Statement s(db, "UPDATE roots SET path=?,online=?,scanning=?,error=? WHERE id=?");
    s.bind(1, r.path);
    s.bind(2, (int64_t)r.online);
    s.bind(3, (int64_t)r.scanning);
    s.bind(4, r.error);
    s.bind(5, r.id);
    s.step();
}
bool Catalog::discover(const Root &r, const juce::String &relative, const FileStamp &st, int64_t scan)
{
    juce::String id;
    bool changed = true;
    int64_t revision = 1;
    {
        Statement s(db, "SELECT id,size,modified,inode,revision FROM assets WHERE root_id=? AND relative=?");
        s.bind(1, r.id);
        s.bind(2, relative);
        if (s.step())
        {
            id = s.str(0);
            changed = s.integer(1) != st.size || s.integer(2) != st.modified || s.integer(3) != st.inode;
            revision = s.integer(4) + (changed ? 1 : 0);
        }
    }
    if (id.isEmpty())
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
    if (id.isEmpty())
    {
        Statement s(
            db,
            "INSERT INTO assets(id,root_id,relative,name,size,modified,inode,seen) VALUES(?,?,?,?,?,?,?,?)");
        s.bind(1, juce::Uuid().toString());
        s.bind(2, r.id);
        s.bind(3, relative);
        s.bind(4, juce::File(r.path).getChildFile(relative).getFileName());
        s.bind(5, st.size);
        s.bind(6, st.modified);
        s.bind(7, st.inode);
        s.bind(8, scan);
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
std::vector<Asset> Catalog::search(const juce::String &query, const juce::String &root, int limit)
{
    if (query.length() == 32 && query.containsOnly("0123456789abcdefABCDEF"))
    {
        juce::String sql =
            "SELECT " + juce::String(columns) +
            " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.id=? AND (?='' OR a.root_id=?)";
        Statement s(db, sql.toRawUTF8());
        s.bind(1, query);
        s.bind(2, root);
        s.bind(3, root);
        std::vector<Asset> v;
        while (s.step())
            v.push_back(readAsset(s));
        return v;
    }
    // Escape every token: user text can never become an FTS operator or SQL.
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
    juce::String sql = "SELECT " + juce::String(columns) + " FROM assets a JOIN roots r ON r.id=a.root_id ";
    if (match.isNotEmpty())
        sql += "JOIN names ON names.rowid=a.rowid ";
    sql += "WHERE (?='' OR a.root_id=?) ";
    if (match.isNotEmpty())
        sql += "AND (names MATCH ?) ";
    sql += "ORDER BY (a.name=? COLLATE NOCASE OR a.id=?) DESC,";
    if (match.isNotEmpty())
        sql += "bm25(names),";
    sql += "a.name COLLATE NOCASE,a.id LIMIT ?";
    Statement s(db, sql.toRawUTF8());
    int n = 1;
    s.bind(n++, root);
    s.bind(n++, root);
    if (match.isNotEmpty())
        s.bind(n++, match);
    s.bind(n++, query);
    s.bind(n++, query);
    s.bind(n++, (int64_t)limit);
    std::vector<Asset> v;
    while (s.step())
        v.push_back(readAsset(s));
    return v;
}
int Catalog::count()
{
    Statement s(db, "SELECT count(*) FROM assets");
    s.step();
    return (int)s.integer(0);
}
} // namespace sp
