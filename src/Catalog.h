#pragma once
#include <juce_core/juce_core.h>
#include <sqlite3.h>
#include "ChannelLayout.h"
#include <vector>
#include <optional>
#include <array>
#include <functional>

namespace sp
{
struct Root
{
    juce::String id, path, volume, volumeRelative, error;
    bool online = false, scanning = false;
    int discovered = 0, ready = 0, failed = 0;
};
struct MatchRegion
{
    int64_t startSample = 0, endSample = 0;
    float similarity = 0;
    int groupedWindows = 1;
};
struct Asset
{
    juce::String id, rootId, relative, name, path, state, error;
    int64_t size = 0, modified = 0, inode = 0, revision = 1;
    double duration = 0, sampleRate = 0;
    int channels = 0;
    ChannelLayout detectedChannels, layoutOverride;
    juce::String layoutConfidence;
    int waveformChannel = -1; // Derivative request only, never catalog/source identity.
    juce::String waveformLayoutKey;
    bool online = false;
    int64_t samples = 0;
    juce::String matchReason;
    std::vector<MatchRegion> matches;
};
struct AnalysisTask { Asset asset; int64_t nextStart = 0; };
struct AnalysisUpdate
{
    Asset asset;
    juce::String model, preprocessing, error;
    int64_t start = 0, end = 0, nextStart = 0;
    bool done = false;
    std::vector<float> embedding;
};
struct AnalysisCounts
{
    int total = 0, complete = 0, pending = 0, failed = 0, regions = 0;
    int64_t vectorBytes = 0;
    double analyzedSeconds = 0;
};
struct RetrievalMetrics { double fetchFilterMs = 0, vectorScoreMs = 0, rankGroupMs = 0; int vectors = 0; };
struct StoredRegion { juce::String asset,root;int64_t revision=0,first=0,last=0;std::array<float,512> vector; };
struct IndexState { juce::String catalog;int64_t sequence=0,oldest=1; };
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
    bool setLayoutOverride(const juce::String &, int64_t, const ChannelLayout &);
    void metadata(const Asset &, double duration, int channels, double rate, const juce::String &error,
                  int64_t sourceSamples = 0);
    std::vector<Asset> search(const juce::String &, const juce::String &root = {}, int limit = 1000,
                              int offset = 0);
    int countMatches(const juce::String &, const juce::String &root = {});
    void materialize(const juce::String &, const juce::String &root = {}, bool empty = false);
    std::vector<Asset> page(int offset, int limit);
    int reconcileBrowse(); // Remove deleted identities while retaining remaining order.
    Asset asset(const juce::String &id);
    int promote(const std::vector<Asset> &prefix, bool keepLexicalTail = true);
    void seedAnalysis(const juce::String &model, const juce::String &preprocessing,
                       const juce::String &assetId = {});
    std::optional<AnalysisTask> nextAnalysis(const juce::String &model, const juce::String &preprocessing,
                                            const juce::String &preferred = {});
    bool commitAnalysis(const AnalysisUpdate &);
    AnalysisCounts analysisCounts(const juce::String &model, const juce::String &preprocessing);
    void retryAnalysis(const juce::String &model, const juce::String &preprocessing);
    IndexState indexState();
    juce::StringArray indexChanges(int64_t since);
    void visitRegions(const juce::String &model,const juce::String &preprocessing,
                      const juce::String &asset,const std::function<void(const StoredRegion &)> &);
    std::vector<Asset> semanticSearch(const std::vector<float> &, const juce::String &model,
                                      const juce::String &preprocessing, const juce::String &root = {},
                                      int limit = 100, RetrievalMetrics *metrics = nullptr,
                                      const juce::String &excludeAsset = {});
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
    int analysisRootCursor = 0;
};
} // namespace sp
