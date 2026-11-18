#pragma once
#include <juce_core/juce_core.h>
#include <sqlite3.h>
#include <vector>

namespace sp
{
struct Root
{
    juce::String id, path, volume, volumeRelative, error;
    bool online = false, scanning = false;
    int discovered = 0, ready = 0, failed = 0;
};
struct Asset
{
    juce::String id, rootId, relative, name, path, state, error;
    int64_t size = 0, modified = 0, inode = 0, revision = 1;
    double duration = 0, sampleRate = 0;
    int channels = 0;
    bool online = false;
};
struct FileStamp
{
    int64_t size = 0, modified = 0, inode = 0;
};
struct DirectoryRecord
{
    juce::String relative;
    int64_t modified = 0, inode = 0;
    juce::StringArray children;
    bool valid = false;
};
struct QueryCancelled : std::exception
{
    const char *what() const noexcept override
    {
        return "Query cancelled";
    }
};
bool fileStamp(const juce::File &, FileStamp &);
bool supportedExtension(const juce::File &);

// Each connection belongs to exactly one worker. UI and audio never call SQLite.
class Catalog
{
  public:
    explicit Catalog(const juce::File &, bool readOnly = false);
    ~Catalog();
    Catalog(const Catalog &) = delete;
    Catalog &operator=(const Catalog &) = delete;
    std::vector<Root> roots();
    Root addRoot(const juce::File &);
    Root addRootRecord(const juce::File &);
    void removeEmptyRoot(const juce::String &);
    void rootState(const Root &);
    void begin();
    void commit();
    void rollback();
    bool discover(const Root &, const juce::String &relative, const FileStamp &, int64_t scan,
                  bool legacyRelink = true, const juce::String &confirmedRename = {});
    void finishScan(const Root &, int64_t scan);
    std::vector<Asset> pending(int limit = 24);
    void metadata(const Asset &, double duration, int channels, double rate, const juce::String &error);
    std::vector<Asset> search(const juce::String &, const juce::String &root = {}, int limit = 1000,
                              int offset = 0);
    int countMatches(const juce::String &, const juce::String &root = {});
    void materialize(const juce::String &, const juce::String &root = {});
    std::vector<Asset> page(int offset, int limit);
    Asset existing(const juce::String &root, const juce::String &relative);
    std::vector<Asset> inodeCandidates(const juce::String &root, int64_t inode);
    std::vector<Asset> pendingRoot(const juce::String &, int limit);
    DirectoryRecord directory(const juce::String &root, const juce::String &relative);
    void directoryDone(const Root &, const DirectoryRecord &, int64_t scan);
    void removeTree(const juce::String &root, const juce::String &relative);
    void cancelWhen(const std::atomic<uint64_t> *generation, uint64_t expected);
    int count();
    void exec(const char *);

  private:
    sqlite3 *db = nullptr;
    const std::atomic<uint64_t> *cancellation = nullptr;
    uint64_t expectedGeneration = 0;
};
} // namespace sp
