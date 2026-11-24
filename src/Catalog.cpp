#include "MetadataFields.h"
#include "AcousticMetadata.h"
#include <juce_cryptography/juce_cryptography.h>
#include "Catalog.h"
#include "RegionRanking.h"
#include "MacSources.h"
#include "Performance.h"
#include <sys/stat.h>
#include <stdexcept>
#include <filesystem>
#include <Accelerate/Accelerate.h>
#include <cmath>
#include <map>
#include <set>

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
    a.samples = s.integer(15);
    auto layout = s.str(16);
    if (layout.isNotEmpty())
    {
        auto v = juce::JSON::parse(layout);
        a.detectedChannels = layoutFromJson(v["layout"]);
        a.layoutConfidence = v["confidence"].toString();
    }
    else if (a.channels > 0)
    {
        a.detectedChannels = a.channels <= 2 ? channelLayout(a.channels == 1 ? "mono-v1" : "stereo-v1")
                                             : discreteLayout(a.channels, true);
        a.layoutConfidence = "unknown";
    }
    auto overrideText = s.str(17);
    if (overrideText.isNotEmpty() && a.channels > 0)
    {
        auto proposed = layoutFromJson(juce::JSON::parse(overrideText));
        // Older catalogs may contain an override from a replaced source. Never
        // expose that incompatible interpretation to audition or Designer Add.
        if (proposed.size() == a.channels)
            a.layoutOverride = proposed;
    }
    if (sqlite3_column_count(s.s) > 18 && juce::String(sqlite3_column_name(s.s, 18)) == "is_favorite")
        a.favorite = s.integer(18) != 0;
    return a;
}
constexpr auto favoriteColumn = ",COALESCE((SELECT favorite FROM library_refs lr WHERE lr.asset=a.id AND "
                                "lr.first=-1 AND lr.last=-1 AND lr.revision=0),0) AS is_favorite";
constexpr auto currentColumns = "a.id,a.root_id,a.relative,a.name,a.size,a.modified,a.inode,a.revision,"
                                "a.duration,a.channels,a.sample_rate,a.state,a.error,(a.online AND "
                                "r.online),r.path,a.samples,a.channel_layout,a.layout_override";
} // namespace

