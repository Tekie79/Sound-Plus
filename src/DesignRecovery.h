#pragma once
#include "DesignModel.h"
#include <mutex>

namespace sp
{
struct DesignRecovery
{
    DesignDocument document;
    juce::File snapshot, savedFile;
    bool dirty = true, archived = false;
    int64_t updated = 0;
};

// Recovery metadata is an application journal, separate from the v1 design format.
class DesignRecoveryStore
{
  public:
    explicit DesignRecoveryStore(juce::File directory);
    void retain(const DesignDocument &, const juce::File &savedFile, bool dirty);
    std::vector<DesignRecovery> entries();
    DesignRecovery read(const juce::File &);
    DesignRecovery prepareRecall(DesignRecovery);
    void activate(const juce::String &id);

  private:
    juce::File directory;
    std::mutex mutex;
    int64_t lastUpdate = 0;
    DesignRecovery readUnlocked(const juce::File &);
};
} // namespace sp
