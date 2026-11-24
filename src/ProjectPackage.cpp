#include "ProjectPackage.h"
#include "PluginProcessing.h"
#include <juce_cryptography/juce_cryptography.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
namespace sp
{
namespace
{
using namespace juce;
constexpr int64_t maxBytes = int64_t(512) << 30;
void require(bool ok, const String &message)
{
    if (!ok)
        throw std::runtime_error(message.toStdString());
}
void put(var &v, const char *k, const var &x)
{
    v.getDynamicObject()->setProperty(k, x);
}
var obj()
{
    return var(new DynamicObject);
}
String jsonHash(const var &v)
{
    auto s = JSON::toString(v, true);
    return SHA256(s.toRawUTF8(), s.getNumBytesAsUTF8()).toHexString();
}
bool digest(const String &s)
{
    return s.length() == 64 && s.containsOnly("0123456789abcdef");
}
struct Handle
{
    int fd;
    explicit Handle(int f) : fd(f) {}
    ~Handle()
    {
        if (fd >= 0)
            ::close(fd);
    }
    Handle(const Handle &) = delete;
    void reset(int next)
    {
        if (fd >= 0)
            ::close(fd);
        fd = next;
    }
};
void cancelledNow(const std::function<bool()> &f)
{
    if (f && f())
        throw QueryCancelled();
}
void safePart(const String &s)
{
    require(s.isNotEmpty() && s.length() < 160 && !s.startsWithChar('.') &&
                s.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._"),
            "Unsafe package path");
}
Handle openRelative(const File &root, const String &relative)
{
    auto parts = StringArray::fromTokens(relative, "/", "");
    require(parts.size() > 0 && parts.size() <= 4 && parts.joinIntoString("/") == relative,
            "Invalid package reference");
    Handle dir(::open(root.getFullPathName().toRawUTF8(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    require(dir.fd >= 0, "Package root unavailable");
    for (int i = 0; i < parts.size(); ++i)
    {
        safePart(parts[i]);
        int fd = ::openat(dir.fd, parts[i].toRawUTF8(),
                          O_RDONLY | O_NOFOLLOW | O_NONBLOCK | (i + 1 < parts.size() ? O_DIRECTORY : 0));
        require(fd >= 0, "Missing or unsafe package member " + relative);
        dir.reset(fd);
    }
    struct stat st
    {
    };
    require(!::fstat(dir.fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1,
            "Invalid or linked package member " + relative);
    return Handle(std::exchange(dir.fd, -1));
}
String stream(int input, int output, int64_t size, const std::function<bool()> &cancel,
              const std::function<void(int64_t)> &advance = {})
{
    CC_SHA256_CTX sha;
    CC_SHA256_Init(&sha);
    std::array<unsigned char, 65536> buffer;
    int64_t total = 0;
    for (;;)
    {
        cancelledNow(cancel);
        auto n = ::read(input, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR)
            continue;
        require(n >= 0, "Source read failed");
        if (n == 0)
            break;
        total += n;
        require(total <= size, "Source grew during collection");
        CC_SHA256_Update(&sha, buffer.data(), (CC_LONG)n);
        if (output >= 0)
            for (ssize_t sent = 0; sent < n;)
            {
                auto w = ::write(output, buffer.data() + sent, n - sent);
                if (w < 0 && errno == EINTR)
                    continue;
                require(w > 0, "Package write failed; check free space");
                sent += w;
            }
        if (advance)
            advance(n);
    }
    require(total == size, "Source length changed");
    unsigned char result[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(result, &sha);
    return String::toHexString(result, sizeof(result), 0);
}
bool matching(const FileStamp &a, const FileStamp &b)
{
    return a.size == b.size && a.modified == b.modified && a.inode == b.inode;
}
String readSource(const File &f, const FileStamp &expected, int out, const std::function<bool()> &cancel,
                  const std::function<void(int64_t)> &advance = {})
{
    FileStamp stamp;
    require(fileStamp(f, stamp) && matching(stamp, expected),
            "Source changed or missing: " + f.getFileName());
    Handle in(::open(f.getFullPathName().toRawUTF8(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    struct stat st
    {
    };
    require(in.fd >= 0 && !::fstat(in.fd, &st) && S_ISREG(st.st_mode), "Cannot collect " + f.getFileName());
    auto hash = stream(in.fd, out, stamp.size, cancel, advance);
    require(fileStamp(f, stamp) && matching(stamp, expected),
            "Source changed while reading: " + f.getFileName());
    return hash;
}
void writeJson(const File &f, const var &v)
{
    auto str = JSON::toString(v, true);
    require(str.getNumBytesAsUTF8() <= 64 * 1024 * 1024, "Package metadata too large");
    auto out = f.createOutputStream();
    require(out && out->write(str.toRawUTF8(), str.getNumBytesAsUTF8()), "Cannot write package metadata");
    out->flush();
    require(out->getStatus().wasOk(), "Cannot flush package metadata");
    Handle fd(::open(f.getFullPathName().toRawUTF8(), O_RDONLY));
    require(fd.fd >= 0 && !::fsync(fd.fd), "Cannot sync package metadata");
}
var readJson(const File &root, const String &name)
{
    auto fd = openRelative(root, name);
    struct stat st
    {
    };
    require(!::fstat(fd.fd, &st) && st.st_size > 0 && st.st_size <= 64 * 1024 * 1024,
            "Invalid package metadata size");
    MemoryBlock bytes((size_t)st.st_size);
    size_t n = 0;
    while (n < bytes.getSize())
    {
        auto got = ::read(fd.fd, (char *)bytes.getData() + n, bytes.getSize() - n);
        require(got > 0, "Cannot read package metadata");
        n += got;
    }
    auto v = JSON::parse(String::fromUTF8((const char *)bytes.getData(), (int)n));
    require(v.isObject(), "Malformed package metadata");
    return v;
}
using MapFile = std::function<String(const String &, const String &, const String &, var &)>;
void relocate(var &v, const MapFile &map, int depth = 0, const File &package = {})
{
    require(depth <= 80, "Excessive document nesting");
    if (auto *a = v.getArray())
    {
        for (auto &row : *a)
            relocate(row, map, depth + 1, package);
        return;
    }
    auto *o = v.getDynamicObject();
    if (!o)
        return;
    auto path = v["path"].toString();
    if (v.hasProperty("fingerprint") && v.hasProperty("format") && v.hasProperty("id"))
    {
        for (auto key : {"path", "bundle"})
        {
            auto value = v[key].toString();
            if (package == File() && File::isAbsolutePath(value))
                put(v, key, "installed-plugin:" + File(value).getFileName());
            else if (package != File() && value.startsWith("installed-plugin:"))
            {
                auto name = value.fromFirstOccurrenceOf(":", false, false);
                require(name == File::createLegalFileName(name) && !name.startsWithChar('.'),
                        "Invalid plug-in reference");
                auto candidate = package.getChildFile(".unavailable-plugins").getChildFile(name);
                auto parent = v["format"].toString() == "AudioUnit" ? "Components" : "VST3";
                for (auto library :
                     {File::getSpecialLocation(File::userHomeDirectory).getChildFile("Library"),
                      File("/Library"), File("/System/Library")})
                {
                    auto f = library.getChildFile("Audio/Plug-Ins").getChildFile(parent).getChildFile(name);
                    if (f.isDirectory() && pluginFingerprint(f) == v["fingerprint"].toString())
                    {
                        candidate = f;
                        break;
                    }
                }
                put(v, key, candidate.getFullPathName());
            }
            else if (package != File())
                require(!File::isAbsolutePath(value), "External plug-in path in package");
        }
        if (auto xml = parseXML(v["xml"].toString()))
        {
            xml->setAttribute("file", v["path"].toString());
            put(v, "xml", xml->toString());
        }
        return;
    }
    if (path.isNotEmpty())
    {
        if (v.hasProperty("samples") || v.hasProperty("frames"))
        {
            auto kind = v.hasProperty("mapping") ? "IR" : v.hasProperty("settings") ? "Renders" : "Media";
            auto mapped = map(path, kind, v["sha256"].toString(), v);
            put(v, "path", mapped);
        }
        else if (v.hasProperty("format") && v.hasProperty("id"))
        {
            if (File::isAbsolutePath(path))
                put(v, "path", "unresolved-plugin:" + File(path).getFileName());
        }
        else if (v.hasProperty("sha256"))
        {
            auto mapped = map(path, "Renders", v["sha256"].toString(), v);
            put(v, "path", mapped);
        }
        else
            require(false, "Unrecognized file reference: " + path);
    }
    if (v.hasProperty("effectivePath"))
    {
        var source = obj();
        auto mapped = map(v["effectivePath"].toString(), "Media", v["sha256"].toString(), source);
        put(v, "effectivePath", mapped);
    }
    if (v.hasProperty("external") && v["source"].isObject())
        put(v, "external", true);
    if (auto source = v["source"]; source.hasProperty("path") && v.hasProperty("sha256"))
    {
        // A pool hash lives on the entry; check it even if the bytes were already deduplicated.
        auto sr = source["path"].toString();
        map(sr, "Media", v["sha256"].toString(), source);
    }
    if (v.hasProperty("checkpoint"))
        put(v, "checkpoint", "");
    if (v.hasProperty("overridePath"))
        put(v, "overridePath", "");
    if (v.hasProperty("journal") && v.hasProperty("sha256"))
    {
        var tmp = v;
        put(v, "journal", map(v["journal"].toString(), "Receipts", "", tmp));
    }
    if (v.hasProperty("receipt") && v.hasProperty("recovered"))
    {
        var receipt = v;
        auto mapped = map(v["receipt"].toString(), "Receipts", "", receipt);
        put(v, "receipt", mapped);
    }
    for (auto &p : o->getProperties())
    {
        auto k = p.name.toString();
        if (k == "path" || k == "receipt" || k == "journal" || k == "effectivePath")
            continue;
        if (p.value.isString())
            require(!File::isAbsolutePath(p.value.toString()), "Unclassified absolute reference: " + k);
        else
        {
            auto child = p.value;
            relocate(child, map, depth + 1, package);
        }
    }
}
void refreshTakeDigests(var &project, bool recordOrigin)
{
    if (auto *a = project["takes"].getArray())
        for (auto &t : *a)
        {
            if (recordOrigin && !t.hasProperty("collectedFromRecipe"))
                put(t, "collectedFromRecipe", t["recipeHash"]);
            put(t, "recipeHash", jsonHash(t["recipe"]));
        }
}
} // namespace
PackagePlan planPackage(const DesignDocument &document, bool outputs, const std::function<bool()> &cancel)
{
    validateDesign(document);
    for (auto &t : document.takes)
    {
        validateTake(t);
        verifyTakeSources(t);
    }
    PackagePlan p;
    p.project = designToJson(document).clone();
    p.project.getDynamicObject()->removeProperty("outputSelections");
    put(p.project, "outputFolder", "");
    if (!outputs)
        put(p.project, "outputs", Array<var>{});
    if (auto *takes = p.project["takes"].getArray())
        for (auto &t : *takes)
        {
            std::map<String, std::pair<File, String>> overrides;
            for (auto &row : *t["sources"].getArray())
                if (row["overridePath"].toString().isNotEmpty())
                    overrides.emplace(
                        row["id"].toString(),
                        std::make_pair(File(row["overridePath"].toString()), row["sha256"].toString()));
            std::function<void(var)> visit = [&](var node)
            {
                if (auto *rows = node.getArray())
                {
                    for (auto &r : *rows)
                        visit(r);
                    return;
                }
                if (auto *o = node.getDynamicObject())
                {
                    auto at = overrides.find(node["id"].toString());
                    if (node.hasProperty("inode") && node.hasProperty("path") && at != overrides.end())
                    {
                        auto &[f, sha] = at->second;
                        FileStamp st;
                        require(fileStamp(f, st) && readSource(f, st, -1, cancel) == sha,
                                "Take relink checksum failed");
                        put(node, "path", f.getFullPathName());
                        put(node, "inode", (int64)st.inode);
                        put(node, "modified", (int64)st.modified);
                        put(node, "size", (int64)st.size);
                    }
                    for (auto &prop : o->getProperties())
                        visit(prop.value);
                }
            };
            if (!overrides.empty())
                visit(t);
        }
    std::map<String, size_t> byPath;
    std::map<String, String> content;
    relocate(
        p.project,
        [&](const String &path, const String &kind, const String &hash, var &ref)
        {
            require(File::isAbsolutePath(path), "Expected a verified local source");
            File f(path);
            FileStamp stamp;
            require(fileStamp(f, stamp), "Missing dependency: " + f.getFileName());
            if (ref.hasProperty("inode"))
                require((int64_t)ref["inode"] == stamp.inode && (int64_t)ref["size"] == stamp.size &&
                            (int64_t)ref["modified"] == stamp.modified,
                        "Changed dependency: " + f.getFileName());
            auto it = byPath.find(path);
            if (it == byPath.end())
            {
                require(p.files.size() < 16384 && stamp.size >= 0 && stamp.size <= maxBytes,
                        "Package size or file limit");
                if (kind == "Receipts")
                {
                    require(stamp.size > 0 && stamp.size <= 64 * 1024 * 1024, "Recording record too large");
                    auto originalHash = readSource(f, stamp, -1, cancel);
                    auto derived = obj();
                    put(derived, "contract", "soundplus-recording-origin-v1");
                    put(derived, "sourceRecordSha256", originalHash);
                    put(derived, "originId", ref["id"]);
                    put(derived, "eventSha256", ref["sha256"]);
                    put(derived, "recovered", ref["recovered"]);
                    if (f.getFileName() == "receipt.json")
                    {
                        auto raw = JSON::parse(f);
                        require(raw["plan"]["id"].toString() == ref["id"].toString() &&
                                    (raw["state"].toString() == "complete" ||
                                     raw["state"].toString() == "recovered"),
                                "Unfinalized recording receipt");
                    }
                    auto text = JSON::toString(derived, true);
                    auto sum = jsonHash(derived);
                    // Repackaging an already-scoped receipt keeps its bytes and provenance.
                    if (f.getFileExtension() == ".json" && stamp.size < 16384)
                    {
                        auto raw = JSON::parse(f);
                        if (raw["contract"].toString() == "soundplus-recording-origin-v1")
                        {
                            text = JSON::toString(raw, true);
                            sum = jsonHash(raw);
                        }
                    }
                    auto relative = "Receipts/" + sum + ".json";
                    p.files.push_back({f, relative, sum, stamp, false, text, originalHash});
                    byPath[path] = p.files.size() - 1;
                    if (content.emplace(sum, relative).second)
                        p.uniqueBytes += text.getNumBytesAsUTF8();
                    return relative;
                }
                require(supportedExtension(f), "Only recognized source audio may be collected");
                auto sha = readSource(f, stamp, -1, cancel);
                auto where = content.find(sha);
                if (where == content.end())
                {
                    p.uniqueBytes += stamp.size;
                    require(p.uniqueBytes <= maxBytes, "Package exceeds size budget");
                    auto ext = f.getFileExtension().toLowerCase();
                    safePart("audio" + ext);
                    where = content.emplace(sha, kind + "/" + sha.substring(0, 2) + "/" + sha + ext).first;
                }
                it = byPath.emplace(path, p.files.size()).first;
                p.files.push_back({f, where->second, sha, stamp, kind == "Renders"});
            }
            auto &entry = p.files[it->second];
            require(hash.isEmpty() || hash == entry.sha256, "Source checksum changed: " + f.getFileName());
            return entry.relative;
        });
    refreshTakeDigests(p.project, true);
    return p;
}
File collectPackage(const PackagePlan &p, const std::set<String> &approved, const File &destination,
                    const std::function<bool()> &cancel, const PackageProgress &progress)
{
    require(!destination.exists() && destination.getParentDirectory().isDirectory(),
            "Choose a new package folder");
    for (auto &f : p.files)
        require(approved.contains(f.original.getFullPathName()),
                "File needs inclusion approval: " + f.original.getFileName());
    require(destination.getParentDirectory().getBytesFreeOnVolume() >= p.uniqueBytes,
            "Insufficient package space");
    TemporaryFile stage(destination.getSiblingFile(".collect-" + Uuid().toString()));
    auto dir = stage.getFile();
    require(::mkdir(dir.getFullPathName().toRawUTF8(), 0700) == 0, "Cannot stage package");
    std::set<String> written;
    Array<var> files;
    int64_t done = 0;
    try
    {
        for (auto &f : p.files)
        {
            cancelledNow(cancel);
            auto file = dir.getChildFile(f.relative);
            if (!written.insert(f.relative).second)
            {
                require(readSource(f.original, f.stamp, -1, cancel) ==
                            (f.derived.isEmpty() ? f.sha256 : f.originalHash),
                        "Deduplicated source changed");
                continue;
            }
            require(file.getParentDirectory().createDirectory().wasOk(), "Cannot create payload folder");
            Handle out(::open(file.getFullPathName().toRawUTF8(), O_CREAT | O_EXCL | O_WRONLY, 0600));
            require(out.fd >= 0, "Cannot create package payload");
            auto sha = f.sha256;
            auto size = f.stamp.size;
            if (f.derived.isNotEmpty())
            {
                require(readSource(f.original, f.stamp, -1, cancel) == f.originalHash,
                        "Recording receipt changed");
                size = f.derived.getNumBytesAsUTF8();
                require(::write(out.fd, f.derived.toRawUTF8(), size) == size,
                        "Cannot publish scoped recording receipt");
                done += size;
            }
            else
                sha = readSource(f.original, f.stamp, out.fd, cancel,
                                 [&](int64_t n)
                                 {
                                     done += n;
                                     if (progress)
                                         progress(done, p.uniqueBytes);
                                 });
            require(sha == f.sha256 && !::fsync(out.fd), "Copied file failed verification");
            auto row = obj();
            put(row, "file", f.relative);
            put(row, "sha256", sha);
            put(row, "bytes", (int64)size);
            files.add(row);
        }
        auto manifest = obj();
        put(manifest, "version", 1);
        put(manifest, "product", JUCE_APPLICATION_VERSION_STRING);
        put(manifest, "files", files);
        put(manifest, "projectSha256", jsonHash(p.project));
        writeJson(dir.getChildFile("Project.soundplus"), p.project);
        auto env = obj();
        put(env, "manifest", manifest);
        put(env, "sha256", jsonHash(manifest));
        writeJson(dir.getChildFile("Manifest.json"), env);
        openPackage(dir, cancel);
        cancelledNow(cancel);
        require(!::renamex_np(dir.getFullPathName().toRawUTF8(), destination.getFullPathName().toRawUTF8(),
                              RENAME_EXCL),
                "Package destination exists or could not be published");
    }
    catch (...)
    {
        dir.deleteRecursively();
        throw;
    }
    return destination;
}
DesignDocument openPackage(const File &root, const std::function<bool()> &cancel)
{
    auto env = readJson(root, "Manifest.json");
    auto m = env["manifest"];
    require((int)m["version"] == 1 && jsonHash(m) == env["sha256"].toString(),
            "Package manifest checksum failed");
    auto project = readJson(root, "Project.soundplus");
    require(jsonHash(project) == m["projectSha256"].toString(), "Packaged project checksum failed");
    auto *files = m["files"].getArray();
    require(files && files->size() <= 16384, "Package member limit");
    std::map<String, std::pair<String, FileStamp>> known;
    std::set<String> folded;
    int64_t total = 0;
    for (auto &f : *files)
    {
        auto rel = f["file"].toString(), sha = f["sha256"].toString();
        auto size = (int64_t)f["bytes"];
        require(digest(sha) && size >= 0 && size <= maxBytes - total &&
                    folded.insert(rel.toLowerCase()).second,
                "Invalid package member");
        total += size;
        auto fd = openRelative(root, rel);
        require(stream(fd.fd, -1, size, cancel) == sha, "Damaged package member: " + rel);
        FileStamp stamp;
        require(fileStamp(root.getChildFile(rel), stamp), "Cannot inspect package member");
        known.emplace(rel, std::make_pair(sha, stamp));
    }
    relocate(
        project,
        [&](const String &rel, const String &, const String &expected, var &ref)
        {
            auto it = known.find(rel);
            require(it != known.end() && (expected.isEmpty() || expected == it->second.first),
                    "Unverified package reference: " + rel);
            if (ref.hasProperty("inode"))
            {
                auto &st = it->second.second;
                put(ref, "size", (int64)st.size);
                put(ref, "inode", (int64)st.inode);
                put(ref, "modified", (int64)st.modified);
            }
            return root.getChildFile(rel).getFullPathName();
        },
        0, root);
    refreshTakeDigests(project, false);
    return designFromJson(project);
}
} // namespace sp
