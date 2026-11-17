#pragma once
#include "Catalog.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
namespace sp
{
struct LibrarySnapshot
{
    std::vector<Root> roots;
    std::vector<Asset> results;
    juce::String error;
    uint64_t request = 0, version = 0;
    int64_t headersRead = 0;
};
class LibraryService
{
  public:
    explicit LibraryService(juce::File database);
    ~LibraryService();
    void addFolder(const juce::File &);
    void rescan();
    uint64_t search(const juce::String &, const juce::String &root = {});
    LibrarySnapshot snapshot();
    bool ready() const
    {
        return initialized.load();
    }

  private:
    void indexLoop();
    void searchLoop();
    void scan(Catalog &, Root);
    void publish();
    juce::File database;
    std::atomic<bool> stopping{false}, initialized{false}, dirty{true}, retryRequested{false};
    std::atomic<uint64_t> version{1};
    std::atomic<int64_t> headers{0};
    std::thread indexer, reader;
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<juce::File> additions;
    juce::String text, rootFilter;
    uint64_t request = 1;
    LibrarySnapshot current;
};
} // namespace sp
