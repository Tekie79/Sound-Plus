#pragma once
#include "Catalog.h"
#include "AIProcess.h"
#include <functional>
namespace sp
{
inline constexpr auto analysisChannelPolicy="all-channel-arithmetic-mean-v1";
struct AudioReference
{
    Asset source;
    int64_t startSample=0,endSample=-1;
    bool external=false,includeSource=false;
    double minimumSimilarity = .55; // Cosine cutoff, not a probability/confidence percentage.
    juce::String channelPolicy=analysisChannelPolicy;
};
struct ReferenceState
{
    AudioReference reference;
    bool present=false,loading=false,cacheHit=false;
    int completedWindows=0,totalWindows=0,silentWindows=0;
    uint64_t generation=0;
    juce::String status,error;
    double encodingMs=0,retrievalMs=0;
};
struct ReferenceEmbedding
{
    AudioReference reference;
    std::vector<float> vector;
    int windows=0,silentWindows=0;
    bool cacheHit=false;
    double encodingMs=0;
};
std::vector<std::pair<int64_t,int64_t>> referenceWindows(int64_t first,int64_t last,double rate);
// Runs only on the single coalescing semantic worker, never the message/audio thread.
class ReferenceEncoder
{
  public:
    explicit ReferenceEncoder(juce::File directory):cache(std::move(directory)) {}
    ReferenceEmbedding encode(AudioReference,AIProcess &,const std::function<bool()> &cancelled,
        const std::function<void(const AudioReference &,int,int)> &progress);
  private:
    juce::File cache;
};
}