bool fileStamp(const juce::File &f, FileStamp &out)
{
    ++performance.fileStats;
    struct stat st
    {
    };
    if (lstat(f.getFullPathName().toRawUTF8(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    out = {st.st_size, st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec, (int64_t)st.st_ino};
    return true;
}
bool supportedExtension(const juce::File &f)
{
    return f.hasFileExtension("wav;bwf;aif;aiff;flac;ogg;mp3;m4a;mp4;caf;w64");
}
Catalog::Catalog(const juce::File &file, bool readOnly, std::function<void()> beforeMigrationCommit)
    : columns(currentColumns)
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
        int64_t schemaVersion = 0;
        {
            Statement version(db, "PRAGMA user_version");
            version.step();
            schemaVersion = version.integer(0);
        }
        metadataSchema = schemaVersion >= 8;
        if (schemaVersion > 9)
            throw std::runtime_error("Catalog was created by a newer SoundPlus version");
        if (readOnly)
        {
            // Recovery can open before the Library writer finishes migration.
            // Read the old schema without writing or waiting on the UI/audio thread.
            // Keep this connection's projection valid across the writer's commit.
            juce::StringArray fields;
            Statement info(db, "PRAGMA table_info(assets)");
            while (info.step())
                fields.add(info.str(1));
            if (!fields.contains("samples"))
                columns = columns.replace("a.samples", "CAST(round(a.duration*a.sample_rate) AS INTEGER)");
            if (!fields.contains("channel_layout"))
                columns = columns.replace("a.channel_layout", "''");
            if (!fields.contains("layout_override"))
                columns = columns.replace("a.layout_override", "''");
            return;
        }
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
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
        if (schemaVersion < 3)
        {
            exec(R"SQL(BEGIN IMMEDIATE;
              ALTER TABLE assets ADD COLUMN samples INTEGER NOT NULL DEFAULT 0;
              UPDATE assets SET samples=CAST(round(duration*sample_rate) AS INTEGER) WHERE state='ready';
              CREATE TABLE analysis_jobs(
                asset_id TEXT NOT NULL REFERENCES assets(id) ON DELETE CASCADE,revision INTEGER NOT NULL,
                root_id TEXT NOT NULL REFERENCES roots(id),model TEXT NOT NULL,preprocessing TEXT NOT NULL,
                state TEXT NOT NULL DEFAULT 'pending',next_start INTEGER NOT NULL DEFAULT 0,
                max_end INTEGER NOT NULL DEFAULT 0,rate REAL NOT NULL,error TEXT NOT NULL DEFAULT '',
                PRIMARY KEY(asset_id,revision,model,preprocessing));
              CREATE INDEX analysis_pending ON analysis_jobs(model,preprocessing,state,root_id,asset_id);
              CREATE TABLE embeddings(
                asset_id TEXT NOT NULL REFERENCES assets(id) ON DELETE CASCADE,revision INTEGER NOT NULL,
                model TEXT NOT NULL,preprocessing TEXT NOT NULL,start_sample INTEGER NOT NULL,
                end_sample INTEGER NOT NULL,rate REAL NOT NULL,vector BLOB NOT NULL,
                PRIMARY KEY(asset_id,revision,model,preprocessing,start_sample),
                CHECK(start_sample>=0 AND end_sample>start_sample),CHECK(length(vector)=2048));
              CREATE INDEX embedding_space ON embeddings(model,preprocessing,asset_id);
              CREATE TRIGGER invalidate_analysis AFTER UPDATE OF revision ON assets WHEN old.revision<>new.revision BEGIN
                DELETE FROM embeddings WHERE asset_id=new.id;
                DELETE FROM analysis_jobs WHERE asset_id=new.id;
              END;
              PRAGMA user_version=3; COMMIT;
            )SQL");
        }
        if (schemaVersion < 4)
        {
            exec(R"SQL(BEGIN IMMEDIATE;
              CREATE TABLE retrieval_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
              INSERT INTO retrieval_meta VALUES('catalog',lower(hex(randomblob(16))));
              CREATE TABLE vector_changes(sequence INTEGER PRIMARY KEY AUTOINCREMENT,asset_id TEXT NOT NULL);
              CREATE TRIGGER vector_change_insert AFTER INSERT ON embeddings BEGIN INSERT INTO vector_changes(asset_id) VALUES(new.asset_id); END;
              CREATE TRIGGER vector_change_delete AFTER DELETE ON embeddings BEGIN INSERT INTO vector_changes(asset_id) VALUES(old.asset_id); END;
              CREATE TRIGGER vector_change_update AFTER UPDATE ON embeddings BEGIN INSERT INTO vector_changes(asset_id) VALUES(new.asset_id); END;
              CREATE TRIGGER vector_change_asset AFTER UPDATE OF state,root_id ON assets
                WHEN (old.state<>new.state OR old.root_id<>new.root_id) AND EXISTS(SELECT 1 FROM embeddings WHERE asset_id=new.id)
                BEGIN INSERT INTO vector_changes(asset_id) VALUES(new.id); END;
              CREATE TRIGGER vector_changes_bound AFTER INSERT ON vector_changes BEGIN
                DELETE FROM vector_changes WHERE sequence<new.sequence-8191; END;
              PRAGMA user_version=4;COMMIT;
            )SQL");
        }
        if (schemaVersion < 5)
            exec("BEGIN IMMEDIATE;ALTER TABLE assets ADD COLUMN channel_layout TEXT NOT NULL DEFAULT "
                 "'';UPDATE assets SET state='pending' WHERE state='ready' AND channels>2;PRAGMA "
                 "user_version=5;COMMIT;");
        if (schemaVersion < 6)
            exec("BEGIN IMMEDIATE;ALTER TABLE assets ADD COLUMN layout_override TEXT NOT NULL DEFAULT "
                 "'';PRAGMA user_version=6;COMMIT;");
        if (schemaVersion < 7)
        {
            exec(R"SQL(BEGIN IMMEDIATE;
              CREATE TABLE collections(id TEXT PRIMARY KEY,parent TEXT NOT NULL DEFAULT '',name TEXT NOT NULL,
                folded TEXT NOT NULL,position INTEGER NOT NULL,UNIQUE(parent,folded));
              CREATE INDEX collection_parent ON collections(parent,position,id);
              CREATE TABLE library_refs(id TEXT PRIMARY KEY,asset TEXT NOT NULL,root TEXT NOT NULL,name TEXT NOT NULL,
                first INTEGER NOT NULL,last INTEGER NOT NULL,revision INTEGER NOT NULL,samples INTEGER NOT NULL,
                rate REAL NOT NULL,snapshot TEXT NOT NULL,catalog TEXT NOT NULL,
                favorite INTEGER NOT NULL DEFAULT 0,recent INTEGER NOT NULL DEFAULT 0,
                UNIQUE(asset,first,last,revision));
              CREATE INDEX reference_asset ON library_refs(asset);
              CREATE INDEX reference_favorites ON library_refs(favorite,name,id);
              CREATE INDEX reference_recent ON library_refs(recent DESC,id);
              CREATE TABLE collection_members(collection TEXT NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
                reference TEXT NOT NULL REFERENCES library_refs(id),PRIMARY KEY(collection,reference));
              CREATE INDEX member_reference ON collection_members(reference,collection);
              CREATE TABLE collection_history(sequence INTEGER PRIMARY KEY AUTOINCREMENT,forward TEXT NOT NULL,inverse TEXT NOT NULL);
              CREATE TABLE collection_cursor(singleton INTEGER PRIMARY KEY CHECK(singleton=1),value INTEGER NOT NULL);
              INSERT INTO collection_cursor VALUES(1,0);
            )SQL");
            if (beforeMigrationCommit)
                beforeMigrationCommit();
            exec("PRAGMA user_version=7;COMMIT;");
        }
        if (schemaVersion < 8)
        {
            exec(R"SQL(BEGIN IMMEDIATE;
            CREATE TABLE IF NOT EXISTS metadata_assets(id TEXT PRIMARY KEY REFERENCES assets(id) ON DELETE CASCADE,revision INTEGER NOT NULL DEFAULT 0,
             embedded TEXT NOT NULL DEFAULT '{}',derived TEXT NOT NULL DEFAULT '{}',overrides TEXT NOT NULL DEFAULT '{}',
             provider TEXT NOT NULL DEFAULT '{}',review INTEGER NOT NULL DEFAULT 0,rejected TEXT NOT NULL DEFAULT '');
            CREATE TABLE IF NOT EXISTS metadata_fields(asset TEXT NOT NULL REFERENCES assets(id) ON DELETE CASCADE,field TEXT NOT NULL,value TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(asset,field));
            CREATE TABLE IF NOT EXISTS metadata_aliases(field TEXT NOT NULL,alias TEXT NOT NULL COLLATE NOCASE,value TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(field,alias));
            CREATE INDEX IF NOT EXISTS metadata_facet ON metadata_fields(field,value,asset);
            CREATE VIRTUAL TABLE IF NOT EXISTS metadata_fts USING fts5(terms,tokenize='unicode61',prefix='2 3 4');
            CREATE TABLE IF NOT EXISTS metadata_backfill(id TEXT PRIMARY KEY,root TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS metadata_backfill_root ON metadata_backfill(root,id);
            INSERT OR IGNORE INTO metadata_backfill SELECT id,root_id FROM assets WHERE state='ready';
            CREATE TABLE IF NOT EXISTS metadata_history(seq INTEGER PRIMARY KEY,rows TEXT NOT NULL,applied INTEGER NOT NULL);
            CREATE TRIGGER IF NOT EXISTS metadata_asset_removed AFTER DELETE ON assets BEGIN DELETE FROM metadata_fts WHERE rowid=OLD.rowid; DELETE FROM metadata_backfill WHERE id=OLD.id;END;
            CREATE INDEX IF NOT EXISTS metadata_audio_facets ON assets(channels,duration,root_id);
            PRAGMA user_version=8;COMMIT;)SQL");
            metadataSchema = true;
        }
        if (schemaVersion < 9)
        {
            exec(R"SQL(BEGIN IMMEDIATE;
                 CREATE TABLE IF NOT EXISTS acoustic_rejections(asset TEXT NOT NULL REFERENCES assets(id) ON DELETE CASCADE,revision INTEGER NOT NULL,policy TEXT NOT NULL,category TEXT NOT NULL,PRIMARY KEY(asset,revision,policy,category));
                 PRAGMA user_version=9;COMMIT;)SQL");
        }
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
            revision = s.integer(4) + (changed ? 1 : 0);
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
                            "pending',duration=NULL,channels=NULL,sample_rate=NULL,error='',"
                            "channel_layout='',layout_override='' WHERE id=?");
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
bool Catalog::setLayoutOverride(const juce::String &id, int64_t revision, const ChannelLayout &layout)
{
    auto a = asset(id);
    if (a.id.isEmpty() || a.revision != revision)
        return false;
    if (layout.id.isNotEmpty())
    {
        validateLayout(layout, 64);
        if (layout.size() != a.channels)
            return false;
    }
    Statement s(db, "UPDATE assets SET layout_override=? WHERE id=? AND revision=?");
    s.bind(1, layout.id.isEmpty() ? juce::String() : juce::JSON::toString(layoutJson(layout), true));
    s.bind(2, id);
    s.bind(3, revision);
    s.step();
    return true;
}
void Catalog::metadata(const Asset &a, double duration, int channels, double rate, const juce::String &error,
                       int64_t sourceSamples)
{
    auto current = asset(a.id);
    auto retained = current.layoutOverride;
    if (retained.size() != channels)
        retained = {};
    Statement s(
        db,
        "UPDATE assets SET "
        "duration=?,channels=?,sample_rate=?,state=?,error=?,samples=?,channel_layout=?,layout_override=? "
        "WHERE id=? AND revision=? AND modified=? AND size=? AND inode=?");
    sqlite3_bind_double(s.s, 1, duration);
    s.bind(2, (int64_t)channels);
    sqlite3_bind_double(s.s, 3, rate);
    s.bind(4, error.isEmpty() ? "ready" : "failed");
    s.bind(5, error);
    s.bind(6, sourceSamples > 0 ? sourceSamples : (int64_t)std::llround(duration * rate));
    juce::var info(new juce::DynamicObject());
    auto layout = a.detectedChannels.id.isEmpty()
                      ? (channels <= 2 ? channelLayout(channels == 1 ? "mono-v1" : "stereo-v1")
                                       : discreteLayout(std::max(1, channels), true))
                      : a.detectedChannels;
    info.getDynamicObject()->setProperty("layout", layoutJson(layout));
    info.getDynamicObject()->setProperty("confidence", a.layoutConfidence);
    s.bind(7, juce::JSON::toString(info, true));
    s.bind(8, retained.id.isEmpty() ? juce::String() : juce::JSON::toString(layoutJson(retained), true));
    s.bind(9, a.id);
    s.bind(10, a.revision);
    s.bind(11, a.modified);
    s.bind(12, a.size);
    s.bind(13, a.inode);
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
#include "CatalogCollections.inc"
#include "CatalogMetadata.inc"
namespace
{
juce::String extraMetadataRows(const juce::String &q, const juce::String &root)
{
    auto sql = " FROM metadata_fts JOIN assets a ON a.rowid=metadata_fts.rowid WHERE metadata_fts MATCH " +
               quoted(matchText(q, {}));
    if (root.isNotEmpty())
        sql += " AND a.root_id=" + quoted(root);
    return sql + " AND a.rowid NOT IN(SELECT rowid FROM names WHERE names MATCH " +
           quoted(matchText(q, root)) + ")";
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
    if (activeScope.isNotEmpty())
    {
        Statement scoped(db,
                         ("SELECT count(*) FROM library_refs lr LEFT JOIN assets a ON a.id=lr.asset WHERE " +
                          referenceWhere(q, root))
                             .toRawUTF8());
        scoped.step();
        return (int)scoped.integer(0);
    }
    if (metadataSchema && isMetadataQuery(q))
    {
        Statement s(db, ("SELECT count(*) FROM assets a WHERE " + metadataWhere(q, root)).toRawUTF8());
        s.step();
        return (int)s.integer(0);
    }
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
    int count = (int)s.integer(0);
    if (metadataSchema && match.isNotEmpty() && !uuidQuery(q))
    {
        Statement extra(db, ("SELECT count(*)" + extraMetadataRows(q, root)).toRawUTF8());
        extra.step();
        count += (int)extra.integer(0);
    }
    return count;
}
std::vector<Asset> Catalog::search(const juce::String &q, const juce::String &root, int limit, int offset)
{
    ++performance.queries;
    if (activeScope.isNotEmpty())
        return scopedRows(q, root, limit, offset);
    std::vector<Asset> result;
    auto collect = [&](Statement &s)
    {
        while (s.step())
            result.push_back(readAsset(s));
    };
    const auto base =
        "SELECT " + juce::String(columns) + favoriteColumn + " FROM assets a JOIN roots r ON r.id=a.root_id ";
    if (metadataSchema && isMetadataQuery(q))
    {
        Statement s(db, (base + " WHERE " + metadataWhere(q, root) +
                         " ORDER BY a.name COLLATE NOCASE,a.id LIMIT ? OFFSET ?")
                            .toRawUTF8());
        s.bind(1, (int64_t)limit);
        s.bind(2, (int64_t)offset);
        collect(s);
        return result;
    }
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
    if (metadataSchema && (int)result.size() < limit)
    {
        int skip = 0;
        if (offset > 0)
        {
            Statement count(db, "SELECT count(*) FROM names WHERE names MATCH ?");
            count.bind(1, match);
            count.step();
            skip = std::max(0, offset - (int)count.integer(0));
        }
        Statement extra(db, (base + " WHERE a.id IN(SELECT a.id" + extraMetadataRows(q, root) +
                             ") ORDER BY a.rowid LIMIT ? OFFSET ?")
                                .toRawUTF8());
        extra.bind(1, (int64_t)(limit - result.size()));
        extra.bind(2, (int64_t)skip);
        collect(extra);
    }
    return result;
}
void Catalog::materialize(const juce::String &q, const juce::String &root, bool empty)
{
    scopedBrowse = activeScope.isNotEmpty();
    if (scopedBrowse)
    {
        scopedMaterialize(q, root, empty);
        return;
    }
    exec("DROP TABLE IF EXISTS temp.browse_ids; CREATE TEMP TABLE browse_ids(position INTEGER PRIMARY KEY,id "
         "TEXT NOT NULL);");
    if (empty)
        return;
    if (metadataSchema && isMetadataQuery(q))
    {
        exec(("INSERT INTO browse_ids(id) SELECT a.id FROM assets a WHERE " + metadataWhere(q, root) +
              " ORDER BY a.name COLLATE NOCASE,a.id")
                 .toRawUTF8());
        return;
    }
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
    if (metadataSchema && match.isNotEmpty() && !uuidQuery(q))
        exec(("INSERT INTO browse_ids(id) SELECT a.id" + extraMetadataRows(q, root) + " ORDER BY a.rowid")
                 .toRawUTF8());
}
std::vector<Asset> Catalog::page(int offset, int limit)
{
    if (scopedBrowse)
    {
        Statement s(
            db,
            ("SELECT " + juce::String(columns) +
             ",lr.id,lr.first,lr.last,lr.revision,lr.samples,lr.rate,lr.snapshot,lr.favorite FROM "
             "browse_refs b JOIN library_refs lr ON lr.id=b.reference LEFT JOIN assets a ON a.id=lr.asset "
             "LEFT JOIN roots r ON r.id=a.root_id WHERE b.position>? ORDER BY b.position LIMIT ?")
                .toRawUTF8());
        s.bind(1, (int64_t)offset);
        s.bind(2, (int64_t)juce::jlimit(1, 512, limit));
        std::vector<Asset> rows;
        while (s.step())
            rows.push_back(readReference(s));
        return rows;
    }
    Statement s(db, ("SELECT " + juce::String(columns) + favoriteColumn +
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
int Catalog::reconcileBrowse()
{
    if (scopedBrowse)
    {
        Statement c(db, "SELECT count(*) FROM browse_refs");
        c.step();
        return (int)c.integer(0);
    }
    bool compact = false;
    {
        Statement missing(
            db, "SELECT count(*) FROM browse_ids b LEFT JOIN assets a ON a.id=b.id WHERE a.id IS NULL");
        missing.step();
        compact = missing.integer(0) > 0;
    }
    if (compact)
        exec("DROP TABLE IF EXISTS temp.remaining_ids; CREATE TEMP TABLE remaining_ids(position INTEGER "
             "PRIMARY KEY,id TEXT NOT NULL);"
             "INSERT INTO remaining_ids(id) SELECT b.id FROM browse_ids b JOIN assets a ON a.id=b.id ORDER "
             "BY b.position;"
             "DROP TABLE browse_ids; ALTER TABLE remaining_ids RENAME TO browse_ids;");
    Statement count(db, "SELECT count(*) FROM browse_ids");
    count.step();
    return (int)count.integer(0);
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
    Statement s(
        db, ("SELECT " + juce::String(columns) +
             " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.root_id=? AND (a.state='pending' " +
             juce::String(metadataSchema ? " OR EXISTS(SELECT 1 FROM metadata_backfill WHERE id=a.id)" : "") +
             ") AND a.online=1 LIMIT ?")
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
Asset Catalog::asset(const juce::String &id)
{
    Statement row(
        db, ("SELECT " + juce::String(columns) + " FROM assets a JOIN roots r ON r.id=a.root_id WHERE a.id=?")
                .toRawUTF8());
    row.bind(1, id);
    return row.step() ? readAsset(row) : Asset{};
}
int Catalog::promote(const std::vector<Asset> &prefix, bool keepLexicalTail)
{
    if (scopedBrowse)
    {
        exec("CREATE TEMP TABLE next_refs(position INTEGER PRIMARY KEY,reference TEXT UNIQUE NOT NULL)");
        for (auto &a : prefix)
        {
            Statement s(db,
                        "INSERT OR IGNORE INTO next_refs(reference) SELECT id FROM library_refs WHERE id=?");
            s.bind(1, a.libraryReference);
            s.step();
        }
        if (keepLexicalTail)
            exec("INSERT OR IGNORE INTO next_refs(reference) SELECT reference FROM browse_refs ORDER BY "
                 "position");
        exec("DROP TABLE browse_refs;ALTER TABLE next_refs RENAME TO browse_refs");
        Statement c(db, "SELECT count(*) FROM browse_refs");
        c.step();
        return (int)c.integer(0);
    }
    exec("DROP TABLE IF EXISTS temp.next_browse_ids; CREATE TEMP TABLE next_browse_ids(position INTEGER "
         "PRIMARY KEY,id TEXT NOT NULL UNIQUE);");
    for (auto &a : prefix)
    {
        Statement insert(
            db, "INSERT OR IGNORE INTO next_browse_ids(id) SELECT id FROM assets WHERE id=? AND revision=?");
        insert.bind(1, a.id);
        insert.bind(2, a.revision);
        insert.step();
    }
    if (keepLexicalTail)
        exec("INSERT OR IGNORE INTO next_browse_ids(id) SELECT id FROM browse_ids ORDER BY position;");
    exec("DROP TABLE browse_ids; ALTER TABLE next_browse_ids RENAME TO browse_ids;");
    Statement count(db, "SELECT count(*) FROM browse_ids");
    count.step();
    return (int)count.integer(0);
}
void Catalog::seedAnalysis(const juce::String &model, const juce::String &preprocessing,
                           const juce::String &id)
{
    Statement s(db,
                ("INSERT OR IGNORE INTO analysis_jobs(asset_id,revision,root_id,model,preprocessing,rate) "
                 "SELECT id,revision,root_id,?,?,sample_rate FROM assets WHERE state='ready' AND samples>0" +
                 (id.isEmpty() ? juce::String() : " AND id=?"))
                    .toRawUTF8());
    s.bind(1, model);
    s.bind(2, preprocessing);
    if (id.isNotEmpty())
        s.bind(3, id);
    s.step();
}
std::optional<AnalysisTask> Catalog::nextAnalysis(const juce::String &model,
                                                  const juce::String &preprocessing,
                                                  const juce::String &preferred)
{
    auto select =
        "SELECT " + juce::String(columns) +
        ",j.next_start FROM analysis_jobs j JOIN assets a ON "
        "a.id=j.asset_id AND a.revision=j.revision JOIN roots r ON r.id=a.root_id WHERE j.model=? "
        "AND j.preprocessing=? AND j.state='pending' AND a.state='ready' AND a.online=1 AND r.online=1 ";
    if (preferred.isNotEmpty())
    {
        Statement s(db, (select + "AND j.asset_id=? LIMIT 1").toRawUTF8());
        s.bind(1, model);
        s.bind(2, preprocessing);
        s.bind(3, preferred);
        if (s.step())
            return AnalysisTask{readAsset(s), s.integer(18)};
    }
    auto sources = roots();
    for (size_t n = 0; n < sources.size(); ++n)
    {
        auto i = (size_t)(analysisRootCursor++ % (int)sources.size());
        if (!sources[i].online)
            continue;
        Statement s(db, (select + "AND j.root_id=? LIMIT 1").toRawUTF8());
        s.bind(1, model);
        s.bind(2, preprocessing);
        s.bind(3, sources[i].id);
        if (s.step())
            return AnalysisTask{readAsset(s), s.integer(18)};
    }
    return {};
}
bool Catalog::commitAnalysis(const AnalysisUpdate &u)
{
    auto a = asset(u.asset.id);
    if (a.id.isEmpty() || a.revision != u.asset.revision || a.modified != u.asset.modified ||
        a.size != u.asset.size || a.inode != u.asset.inode || a.state != "ready" ||
        a.samples != u.asset.samples || a.sampleRate != u.asset.sampleRate)
        return false;
    if (u.error.isEmpty())
    {
        if (u.embedding.size() != 512 || u.start < 0 || u.end <= u.start || u.end > a.samples)
            throw std::runtime_error("Invalid derived source range or vector");
        for (auto v : u.embedding)
            if (!std::isfinite(v))
                throw std::runtime_error("Nonfinite embedding");
        Statement s(db, "INSERT OR REPLACE INTO "
                        "embeddings(asset_id,revision,model,preprocessing,start_sample,end_sample,rate,"
                        "vector) VALUES(?,?,?,?,?,?,?,?)");
        s.bind(1, a.id);
        s.bind(2, a.revision);
        s.bind(3, u.model);
        s.bind(4, u.preprocessing);
        s.bind(5, u.start);
        s.bind(6, u.end);
        sqlite3_bind_double(s.s, 7, a.sampleRate);
        sqlite3_bind_blob(s.s, 8, u.embedding.data(), 2048, SQLITE_TRANSIENT);
        s.step();
    }
    Statement job(
        db,
        "INSERT INTO "
        "analysis_jobs(asset_id,revision,root_id,model,preprocessing,state,next_start,max_end,rate,error) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(asset_id,revision,model,preprocessing) DO UPDATE SET "
        "state=excluded.state,next_start=excluded.next_start,max_end=max(analysis_jobs.max_end,excluded.max_"
        "end),error=excluded.error");
    job.bind(1, a.id);
    job.bind(2, a.revision);
    job.bind(3, a.rootId);
    job.bind(4, u.model);
    job.bind(5, u.preprocessing);
    job.bind(6, u.error.isNotEmpty() ? "failed" : u.done ? "complete" : "pending");
    job.bind(7, u.error.isEmpty() ? u.nextStart : u.start);
    job.bind(8, u.error.isEmpty() ? u.end : 0);
    sqlite3_bind_double(job.s, 9, a.sampleRate);
    job.bind(10, u.error);
    job.step();
    return true;
}
AnalysisCounts Catalog::analysisCounts(const juce::String &model, const juce::String &preprocessing)
{
    AnalysisCounts c;
    {
        Statement s(db, "SELECT count(*) FROM assets WHERE state='ready' AND samples>0");
        s.step();
        c.total = (int)s.integer(0);
    }
    {
        Statement s(db, "SELECT sum(j.state='complete'),sum(j.state='failed'),sum(j.max_end/j.rate) "
                        "FROM analysis_jobs j JOIN assets a ON a.id=j.asset_id AND a.revision=j.revision "
                        "WHERE j.model=? AND j.preprocessing=?");
        s.bind(1, model);
        s.bind(2, preprocessing);
        s.step();
        c.complete = (int)s.integer(0);
        c.failed = (int)s.integer(1);
        c.analyzedSeconds = s.real(2);
    }
    {
        Statement s(db, "SELECT count(*),coalesce(sum(length(e.vector)),0) FROM embeddings e JOIN assets a "
                        "ON a.id=e.asset_id AND a.revision=e.revision WHERE e.model=? AND e.preprocessing=?");
        s.bind(1, model);
        s.bind(2, preprocessing);
        s.step();
        c.regions = (int)s.integer(0);
        c.vectorBytes = s.integer(1);
    }
    c.pending = std::max(0, c.total - c.complete - c.failed);
    return c;
}
void Catalog::retryAnalysis(const juce::String &model, const juce::String &preprocessing)
{
    Statement s(db, "UPDATE analysis_jobs SET state='pending',error='' WHERE model=? AND preprocessing=? AND "
                    "state='failed'");
    s.bind(1, model);
    s.bind(2, preprocessing);
    s.step();
}
IndexState Catalog::indexState()
{
    IndexState state;
    Statement id(db, "SELECT value FROM retrieval_meta WHERE key='catalog'");
    if (id.step())
        state.catalog = id.str(0);
    Statement latest(db, "SELECT seq FROM sqlite_sequence WHERE name='vector_changes'");
    if (latest.step())
        state.sequence = latest.integer(0);
    Statement oldest(db, "SELECT min(sequence) FROM vector_changes");
    oldest.step();
    state.oldest = oldest.integer(0);
    if (!state.oldest)
        state.oldest = state.sequence + 1;
    return state;
}
juce::StringArray Catalog::indexChanges(int64_t since)
{
    juce::StringArray ids;
    Statement s(db, "SELECT DISTINCT asset_id FROM vector_changes WHERE sequence>?");
    s.bind(1, since);
    while (s.step())
        ids.add(s.str(0));
    return ids;
}
void Catalog::visitRegions(const juce::String &model, const juce::String &preprocessing,
                           const juce::String &assetId,
                           const std::function<void(const StoredRegion &)> &visit)
{
    Statement s(
        db,
        ("SELECT e.asset_id,a.root_id,e.revision,e.start_sample,e.end_sample,e.vector FROM embeddings e "
         "JOIN assets a ON a.id=e.asset_id AND a.revision=e.revision WHERE e.model=? AND e.preprocessing=? "
         "AND a.state='ready'" +
         (assetId.isEmpty() ? juce::String() : " AND e.asset_id=?") + " ORDER BY e.asset_id,e.start_sample")
            .toRawUTF8());
    s.bind(1, model);
    s.bind(2, preprocessing);
    if (assetId.isNotEmpty())
        s.bind(3, assetId);
    while (s.step())
    {
        if (sqlite3_column_bytes(s.s, 5) != 2048)
            continue;
        StoredRegion row;
        row.asset = s.str(0);
        row.root = s.str(1);
        row.revision = s.integer(2);
        row.first = s.integer(3);
        row.last = s.integer(4);
        std::memcpy(row.vector.data(), sqlite3_column_blob(s.s, 5), 2048);
        visit(row);
    }
}
std::vector<Asset> Catalog::semanticSearch(const std::vector<float> &query, const juce::String &model,
                                           const juce::String &preprocessing, const juce::String &root,
                                           int limit, RetrievalMetrics *metrics,
                                           const juce::String &excludeAsset)
{
    auto began = monotonicMs();
    RetrievalMetrics measured;
    if (query.size() != 512)
        throw std::runtime_error("Invalid query embedding dimensions");
    RegionRanking ranking(limit);
    auto eligible = scopeRanges(root);
    Statement rows(
        db, ("SELECT e.asset_id,e.revision,e.start_sample,e.end_sample,e.vector FROM embeddings e "
             "JOIN assets a ON a.id=e.asset_id AND a.revision=e.revision JOIN roots r ON r.id=a.root_id "
             "WHERE e.model=? AND e.preprocessing=? AND a.state='ready'" +
             (root.isEmpty() ? juce::String() : " AND a.root_id=?") +
             (excludeAsset.isEmpty() ? juce::String() : " AND a.id<>?") +
             (activeScope.isEmpty() ? juce::String()
                                    : " AND a.id IN(SELECT lr.asset FROM library_refs lr LEFT JOIN assets a "
                                      "ON a.id=lr.asset WHERE " +
                                          referenceWhere({}, root) + ")") +
             " ORDER BY e.asset_id,e.start_sample")
                .toRawUTF8());
    rows.bind(1, model);
    rows.bind(2, preprocessing);
    int binding = 3;
    if (root.isNotEmpty())
        rows.bind(binding++, root);
    if (excludeAsset.isNotEmpty())
        rows.bind(binding, excludeAsset);
    int seen = 0;
    measured.fetchFilterMs = monotonicMs() - began;
    for (;;)
    {
        auto stepAt = monotonicMs();
        bool row = rows.step();
        measured.fetchFilterMs += monotonicMs() - stepAt;
        if (!row)
            break;
        if ((++seen % 64) == 0 && cancellation && cancellation->load() != expectedGeneration)
            throw QueryCancelled();
        if (sqlite3_column_bytes(rows.s, 4) != 2048)
            continue;
        auto found = eligible.find(rows.str(0));
        if (restrictedRanking() && found == eligible.end())
            continue;
        auto first = rows.integer(2), last = rows.integer(3);
        if (activeScope.isNotEmpty() &&
            std::none_of(found->second.begin(), found->second.end(),
                         [&](const auto &r) { return first < r.endSample && last > r.startSample; }))
            continue;
        auto *vector = static_cast<const float *>(sqlite3_column_blob(rows.s, 4));
        auto scoreAt = monotonicMs();
        float score = 0;
        vDSP_dotpr(query.data(), 1, vector, 1, &score, 512);
        measured.vectorScoreMs += monotonicMs() - scoreAt;
        ++measured.vectors;
        if (activeScope.isEmpty())
            ranking.add(rows.str(0), rows.integer(1), first, last, score);
        else
            for (auto &r : found->second)
                if (first < r.endSample && last > r.startSample)
                    ranking.add(rows.str(0), rows.integer(1), std::max(first, r.startSample),
                                std::min(last, r.endSample), score);
    }
    auto result = ranking.finish(*this, root);
    measured.rankGroupMs =
        std::max(0.0, monotonicMs() - began - measured.fetchFilterMs - measured.vectorScoreMs);
    if (metrics)
        *metrics = measured;
    return result;
}
} // namespace sp
