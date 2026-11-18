#pragma once
#include "Catalog.h"
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
namespace sp
{
struct IndexStats
{
    bool ready=false,building=false,reused=false,paused=false;
    int64_t regions=0,bytes=0,sequence=-1;
    double openMs=0,buildMs=0;
    juce::String status;
};
// Rebuildable, checksummed, memory-mapped exact index. SQLite remains authoritative.
// One owned builder, one semantic search caller; a bounded change log supplies the live delta.
class RetrievalIndex
{
  public:
    explicit RetrievalIndex(juce::File database,std::function<void(const IndexStats &)> changed = {},
                            std::function<bool()> pauseRequested = {});
    ~RetrievalIndex();
    std::vector<Asset> search(Catalog &,const std::vector<float> &,const juce::String &root,
        const juce::String &excluded,int limit,RetrievalMetrics *,const std::function<bool()> &cancelled);
    IndexStats stats();
  private:
    struct PackedRegion { int64_t first=0,last=0;std::array<float,512> vector; };
    struct Block {juce::String asset,root;int64_t revision=0,first=0,count=0;};
    struct Snapshot
    {
        juce::File file;
        IndexState state;
        std::vector<Block> blocks;
        std::unique_ptr<juce::MemoryMappedFile> mapped;
        int64_t count=0;
    };
    void run();
    std::shared_ptr<Snapshot> load(Catalog &);
    std::shared_ptr<Snapshot> build(Catalog &);
    void publish(std::shared_ptr<Snapshot>,bool reused,double elapsed);
    juce::File database,directory;
    std::function<void(const IndexStats &)> changed;
    std::function<bool()> pauseRequested;
    std::atomic<bool> stopping{false};
    std::thread builder;
    std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<Snapshot> active;
    std::weak_ptr<Snapshot> deltaBase;
    IndexStats state;
    int64_t deltaSequence=0;
    std::map<juce::String,std::vector<StoredRegion>> delta;
};
}
