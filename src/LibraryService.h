#pragma once
#include "Catalog.h"
#include <functional>
#include <memory>
#include "ModelPack.h"
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
    std::vector<Asset> results;
    juce::String error;
    uint64_t request = 0, version = 0, pageSerial = 0;
    int64_t headersRead = 0;
    int total = 0, offset = 0;
    bool newResultsAvailable = false;
    IndexProgress progress;
    struct Semantic
    {
        bool enabled = false, ready = false, installing = false, active = false;
        juce::String status, error, model, preprocessing, asset;
        AnalysisCounts coverage;
        int64_t downloadedBytes = 0, downloadTotalBytes = 0, windowsThisSession = 0, helperPeakBytes = 0;
        double modelLoadMs = 0, analysisWallMs = 0, sourceSecondsThisSession = 0,
               firstWindowStartedMs = 0, lastWindowPublishedMs = 0;
    } semantic;
    struct SemanticTiming
    {
        double textPreprocessMs = 0, textEncodeMs = 0, workerRoundTripMs = 0,
               fetchFilterMs = 0, vectorScoreMs = 0, rankGroupMs = 0, fusionMs = 0;
        int vectors = 0;
        bool textCacheHit = false;
    } semanticTiming;
    uint64_t semanticCompletedRequest = 0, semanticSerial = 0;
};
class LibraryService
{
  public:
    explicit LibraryService(juce::File database, juce::File applicationExecutable = {},
                             juce::File modelDirectory = {});
    ~LibraryService();
    void addFolder(const juce::File &);
    void rescan();
    void pause(bool);
    uint64_t search(const juce::String &, const juce::String &root = {});
    void page(int offset, int limit = 256);
    void setChangeCallback(std::function<void()>);
    LibrarySnapshot snapshot();
    bool ready() const;
    void installModel(const juce::File &offlinePack = {});
    void cancelModelInstall();
    void retryAnalysis();
    void semanticNow();
    void prioritizeAnalysis(const juce::String &assetId);
    void setAuditionActive(bool);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sp
