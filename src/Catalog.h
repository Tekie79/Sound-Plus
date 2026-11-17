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
    void rootState(const Root &);
    void begin();
    void commit();
    bool discover(const Root &, const juce::String &relative, const FileStamp &, int64_t scan);
    void finishScan(const Root &, int64_t scan);
    std::vector<Asset> pending(int limit = 24);
    void metadata(const Asset &, double duration, int channels, double rate, const juce::String &error);
    std::vector<Asset> search(const juce::String &, const juce::String &root = {}, int limit = 1000);
    int count();
    void exec(const char *);

  private:
    sqlite3 *db = nullptr;
};
} // namespace sp
