#pragma once
#include "Catalog.h"
#include <functional>
#include <future>
#include <memory>
#include "ModelPack.h"
#include "Reference.h"
namespace sp
{
struct IndexProgress
{
    bool pauseRequested = false, paused = false;
    int activeWorkers = 0, parkedWorkers = 0, queuedBatches = 0, queuedDirectories = 0, metadataBacklog = 0,
        completedJobs = 0;
    juce::String root, stage = "Opening";
};
struct LibrarySnapshot
{
    std::vector<Root> roots;
    CollectionState organization;
    juce::String bookmarkInspection;
    bool bookmarkFavorite = false;
    std::vector<Asset> results;
    juce::String error;
    uint64_t request = 0, version = 0, pageSerial = 0, metadataVersion = 0;
    int64_t headersRead = 0;
    int total = 0, offset = 0;
    bool newResultsAvailable = false;
    juce::String selectedAssetId;
    bool selectedAssetRemoved = false;
    IndexProgress progress;
    struct Semantic
    {
        bool enabled = false, ready = false, installing = false, active = false;
        juce::String status, error, model, preprocessing, asset;
        AnalysisCounts coverage;
        int64_t downloadedBytes = 0, downloadTotalBytes = 0, windowsThisSession = 0, helperPeakBytes = 0;
        double modelLoadMs = 0, analysisWallMs = 0, sourceSecondsThisSession = 0, firstWindowStartedMs = 0,
               lastWindowPublishedMs = 0;
        juce::String indexStatus;
        bool indexReady = false, indexReused = false, indexBuilding = false;
        int64_t indexRegions = 0, indexBytes = 0, indexGeneration = -1;
        double indexOpenMs = 0, indexBuildMs = 0;
    } semantic;
    struct SemanticTiming
    {
        double textPreprocessMs = 0, textEncodeMs = 0, workerRoundTripMs = 0, fetchFilterMs = 0,
               vectorScoreMs = 0, rankGroupMs = 0, fusionMs = 0;
        int vectors = 0;
        bool textCacheHit = false;
    } semanticTiming;
    uint64_t semanticCompletedRequest = 0, semanticSerial = 0;
    ReferenceState reference;
    bool exampleMode = false;
};
class LibraryService
{
  public:
    explicit LibraryService(juce::File database, juce::File applicationExecutable = {},
                            juce::File modelDirectory = {}, bool controlledSemantic = false);
    ~LibraryService();
    void addFolder(const juce::File &);
    std::future<Asset> indexGenerated(Asset verified, juce::String collection = {}, juce::var receipt = {});
    void rescan();
    void setLayoutOverride(const Asset &, const ChannelLayout &);
    void pause(bool);
    bool editCollections(CollectionEdit);
    void inspectBookmark(BookmarkRequest, const juce::String &key);
    uint64_t search(const juce::String &, const juce::String &root = {}, const juce::String &scope = {});
    void page(int offset, int limit = 256);
    bool pinSelection(uint64_t request, uint64_t pageSerial, const juce::String &assetId);
    bool pageForRequest(uint64_t request, int offset, int limit = 256);
    void setChangeCallback(std::function<void()>);
    LibrarySnapshot snapshot();
    juce::var queryDiagnostics(); // Explicit benchmark diagnostics, not a user query path.
    bool ready() const;
    juce::File catalogFile() const;
    void installModel(const juce::File &offlinePack = {});
    void cancelModelInstall();
    void retryAnalysis();
    void semanticNow();
    void prioritizeAnalysis(const juce::String &assetId);
    void setAuditionActive(bool);
    uint64_t findSimilar(const AudioReference &, const juce::String &root = {},
                         const juce::String &scope = {});
    void clearReference();

  private:
    friend struct LibraryReliabilityTestAccess;
    friend struct CollectionViewTestAccess;
    friend struct PlaybackSearchTestAccess;
    void publishControlledSemantic(uint64_t, std::vector<Asset>);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sp
