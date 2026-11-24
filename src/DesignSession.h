#pragma once
#include "DesignAudio.h"
#include "AutomationMidi.h"
#include "AudioIPC.h"
#include "DesignRecovery.h"
#include "SerialWorker.h"
#include "SourcePools.h"
#include "LiveInsertSession.h"
#include "InputRecording.h"
#include <set>
#include <deque>

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
    std::function<void(bool)> finishTake;
    std::function<void(bool)> reserveTake;
    std::function<void(juce::var)> inputCommand, squareTouchCommand;
    std::function<void(std::function<void(bool)>)> inputPermission;
    // Optional controlled capture service, same completion boundary as the native service.
    std::function<void(std::function<void(bool)>)> captureLive;
    std::function<bool(const juce::String &, const juce::String &, const juce::String &, int, float)>
        hostedGesture;
    std::function<void(const juce::String &, const juce::String &, uint64_t)> hostedTouchAck;
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
    uint64_t documentGeneration() const
    {
        return lifetime;
    }
    void protectStartupEdits();
    bool transitionPending() const
    {
        return replacing;
    }
    enum class ChangeImpact
    {
        audio,
        metadata
    };
    void edit(const juce::String &, const std::function<void(DesignDocument &)> &,
              ChangeImpact = ChangeImpact::audio);
    juce::File readyOutput();
    juce::StringArray readyFiles();
    void chooseOutput(const juce::String &path);
    void editMetadata(const juce::String &label, const std::function<void(DesignDocument &)> &change)
    {
        edit(label, change, ChangeImpact::metadata);
    }
    void beginGesture();
    void endGesture();
    void cancelGesture();
    void undo();
    void redo();
    void newDesign();
    void open(const juce::File &);
    void save(const juce::File &);
    bool openLiveEditor(const juce::String &slot);
    juce::String liveInsertProblem(const juce::String &slot)
    {
        return liveInserts ? liveInserts->problem(slot) : juce::String();
    }
    bool hostedTouch(const juce::String &slot, const juce::String &parameter, uint64_t incarnation, int kind,
                     float value = 0);
    void withLiveState(std::function<void()>, std::function<void()> rejected = {});
    bool hasLiveInserts() const
    {
        return liveInserts != nullptr;
    }
    void closeWhenSafe(std::function<void()>);
    bool inputBusy() const
    {
        return inputOperation.isNotEmpty();
    }
    bool inputArmed(const juce::String &id) const
    {
        return armedInputs.contains(id);
    }
    bool inputMonitored(const juce::String &id) const
    {
        return monitoredInputs.contains(id);
    }
    bool inputReady() const;
    float inputPeak(const juce::String &) const;
    juce::var inputStatus() const
    {
        return services.status()["inputRecording"];
    }
    void configureTrackInput(const juce::String &, TrackInput);
    void armInputTrack(const juce::String &, bool);
    void monitorInputTrack(const juce::String &, bool);
    void startInputRecording();
    bool hasArmedMidi() const
    {
        return !armedMidi.empty();
    }
    bool midiBusy() const
    {
        return midiOperation.isNotEmpty();
    }
    bool midiArmed(const juce::String &id) const
    {
        return armedMidi.contains(id);
    }
    bool midiMonitored(const juce::String &id) const
    {
        return monitoredMidi.contains(id);
    }
    juce::var midiStatus() const
    {
        return services.status()["midi"];
    }
    void configureMidiInput(const juce::String &, const juce::String &uid, int channel);
    void armMidiTrack(const juce::String &, bool);
    void monitorMidiTrack(const juce::String &, bool);
    void previewMidiNote(const juce::String &, int pitch, bool on, int velocity = 100);
    void startMidiRecording();
    void stopMidiRecording(bool cancel = false);
    void recoverMidiRecording(const juce::File &);
    juce::File midiRecordingRoot() const
    {
        return directory.getChildFile("recorded-midi");
    }
    void startTrackRecording()
    {
        if (!armedMidi.empty())
            startMidiRecording();
        else
            startInputRecording();
    }
    void stopInputRecording(bool cancel = false);
    void recoverInputRecording(const juce::File &, bool allowUnverifiedLegacy = false);
    void findInputRecoveries(std::function<void(juce::Array<juce::var>)>);
    void add(const Asset &, int64_t first, int64_t last, bool external = false, int pair = 0,
             int64_t placement = -1, bool tracksForNewDesign = false, int blendSlot = -1);
    void chooseImpulse(const juce::String &owner, const juce::String &effect, const juce::File &,
                       const juce::String &mapping, bool relink = false);
    void addExternal(const juce::File &, int64_t placement = -1);
    void addExternals(const juce::StringArray &, int64_t placement);
    void replaceSource(const juce::String &layer, const juce::File &);
    void addPoolSource(const juce::String &layer, const Asset &, int64_t first, int64_t last,
                       bool external = false);
    void addPoolFiles(const juce::String &layer, const juce::StringArray &);
    void selectPoolEntry(const juce::String &layer, const juce::String &entry);
    void reviewAndSelectPoolEntry(const juce::String &layer, const juce::String &entry, const LayerRouting &,
                                  int pair);
    void removePoolEntry(const juce::String &layer, const juce::String &entry,
                         const juce::String &replacement = {});
    void reorderPoolEntry(const juce::String &layer, const juce::String &entry, int delta);
    void setPoolEntryEnabled(const juce::String &layer, const juce::String &entry, bool);
    void lockPoolChoice(const juce::String &layer, bool);
    void relinkPoolEntry(const juce::String &layer, const juce::String &entry, const juce::File &);
    void play();
    void playAccurate();
    void seek(int64_t);
    void addMarker(int64_t frame = -1);
    void navigateMarker(int direction);
    void stop();
    bool ownsPlayback() const;
    std::optional<int64_t> automationFrame() const;
    enum class WriteMode
    {
        touch,
        latch,
        replace,
        merge,
        preview
    };
    struct LaneWriteSettings
    {
        WriteMode mode = WriteMode::touch;
        bool armed = false;
        uint64_t started = 0;
    };
    LaneWriteSettings &laneWriteSettings(const juce::String &owner);
    void learnAutomationMidi(const juce::String &, const AutomationTarget &, AutomationMidiBinding);
    void cancelMidiLearn();
    bool learningMidi() const
    {
        return learning.has_value();
    }
    bool beginAutomationWrite(const juce::String &, const AutomationTarget &, double value, bool merge, bool preview);
    bool ownsAutomationWrite(const juce::String &, const AutomationTarget &) const;
    void moveAutomationWrite(const juce::String &, const AutomationTarget &, double);
    void endAutomationWrite(bool punch = false);
    uint64_t automationPlaybackGeneration() const { return expectedGeneration; }
    bool beginSquareTouch(const juce::String &owner, juce::Point<double>);
    bool moveSquareTouch(juce::Point<double>);
    void endSquareTouch();
    bool squareTouchBusy() const
    {
        return squarePass.has_value();
    }
    enum class TakeState
    {
        idle,
        arming,
        recording,
        finalizing,
        ready,
        incomplete
    };
    TakeState takeState = TakeState::idle;
    juce::String captureId;
    bool takeBusy() const
    {
        return takeState == TakeState::arming || takeState == TakeState::recording ||
               takeState == TakeState::finalizing;
    }
    void recordTake();
    void stopTake(bool cancel = false);
    void selectTake(const juce::String &);
    void nameTake(const juce::String &, bool keep = true);
    void deleteTake();
    void createCorrectedSpatialCopy();
    void relinkTakeSource(const juce::String &source, const juce::File &);
    const DesignMix &performanceMix() const;
    void beginSeekGesture(bool audible);
    void dragSeek(int64_t);
    void endSeekGesture();
    void render(bool selection = false, int pad = -1);
    void renderStems(juce::StringArray owners);
    void captureTransferSnapshot(std::function<void(DesignDocument)> ready,
                                 std::function<void(juce::String)> failed);
    void copyOriginal(const juce::File &);
    void transferInsert(const juce::String &sourceOwner, InsertRef, const juce::String &targetOwner,
                        bool move);
    void cancelRender();
    void selectLayer(const juce::String &, bool additive = false);
    void selectMidiClip(const juce::String &);
    void assignSampler(const juce::String &track, const juce::File &, bool alternate = false,
                       int64_t first = 0, int64_t last = 0, std::optional<Asset> catalogSource = {});
    void selectVariation(const juce::String &);
    void generate(uint64_t);
    void saveSnapshot(const juce::String &name);
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
    friend struct PoolCorrectionViewAccess;
    friend struct CorrectionViewTestAccess;
    friend struct TakeIntegrityTestAccess;
    friend struct LayoutViewTestAccess;
    friend struct LiveCorrectionTestAccess;
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
        take,
        input,
        inputRecoveryList,
        midiSource,
        midi,
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
    juce::String auditionContext;
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
    void invalidatePoolWork();
    uint64_t poolHistoryEpoch = 0;
    std::map<juce::String, std::shared_ptr<std::atomic<uint64_t>>> poolRequests;
    void requestPoolActivation(const juce::String &, const juce::String &, std::optional<LayerRouting>,
                               int pair = -99, const juce::String &remove = {});
    void addPoolEntries(const juce::String &, std::function<std::vector<SourcePoolEntry>()>);
    void generatePoolVariations(uint64_t);
    bool replacing = false, active = false, previewWaiting = false, autoPending = false;
    bool seekGesture = false, seekWasPlaying = false, audibleSeek = false;
    uint64_t expectedGeneration = 0;
    struct LearnTarget
    {
        juce::String owner, document, variation;
        AutomationTarget target;
        AutomationMidiBinding binding;
        uint64_t lifetime = 0;
    };
    struct MidiControlRoute
    {
        LearnTarget destination;
        AutomationParameter parameter;
        ControllerPickup pickup;
    };
    std::map<juce::String, LaneWriteSettings> laneModes;
    juce::String laneModesDocument, laneModesVariation;
    uint64_t laneModesLifetime = 0;
    void pollArmedWrites(const juce::var &);
    std::optional<LearnTarget> learning;
    std::vector<MidiControlRoute> controlRoutes;
    std::map<juce::String, std::unique_ptr<AutomationMidiInput>> controlInputs;
    juce::String controlContext, midiWriterId;
    int64_t controlRevision = -1;
    double nextControlScan = 0;
    uint64_t controlGeneration = 0;
    void pollAutomationMidi(const juce::var &);
    struct WriterPass
    {
        juce::String id, owner, document, variation, base;
        juce::var completedPreview;
        AutomationTarget target;
        uint64_t lifetime = 0, generation = 0;
        int64_t epoch = 0, helper = 0, first = 0, last = 0;
        bool merge = false, preview = false, ending = false, commit = false;
        double deadline = 0;
    };
    std::optional<WriterPass> writerPass;
    std::function<void()> afterWriter;
    void sendAutomationWrite(const char *, double = 0);
    void pollAutomationWrite(const juce::var &);
    struct SquarePass
    {
        juce::String id, owner, document, variation;
        uint64_t lifetime, generation;
        int64_t epoch, helper;
        bool ending = false;
        double deadline = 0;
    };
    std::optional<SquarePass> squarePass;
    std::function<void()> afterSquareTouch;
    void sendSquareTouch(const char *, juce::Point<double> = {});
    void pollSquareTouch(const juce::var &);
    double lastEdit = 0, previewDue = 0, lastAudioUpdate = -1000, autosaveRetry = 0;
    int64_t autoRevision = 0;
    juce::String autoDocument, publishedStructure;
    DesignMix takeDisplay;
    std::optional<PerformControls> takeGestureStart;
    std::optional<History> takeUndoBase;
    bool takeMutation = false, takeReadPending = false;
    std::set<juce::String> armedMidi, monitoredMidi;
    juce::String midiOperation, midiContext;
    juce::var midiPlan;
    uint64_t midiIncarnation = 0, midiProcess = 0;
    bool midiMutation = false, midiReadPending = false, midiStopping = false;
    double midiStarted = 0;
    std::function<void()> midiClose;
    void updateMidiDevice();
    void pollMidi(const juce::var &);
    void applyMidiReceipt(juce::var);
    void clearMidiContext();
    std::set<juce::String> armedInputs, monitoredInputs, pendingInputArms;
    juce::String inputOperation, inputDocument, inputVariation, inputArmContext, inputLastPhase;
    uint64_t inputIncarnation = 0, inputProcessGeneration = 0, inputControlSerial = 0;
    uint64_t inputMonitorControlSerial = 0;
    int overdubPad = -1;
    bool inputMutation = false, inputReadPending = false, inputStopRequested = false,
         inputCancelRequested = false;
    bool inputConfigPending = false;
    std::function<void()> inputClose;
    void pollInput(const juce::var &);
    void updateInputDevice();
    void applyInputReceipt(juce::var);
    void finishInputContext();
    juce::File inputRecordingRoot() const
    {
        return directory.getChildFile("recorded-audio");
    }
    double takeStartedAt = 0;
    juce::File takeCheckpoint;
    bool takeCapturingPlugins = false;
    juce::String takeBoundary;
    void prepareTake(Stamp);
    void rejectArmingTake(const juce::String &, const juce::String &);
    void pollTake(const juce::var &);
    void restoreTakeDrafts();
    void selectContextOutput();
    Stamp stamp(Operation);
    bool accepts(Stamp) const;
    void finishOperation(Stamp);
    void enqueue(Stamp, std::function<std::function<void()>()>);
    std::unique_ptr<LiveInsertSession> liveInserts;
    AudioClient *liveAudio = nullptr;
    double lastLiveSync = -1000;
    bool liveReady = false, liveApplying = false, liveFlushing = false;
    std::map<juce::String, uint64_t> impulseRequests;
    struct LiveCommand
    {
        uint64_t incarnation;
        juce::String document, variation, take;
        std::function<void()> continuation;
        bool allowFailure = false;
        std::function<void()> rejected;
    };
    std::deque<LiveCommand> liveCommands;
    bool liveSuppressHistory = false, liveInvalidateRedo = false;
    void runLiveCommands();
    void finishLiveCommand(bool);
    void renderReady(bool, const juce::String &, int pad = -1);
    std::optional<DesignDocument> liveIntentBase;
    void stampLiveIntents(int64_t);
    uint64_t liveUserEdits = 0;
    juce::String liveHistoryKey;
    int64_t liveHistoryRevision = -1;
    void syncLive();
    friend struct HostedAutomationTestAccess;
    void receiveHostedTouches(const LiveInsertSession::Capture &);
    void acknowledgeRetainedTouch(const juce::String &, const juce::String &, uint64_t);
    struct HostedTouchContext
    {
        juce::String document, variation, generation, slot, owner;
        uint64_t incarnation = 0, id = 0;
        AutomationTarget target;
        HostedAutomationBinding binding;
        std::vector<AutomationPoint> points;
        bool applied = false, retentionPending = false;
    };
    void retainHostedTouch(const juce::String &);
    std::map<juce::String, HostedTouchContext> nativeTouches;
    std::map<juce::String, std::set<uint64_t>> completedTouches;
    std::map<juce::String, uint64_t> retainedTouch;
    struct InlineTouch
    {
        juce::String owner, variation;
        HostedAutomationBinding binding;
    };
    std::map<juce::String, InlineTouch> inlineTouches;
    uint64_t touchLifetime = 0;
    void receiveLive(LiveInsertSession::Capture);
    bool flushLive(std::function<void()>, bool allowFailure = false, std::function<void()> rejected = {});
    std::map<juce::String, LiveInsertSession::Capture> deferredNativeCaptures;
    void drainLiveCaptures();
    void notify();
    void changed(ChangeImpact = ChangeImpact::audio);
    void restoreDerivedState();
    void reconcileSelection(const DesignDocument *before = nullptr);
    void transition(const juce::File &, bool recover, std::function<void()> close = {});
    void refreshRecoveries();
    void schedulePreview(const juce::String &mode = "play");
    void timerCallback() override;
    void startOutput(DesignDocument, bool, const juce::File &, juce::String kind = "design-render",
                     juce::StringArray stems = {});
};
} // namespace sp
