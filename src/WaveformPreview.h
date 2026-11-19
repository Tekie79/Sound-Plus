#pragma once
#include "Waveform.h"
#include <map>
namespace sp
{
using PeakColumns = std::vector<std::pair<float, float>>;
// All-channel extrema. Edge bins intersecting the requested half-open range are
// included conservatively: cache resolution, rather than sample-exact trimming.
PeakColumns compactWaveformColumns(const WaveformData &, int64_t first, int64_t last, bool reverse,
                                   int width);
void paintCompactWaveform(juce::Graphics &, juce::Rectangle<float>, const PeakColumns &, juce::Colour,
                          std::optional<double> playhead = {});
juce::String waveformSourceKey(const Asset &);

// One serial decoder/cache worker shared by the detailed audition, Designer and
// visible file rows. Demands are replaced per surface, not appended per paint.
// Lower owner number is higher priority. UI-thread API; immutable worker results.
class WaveformPool
{
  public:
    explicit WaveformPool(const juce::File &, std::function<void()> beforeRead = {},
                          std::function<double()> now = {}, WaveformCache::Checkpoint = {});
    void demand(int owner, const std::vector<Asset> &);
    bool tick();
    std::shared_ptr<const WaveformData> get(const Asset &);
    std::shared_ptr<const PeakColumns> columns(const Asset &, int64_t first, int64_t last, bool reverse,
                                               int width);
    void retry(const Asset &);
    juce::var metrics() const;
    uint64_t revision() const
    {
        return changedGeneration;
    }
    static constexpr size_t maxSources = 48, maxGeometries = 96, maxDemand = 40;
    static constexpr size_t maxDisplaySources = 160, maxSummaryPairs = 2048;

  private:
    struct Entry
    {
        std::shared_ptr<const WaveformData> data;
        uint64_t used = 0;
        double validated = 0;
    };
    struct Geometry
    {
        std::shared_ptr<const PeakColumns> data;
        uint64_t used = 0;
    };
    void adoptDisplay(const juce::String &, Entry);
    std::shared_ptr<std::atomic<uint64_t>> decodes;
    std::function<double()> now;
    WaveformCache worker;
    std::map<int, std::vector<Asset>> owners;
    std::map<juce::String, Entry> completed;
    // Bounded whole-source display summaries for demanded, evicted peak sources.
    // Four surfaces *40 demands *2048 peak pairs maximum, separate from full peaks.
    std::map<juce::String, Entry> displays;
    std::map<juce::String, Geometry> geometries;
    std::map<juce::String, uint64_t> serviced;
    juce::String pending;
    int pendingOwner = 0, foregroundStreak = 0;
    bool protectedBackground = false;
    bool pendingValidation = false;
    uint64_t validationAdmissions = 0, sinceValidation = 0;
    double startedMs = 0, submittedMs = 0, firstReadyMs = -1;
    std::vector<double> loadMs;
    uint64_t token = 0, clock = 0, requests = 0, diskHits = 0, memoryHits = 0, changedGeneration = 0;
    uint64_t cancellations = 0, preemptions = 0, fairAdmissions = 0, admissions = 0;
};
} // namespace sp
