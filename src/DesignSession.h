#pragma once
#include "DesignAudio.h"
#include "AudioIPC.h"
#include "DesignRecovery.h"
#include "SerialWorker.h"
#include <set>

namespace sp
{
// Injected only at the service boundary: deterministic tests control work/completions,
// time, storage failure and transport status without creating a device process.
struct DesignSessionServices
{
    std::function<bool(std::function<void()>)> submit;
    std::function<void(std::function<void()>)> post;
    std::function<double()> now;
    std::function<void(const juce::File &, const DesignDocument &)> write;
    std::function<void(const DesignDocument &, const juce::File &, bool)> retain;
    std::function<void(DesignDocument &)> resolve;
    std::function<juce::var()> status;
    std::function<uint64_t(const juce::String &, const juce::File &, int64_t, const juce::String &)> play;
    std::function<void(const DesignDocument &, uint64_t, double)> parameters;
    std::function<void()> stop;
    bool automaticRecovery = true, timers = true;
};

class DesignSession final : public juce::Component, private juce::Timer
{
  public:
    struct DialogToken
    {
        uint64_t lifetime = 0, operation = 0;
    };
    DesignSession(juce::File catalog, AudioClient &);
    DesignSession(juce::File catalog, DesignSessionServices);
    ~DesignSession() override;
    DesignDocument document;
    juce::File savedFile, lastOutput;
    juce::String message;
    std::vector<DesignRecovery> recoveries;
    std::set<juce::String> selectedIds;
    std::function<void()> onChange, onBeforePlay;
    bool dirty = false, working = false;
    std::atomic<bool> rendering{false};
    std::atomic<double> renderProgress{0};
    juce::File outputFolder() const;
    DialogToken beginDialog();
    bool accepts(DialogToken) const;
    void protectStartupEdits();
    bool transitionPending() const
    {
        return replacing;
    }
    void edit(const juce::String &, const std::function<void(DesignDocument &)> &);
    void beginGesture();
    void endGesture();
    void cancelGesture();
    void undo();
    void redo();
    void newDesign();
    void open(const juce::File &);
    void save(const juce::File &);
    void closeWhenSafe(std::function<void()>);
    void add(const Asset &, int64_t first, int64_t last, bool external = false, int pair = 0,
             int64_t placement = -1);
    void addExternal(const juce::File &, int64_t placement = -1);
    void addExternals(const juce::StringArray &, int64_t placement);
    void replaceSource(const juce::String &layer, const juce::File &);
    void play();
    void playAccurate();
    void seek(int64_t);
    void stop();
    bool ownsPlayback() const;
    void beginSeekGesture(bool audible);
    void dragSeek(int64_t);
    void endSeekGesture();
    void render(bool selection = false);
    void copyOriginal(const juce::File &);
    void cancelRender();
    void selectLayer(const juce::String &, bool additive = false);
    void selectVariation(const juce::String &);
    void generate(uint64_t);
    void recoverAutosave(const juce::File & = {});
    void poll();
    bool canUndo() const
    {
        return !undoHistory.empty();
    }
    bool canRedo() const
    {
        return !redoHistory.empty();
    }
    DesignLayer *selectedLayer();

  private:
    enum class Operation
    {
        save,
        transition,
        import,
        relink,
        recoveryList,
        preview,
        output,
        autosave,
        count
    };
    struct Stamp
    {
        uint64_t lifetime, operation;
        Operation kind;
    };
    struct History
    {
        DesignDocument document;
        std::set<juce::String> selection;
        juce::String label;
    };
    std::vector<History> undoHistory, redoHistory;
    std::optional<History> gesture;
    juce::File catalog, directory;
    DesignSessionServices services;
    std::unique_ptr<SerialWorker> worker;
    std::shared_ptr<DesignRecoveryStore> recovery;
    std::thread renderWorker;
    std::shared_ptr<std::atomic<uint64_t>> previewSerial = std::make_shared<std::atomic<uint64_t>>(0);
    std::atomic<bool> quitting{false}, renderCancelled{false};
    uint64_t lifetime = 1, dialogSerial = 0;
    std::shared_ptr<std::atomic<uint64_t>> lifetimeGate = std::make_shared<std::atomic<uint64_t>>(1);
    std::array<uint64_t, (size_t)Operation::count> operations{};
    std::set<uint64_t> acceptedImports;
    uint64_t startupActivity = 0;
    bool replacing = false, active = false, previewWaiting = false, autoPending = false;
    bool seekGesture = false, seekWasPlaying = false, audibleSeek = false;
    uint64_t expectedGeneration = 0;
    double lastEdit = 0, previewDue = 0, lastAudioUpdate = -1000, autosaveRetry = 0;
    int64_t autoRevision = 0;
    juce::String autoDocument, publishedStructure;
    Stamp stamp(Operation);
    bool accepts(Stamp) const;
    void finishOperation(Stamp);
    void enqueue(Stamp, std::function<std::function<void()>()>);
    void notify();
    void changed();
    void restoreDerivedState();
    void reconcileSelection(const DesignDocument *before = nullptr);
    void transition(const juce::File &, bool recover, std::function<void()> close = {});
    void refreshRecoveries();
    void schedulePreview(const juce::String &mode = "play");
    void timerCallback() override;
    void startOutput(DesignDocument, bool, const juce::File &, juce::String kind = "design-render");
};
} // namespace sp
