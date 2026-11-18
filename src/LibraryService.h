#pragma once
#include "Catalog.h"
#include <functional>
#include <memory>
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
};
class LibraryService
{
  public:
    explicit LibraryService(juce::File database);
    ~LibraryService();
    void addFolder(const juce::File &);
    void rescan();
    void pause(bool);
    uint64_t search(const juce::String &, const juce::String &root = {});
    void page(int offset, int limit = 256);
    void setChangeCallback(std::function<void()>);
    LibrarySnapshot snapshot();
    bool ready() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sp
