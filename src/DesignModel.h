#pragma once
#include "Catalog.h"
#include "AudioRegion.h"
#include "FadeCurve.h"
#include "HostedRack.h"
#include "HostedAutomationBinding.h"
#include <optional>
#include "MidiModel.h"
#include <array>
#include <atomic>
#include <map>
namespace sp
{
struct PerformanceTarget
{
    juce::String layer, effect, parameter;
    bool operator==(const PerformanceTarget &) const = default;
};
struct PerformanceMapping
{
    PerformanceTarget target;
    double minimum = 0, maximum = 1;
    juce::String curve = "linear";
    bool inverted = false;
};
struct PerformanceMacro
{
    juce::String name;
    double value = .5, reset = .5;
    bool enabled = false;
    std::vector<PerformanceMapping> mappings;
};
struct BlendAssignment
{
    juce::String layer, name;
};
struct PerformanceState
{
    int blendLaw = 2; // 1: saved v1 bilinear sparse law; 2: continuous convex-hull law.
    bool blendEnabled = false, mute = false, solo = false;
    double x = .5, y = .5, levelDb = 0;
    std::array<BlendAssignment, 4> assignments;
    std::array<PerformanceMacro, 4> macros;
    PerformanceState()
    {
        for (int i = 0; i < 4; ++i)
            macros[(size_t)i].name = "Macro " + juce::String(i + 1);
    }
};
struct PerformancePad
{
    juce::String id = juce::Uuid().toString();
    PerformanceState state;
};
struct PadControlValues
{
    double x = .5, y = .5, levelDb = 0;
    std::array<double, 4> macros{.5, .5, .5, .5};
    std::array<bool, 4> enabled{};
    bool blend = false, mute = false, solo = false;
    bool operator==(const PadControlValues &) const = default;
};
enum VariationParameter : uint32_t
{
    varyGain = 1,
    varyPan = 2,
    varyPlacement = 4,
    varyRate = 8,
    varyReverse = 16,
    varyFades = 32,
    varyTrim = 64,
    varyEffects = 128,
    lockFadeIn = 256,
    lockFadeOut = 512,
    lockTrimIn = 1024,
    lockTrimOut = 2048,
    varySource = 4096
};
struct ImpulseRoute
{
    juce::String input, output;
    int channel = 0;
    bool operator==(const ImpulseRoute &) const = default;
};
struct ImpulseReference
{
    Asset source;
    juce::String sha256, mapping;
    std::vector<ImpulseRoute> routes;
};
struct StockEffect
{
    juce::String id = juce::Uuid().toString(), type = "gain";
    bool bypass = false, locked = false;
    std::array<double, 4> parameters{0, 0, 0, 0};
    uint32_t parameterLocks = 0;
    int nativeVersion = 0; // 0 retains legacy encoding/DSP verbatim.
    std::vector<double> nativeValues;
    juce::StringArray nativeLocks;
    std::optional<ImpulseReference> impulse; // Referenced original; new convolution identity only.
};
// A single owner chain: order is authoritative; typed payload tables have no
// signal-order meaning. Stable IDs join each typed slot to exactly one payload.
enum class InsertKind
{
    stock,
    hosted
};
struct InsertRef
{
    InsertKind kind = InsertKind::stock;
    juce::String id;
    bool operator==(const InsertRef &) const = default;
};
struct InsertChain
{
    std::vector<InsertRef> order;
    std::vector<StockEffect> stock;
    HostedRack hosted;
    bool bypass = false;
};
std::vector<InsertRef> resolvedInsertOrder(const InsertChain &);
void normaliseInsertChain(InsertChain &);
void rekeyInsertChain(InsertChain &);
bool requiresOrderedInserts(const InsertChain &);
juce::var insertChainToJson(const InsertChain &);
InsertChain insertChainFromJson(const juce::var &);

struct SourcePoolEntry
{
    juce::String id = juce::Uuid().toString(), fingerprint;
    Asset source;
    PlaybackRegion region;
    LayerRouting routing;
    ChannelLayout reviewedBus;
    bool external = false, enabled = true, reviewed = false;
};
struct SourceTriggerMidi
{
    juce::String device, entry;
    int channel = 1, key = 0;
    bool note = true;
};
struct SourcePool
{
    juce::String id = juce::Uuid().toString(), selected;
    std::vector<SourcePoolEntry> entries;
    bool choiceLocked = false, live = false;
    std::optional<SourceTriggerMidi> triggerMidi;
    juce::String policy = "explicit-v1";
    uint64_t seed = 0, ordinal = 0;
};
constexpr size_t maxPoolEntries = 64;
struct SourceVoiceBinding
{
    juce::String pad, layer, pool;
    int corner = 0;
    SourcePoolEntry entry;
};
enum class SourceVoiceAction
{
    trigger,
    switchSource
};
struct SourceVoiceEvent
{
    juce::String id;
    int binding = 0;
    SourceVoiceAction action = SourceVoiceAction::trigger;
    int64_t frame = 0;
    uint64_t sequence = 0;
    int overlap = 0;
    double levelDb = 0;
};
struct SourcePerformance
{
    std::vector<SourceVoiceBinding> bindings;
    std::vector<SourceVoiceEvent> events;
};
struct SourceTiming
{
    int64_t first = 0, last = 0, placement = 0, fadeIn = 0, fadeOut = 0;
    double rate = 1;
    bool reverse = false;
    FadeShape inShape = FadeShape::linear, outShape = FadeShape::linear;
    std::vector<FadePoint> inPoints, outPoints;
    juce::String source;
    bool operator==(const SourceTiming &) const = default;
};
struct SplitHistory
{
    SourceTiming origin, fragment;
    bool ownsTail = true;
    int64_t frames = 0; // Exact project-frame gate, independent of rounded source-sample trims.
};
struct DesignLayer
{
    juce::String id = juce::Uuid().toString(), name, trackId;
    Asset source;
    LayerRouting routing;
    bool external = false, mute = false, solo = false, reverse = false, locked = false;
    PlaybackRegion region;
    int64_t placement = 0, sync = 0, fadeIn = 0, fadeOut = 0;
    FadeShape fadeInShape = FadeShape::linear, fadeOutShape = FadeShape::linear;
    std::vector<FadePoint> fadeInPoints, fadeOutPoints;
    double gainDb = 0, pan = 0, rate = 1;
    double controlGain = 1; // Resolved amplitude factor; ordinary base layers remain at one.
    uint32_t locks = 0;
    uint32_t colour = 0; // Optional ARGB track colour. Zero uses the application palette.
    InsertChain inserts; // Source-channel chain before spatial/matrix routing.
    std::optional<SourcePool> pool;
    std::optional<SplitHistory> splitHistory;
    juce::var recordingOrigin; // Immutable source-capture receipt, distinct from Perform events.
};
SourceTiming sourceTiming(const DesignLayer &);
bool usesSplitHistory(const DesignLayer &);
DesignLayer splitPlaybackLayer(const DesignLayer &);
enum class TrackKind
{
    audio,
    midiInstrument,
    folder,
    groupBus,
    fxReturn,
    master
};
struct AutomationTarget
{
    juce::String instance, parameter = "volume";
    bool operator==(const AutomationTarget &) const = default;
};
struct AutomationPoint
{
    juce::String id = juce::Uuid().toString();
    int64_t frame = 0;
    double value = 0;
    bool hold = false;                     // Outgoing segment. Discrete targets always hold.
    bool smooth = false;                   // Square XY only.
    double smoothStart = 0, smoothEnd = 1; // Retain the shape when a segment is cut.
};
struct AutomationMidiBinding
{
    juce::String device;
    int channel = 1, cc = 0;
    double low = 0, high = 1;
    bool invert = false, pickup = true;
    int hysteresis = 2;
    bool operator==(const AutomationMidiBinding &) const = default;
};
struct AutomationTrim
{
    int64_t first=0, last=0;
    double amount=0; // Additive dB or octaves for a logarithmic frequency target.
};
struct AutomationCurve
{
    AutomationTarget target;
    bool read = true;
    std::vector<AutomationPoint> points;
    std::optional<HostedAutomationBinding> hostedBinding;
    std::vector<AutomationTrim> trims;
    std::optional<AutomationMidiBinding> midi;
    juce::String followClip;
};
struct AutomationLane
{
    bool expanded = false, touch = false;
    AutomationTarget shown, armed; // Display, write target and stored read curves are independent.
    std::vector<AutomationCurve> curves;
};
struct InstrumentSlot
{
    juce::String id = juce::Uuid().toString(), format, identifier, state;
    int version = 0;
    std::vector<SourcePoolEntry> samples;
    int rootNote = 60, polyphony = 32;
    double attack = .005, decay = .1, sustain = .8, release = .2, bendRange = 2, instrumentGainDb = 0,
           transpose = 0;
    bool sampleLoop = false, oneShot = false, roundRobin = false;
    InsertChain hostedInstrument;
};
struct TrackInput
{
    juce::String deviceUID, deviceName;
    std::vector<int> channels;
    ChannelLayout layout;
};
struct SendRole
{
    juce::String source, destination;
    double gainDb = 0.;
};
struct MixerSend
{
    juce::String id = juce::Uuid().toString(), destination;
    double levelDb = -12.;
    bool enabled = false, postFader = true, followPan = true;
    std::vector<SendRole> roles;
};
struct DesignTrack
{
    juce::String id = juce::Uuid().toString(), name = "Audio", parent, output;
    TrackKind kind = TrackKind::audio;
    ChannelLayout layout;
    bool mute = false, solo = false, collapsed = false, spatial = false;
    uint32_t colour = 0;
    double volumeDb = 0, pan = 0, azimuth = 0, width = 60;
    std::optional<SurroundPosition> surroundXY; // Explicit square-surround-v1 processing.
    InsertChain inserts; // Clip-source compatibility chains stay on clips, before their routing.
    std::optional<InstrumentSlot>
        instrument; // Dedicated processor before the Track inserts; version0 is historical reserved metadata.
    AutomationLane automation;
    juce::String midiInputUID;
    int midiChannel = 0;             // All=0, otherwise 1–16. Arm/monitor remain transient.
    std::optional<TrackInput> input; // Configuration only. Arm and monitoring never reopen automatically.
    std::vector<MixerSend> sends;
};
struct DesignMix
{
    ChannelLayout layout;         // Empty retains the legacy stereo contract verbatim.
    int routingVersion = 2;       // Version1 remains immutable in historical layout-aware takes.
    PerformanceState performance; // Historical Pad A, unchanged until multi-pad is used.
    std::vector<PerformancePad> performancePads; // B–D, value-owned; at most three.
    std::vector<DesignLayer> layers; // Persistent audio clips; old IDs and source-stage semantics retained.
    MidiTiming midiTiming;
    bool midiEnabled = false;
    std::vector<MidiClip> midiClips;
    bool trackSchema = false;
    std::vector<DesignTrack> tracks;
    AutomationLane masterAutomation;
    InsertChain inserts; // Summed bus chain before master gain.
    double masterDb = -6;
    int64_t duration = 0; // Zero means the complete audible layer extent.
};
struct DesignVariation
{
    juce::String id = juce::Uuid().toString(), name;
    uint64_t seed = 0;
    bool kept = false, snapshot = false;
    DesignMix mix;
};
struct VariationSettings
{
    int count = 5;
    uint32_t enabled = varyGain | varyPan | varyPlacement | varyRate;
    double gainDb = 3, pan = .25, placementSeconds = .025, ratePercent = 10, fadeSeconds = .02,
           trimSeconds = .02, effectAmount = .1;
};
struct DesignRenderSettings
{
    double sampleRate = 48000, tailSeconds = 1;
    int channels = 2, bits = 24;
    int64_t first = 0, last = -1;
    bool normalize = false, dither = false;
    juce::String layout; // Empty = legacy channels; "same" = current design layout.
    bool splitMono = false;
};
struct SpatialValue
{
    int layer = -1;
    double azimuth = 0, width = 60;
    bool operator==(const SpatialValue &) const = default;
};
struct PerformControls
{
    int padCount = 0; // 0 retains the historical event encoding.
    double levelDb = 0;
    bool mute = false, solo = false;
    std::array<PadControlValues, 3> pads{};
    int spatialCount = 0;
    std::array<SpatialValue, 16> spatial{};
    double x = .5, y = .5;
    std::array<double, 4> macros{.5, .5, .5, .5};
    bool blend = false;
    std::array<bool, 4> enabled{};
    bool operator==(const PerformControls &) const = default;
};
struct TakeEvent
{
    int64_t frame = 0;
    uint64_t sequence = 0, generation = 0;
    PerformControls controls;
};
struct TakeSource
{
    juce::String id, sha256, overridePath;
    int64_t overrideSize = 0, overrideModified = 0, overrideInode = 0;
};
class TakeEventList
{
    std::shared_ptr<std::vector<TakeEvent>> data = std::make_shared<std::vector<TakeEvent>>();
    void writable()
    {
        if (data.use_count() != 1)
            data = std::make_shared<std::vector<TakeEvent>>(*data);
    }

  public:
    auto begin() const
    {
        return data->cbegin();
    }
    auto end() const
    {
        return data->cend();
    }
    size_t size() const
    {
        return data->size();
    }
    bool empty() const
    {
        return data->empty();
    }
    const TakeEvent &operator[](size_t n) const
    {
        return data->at(n);
    }
    const TakeEvent &back() const
    {
        return data->back();
    }
    void reserve(size_t n)
    {
        writable();
        data->reserve(n);
    }
    void push_back(TakeEvent e)
    {
        writable();
        data->push_back(e);
    }
    void clear()
    {
        data = std::make_shared<std::vector<TakeEvent>>();
    }
    const void *identity() const
    {
        return data.get();
    }
};
class FrozenTakeMix
{
    std::shared_ptr<const DesignMix> value = std::make_shared<const DesignMix>();

  public:
    FrozenTakeMix &operator=(DesignMix mix)
    {
        value = std::make_shared<const DesignMix>(std::move(mix));
        return *this;
    }
    const DesignMix *operator->() const
    {
        return value.get();
    }
    operator const DesignMix &() const
    {
        return *value;
    }
    const void *identity() const
    {
        return value.get();
    }
};
struct PerformTake
{
    juce::String id = juce::Uuid().toString(), name = "Untitled take", session, reason;
    juce::String recipeHash, eventHash, processing = "stock-takes-v1";
    FrozenTakeMix recipe;
    bool trackRecipe = false;   // Recipe v4 freezes independent tracks/clips/project-time automation.
    bool pluginAware = false;   // Recipe v3: fixed hosted state, independent frame-zero processors.
    bool orderedRecipe = false; // Recipe version 2; historical recipe hashes keep their legacy encoding.
    double sampleRate = 48000;
    int64_t bodyEnd = 0;
    int64_t outputFrames = 0;
    double captureRate = 48000;
    uint64_t generation = 0;
    bool complete = false, kept = false;
    TakeEventList events;
    SourcePerformance sourcePerformance;
    std::vector<TakeSource> sources;
    juce::String checkpoint;
    juce::String correctedFrom, originalRecipeHash, originalEventHash;
    // Versioned provenance, outside the unchanged DSP recipe/event hash contract.
    juce::String poolOrigins; // Canonical JSON, immutable value semantics.
};
constexpr size_t maxTakeEvents = 8192, maxDocumentTakes = 16;
constexpr int maxTakeSeconds = 120;
struct DesignOutput
{
    juce::String path, sha256, variation, kind = "render", created;
    int64_t samples = 0, revision = 0;
    double peak = 0;
    int64_t clipped = 0;
    DesignRenderSettings settings;
    juce::String takeId, recipeHash, eventHash, processing;
    double projectRate = 0;
    double captureRate = 0;
    int64_t bodyEnd = 0;
    int64_t capturedFrames = 0;
    juce::String tailPolicy;
    juce::var sourceProvenance, pluginProvenance;
    juce::var channelProvenance, files;
    int outputSetVersion = 0; // Metadata contract; independent of legacy/new DSP.
};
struct DesignMarker
{
    juce::String id = juce::Uuid().toString(), name;
    int64_t frame = 0;
};
struct DesignDocument
{
    juce::String id = juce::Uuid().toString(), name = "Untitled design", activeVariation, selectedLayer,
                 outputFolder, activeTake, selectedTrack;
    juce::String selectedMidiClip;
    int performPad = 0; // Editor tab only, excluded from audio identity.
    int64_t revision = 1, playhead = 0;
    double sampleRate = 48000;
    int64_t liveControlTicks = 0; // Transient UI control-event clock.
    bool liveInserts = false;     // Transient retained-instance routing; never saved.
    bool takeProcessing = false;  // Transient partition contract, never a saved static change.
    std::vector<DesignMarker> markers;
    DesignMix original;
    VariationSettings variationSettings;
    DesignRenderSettings renderSettings;
    std::vector<DesignVariation> variations;
    std::vector<DesignOutput> outputs;
    std::map<juce::String, juce::String> outputSelections;
    std::vector<PerformTake> takes;
    DesignMix &activeMix();
    const DesignMix &activeMix() const;
};
bool hasHostedInserts(const DesignMix &);
bool hasHostedInserts(const DesignDocument &);
juce::StringArray outputFiles(const DesignOutput &);
void setMixLayout(DesignMix &, const ChannelLayout &);
bool upgradeLiveSpatial(DesignDocument &);
PerformTake correctedSpatialCopy(const PerformTake &);
bool simplePanAvailable(const DesignMix &, const DesignLayer &);
void initialiseRouting(DesignLayer &, const ChannelLayout &, bool preserveLegacySelection = false);
void validateLayoutMix(const DesignMix &, double);
int designChannels(const DesignDocument &);
ChannelLayout designLayout(const DesignDocument &);
ChannelLayout exportLayout(const DesignDocument &, const DesignRenderSettings &);
PerformControls mixControls(const DesignMix &);
void applyMixControls(DesignMix &, const PerformControls &);
int64_t layerDuration(const DesignLayer &, double designRate);
int64_t mixDuration(const DesignMix &, double designRate);
int64_t designPreviewEnd(const DesignDocument &);
int64_t designPlayStart(const DesignDocument &, int64_t parkedFrame);
int64_t layerSyncFrame(const DesignLayer &, double designRate);
DesignLayer makeDesignLayer(const Asset &, int64_t first, int64_t last, bool external = false);
StockEffect makeStockEffect(const juce::String &type);
FadeShape fadeShapeFromId(const juce::String &);
juce::var fadePointsToJson(const std::vector<FadePoint> &);
std::vector<FadePoint> fadePointsFromJson(const juce::var &);
juce::var designToJson(const DesignDocument &);
DesignDocument designFromJson(const juce::var &);
void validateDesign(const DesignDocument &, bool verifyTakeContent = true);
void saveDesignAtomic(
    const juce::File &, const DesignDocument &,
    const std::function<bool()> &cancelled = [] { return false; });
DesignDocument loadDesign(const juce::File &);
std::vector<DesignVariation> generateVariations(const DesignDocument &, uint64_t seed);
Asset inspectDesignSource(const juce::File &);
void resolveDesignSources(DesignDocument &, const juce::File &catalog);
juce::var designMixToJson(const DesignMix &, bool legacyInserts = false);
DesignMix designMixFromJson(const juce::var &);
PerformControls performControls(const PerformanceState &);
void applyPerformControls(PerformanceState &, const PerformControls &);
juce::var controlsToJson(const PerformControls &);
PerformControls controlsFromJson(const juce::var &);
juce::var takeToJson(const PerformTake &);
PerformTake takeFromJson(const juce::var &);
void validateTake(const PerformTake &);
juce::var takePluginProvenance(const PerformTake &);
void hashTake(PerformTake &);
PerformTake freezeTake(const DesignDocument &, const juce::String &name);
const PerformTake *selectedTake(const DesignDocument &);
DesignDocument takeDocument(const DesignDocument &);
DesignMix takeDisplayMix(const PerformTake &);
void verifyTakeSources(const PerformTake &);
void verifyTakePlugins(const PerformTake &);
bool recoverTakeDrafts(DesignDocument &);
} // namespace sp
