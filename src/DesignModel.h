#pragma once
#include "Catalog.h"
#include "AudioRegion.h"
#include "FadeCurve.h"
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
struct BlendAssignment { juce::String layer, name; };
struct PerformanceState
{
    int blendLaw = 2; // 1: saved v1 bilinear sparse law; 2: continuous convex-hull law.
    bool blendEnabled = false;
    double x = .5, y = .5;
    std::array<BlendAssignment, 4> assignments;
    std::array<PerformanceMacro, 4> macros;
    PerformanceState()
    {
        for (int i = 0; i < 4; ++i) macros[(size_t)i].name = "Macro " + juce::String(i + 1);
    }
};
enum VariationParameter : uint32_t { varyGain=1,varyPan=2,varyPlacement=4,varyRate=8,varyReverse=16,varyFades=32,varyTrim=64,varyEffects=128, lockFadeIn=256,lockFadeOut=512,lockTrimIn=1024,lockTrimOut=2048 };
struct StockEffect
{
    juce::String id=juce::Uuid().toString(),type="gain";
    bool bypass=false,locked=false;
    std::array<double,4> parameters{0,0,0,0};
    uint32_t parameterLocks=0;
};
struct DesignLayer
{
    juce::String id=juce::Uuid().toString(),name;
    Asset source;
    LayerRouting routing;
    bool external=false,mute=false,solo=false,reverse=false,locked=false;
    PlaybackRegion region;
    int64_t placement=0,sync=0,fadeIn=0,fadeOut=0;
    FadeShape fadeInShape=FadeShape::linear,fadeOutShape=FadeShape::linear;
    std::vector<FadePoint> fadeInPoints,fadeOutPoints;
    double gainDb=0,pan=0,rate=1;
    double controlGain=1; // Resolved amplitude factor; ordinary base layers remain at one.
    uint32_t locks=0;
    uint32_t colour=0; // Optional ARGB track colour. Zero uses the application palette.
    std::vector<StockEffect> effects;
};
struct DesignMix
{
    ChannelLayout layout; // Empty retains the legacy stereo contract verbatim.
    int routingVersion = 2; // Version1 remains immutable in historical layout-aware takes.
    PerformanceState performance;
    std::vector<DesignLayer> layers;
    std::vector<StockEffect> masterEffects;
    double masterDb=-6;
    int64_t duration=0; // Zero means the complete audible layer extent.
};
struct DesignVariation
{
    juce::String id=juce::Uuid().toString(),name;
    uint64_t seed=0;
    bool kept=false,snapshot=false;
    DesignMix mix;
};
struct VariationSettings
{
    int count=5;
    uint32_t enabled=varyGain|varyPan|varyPlacement|varyRate;
    double gainDb=3,pan=.25,placementSeconds=.025,ratePercent=10,fadeSeconds=.02,trimSeconds=.02,effectAmount=.1;
};
struct DesignRenderSettings
{
    double sampleRate=48000,tailSeconds=1;
    int channels=2,bits=24;
    int64_t first=0,last=-1;
    bool normalize=false,dither=false;
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
    int spatialCount = 0;
    std::array<SpatialValue, 16> spatial{};
    double x=.5,y=.5;
    std::array<double,4> macros{.5,.5,.5,.5};
    bool blend=false;
    std::array<bool,4> enabled{};
    bool operator==(const PerformControls &) const = default;
};
struct TakeEvent
{
    int64_t frame=0;
    uint64_t sequence=0,generation=0;
    PerformControls controls;
};
struct TakeSource
{
    juce::String id,sha256,overridePath;
    int64_t overrideSize=0,overrideModified=0,overrideInode=0;
};
class TakeEventList
{
    std::shared_ptr<std::vector<TakeEvent>> data=std::make_shared<std::vector<TakeEvent>>();
    void writable(){if(data.use_count()!=1)data=std::make_shared<std::vector<TakeEvent>>(*data);}
  public:
    auto begin()const{return data->cbegin();}auto end()const{return data->cend();}
    size_t size()const{return data->size();}bool empty()const{return data->empty();}
    const TakeEvent &operator[](size_t n)const{return data->at(n);}
    const TakeEvent &back()const{return data->back();}
    void reserve(size_t n){writable();data->reserve(n);}
    void push_back(TakeEvent e){writable();data->push_back(e);}
    void clear(){data=std::make_shared<std::vector<TakeEvent>>();}
    const void *identity()const{return data.get();}
};
class FrozenTakeMix
{
    std::shared_ptr<const DesignMix> value=std::make_shared<const DesignMix>();
  public:
    FrozenTakeMix &operator=(DesignMix mix){value=std::make_shared<const DesignMix>(std::move(mix));return *this;}
    const DesignMix *operator->()const{return value.get();}
    operator const DesignMix &()const{return *value;}
    const void *identity()const{return value.get();}
};
struct PerformTake
{
    juce::String id=juce::Uuid().toString(),name="Untitled take",session,reason;
    juce::String recipeHash,eventHash,processing="stock-takes-v1";
    FrozenTakeMix recipe;
    double sampleRate=48000;
    int64_t bodyEnd=0;
    int64_t outputFrames=0;
    double captureRate=48000;
    uint64_t generation=0;
    bool complete=false,kept=false;
    TakeEventList events;
    std::vector<TakeSource> sources;
    juce::String checkpoint;
    juce::String correctedFrom, originalRecipeHash, originalEventHash;
};
constexpr size_t maxTakeEvents=8192,maxDocumentTakes=16;
constexpr int maxTakeSeconds=120;
struct DesignOutput
{
    juce::String path,sha256,variation,kind="render",created;
    int64_t samples=0,revision=0;
    double peak=0;
    int64_t clipped=0;
    DesignRenderSettings settings;
    juce::String takeId,recipeHash,eventHash,processing;
    double projectRate=0;
    double captureRate=0;
    int64_t bodyEnd=0;
    int64_t capturedFrames=0;
    juce::String tailPolicy;
    juce::var sourceProvenance;
    juce::var channelProvenance, files;
    int outputSetVersion = 0; // Metadata contract; independent of legacy/new DSP.
};
struct DesignDocument
{
    juce::String id=juce::Uuid().toString(),name="Untitled design",activeVariation,selectedLayer,outputFolder,activeTake;
    int64_t revision=1,playhead=0;
    double sampleRate=48000;
    bool takeProcessing=false; // Transient partition contract, never a saved static change.
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
int64_t layerDuration(const DesignLayer &,double designRate);
int64_t mixDuration(const DesignMix &,double designRate);
int64_t designPreviewEnd(const DesignDocument &);
int64_t designPlayStart(const DesignDocument &,int64_t parkedFrame);
int64_t layerSyncFrame(const DesignLayer &,double designRate);
DesignLayer makeDesignLayer(const Asset &,int64_t first,int64_t last,bool external=false);
StockEffect makeStockEffect(const juce::String &type);
FadeShape fadeShapeFromId(const juce::String &);
juce::var fadePointsToJson(const std::vector<FadePoint> &);
std::vector<FadePoint> fadePointsFromJson(const juce::var &);
juce::var designToJson(const DesignDocument &);
DesignDocument designFromJson(const juce::var &);
void validateDesign(const DesignDocument &, bool verifyTakeContent = true);
void saveDesignAtomic(const juce::File &,const DesignDocument &,const std::function<bool()> &cancelled=[] {return false;});
DesignDocument loadDesign(const juce::File &);
std::vector<DesignVariation> generateVariations(const DesignDocument &,uint64_t seed);
Asset inspectDesignSource(const juce::File &);
void resolveDesignSources(DesignDocument &,const juce::File &catalog);
juce::var designMixToJson(const DesignMix &);
DesignMix designMixFromJson(const juce::var &);
PerformControls performControls(const PerformanceState &);
void applyPerformControls(PerformanceState &,const PerformControls &);
juce::var controlsToJson(const PerformControls &);
PerformControls controlsFromJson(const juce::var &);
juce::var takeToJson(const PerformTake &);
PerformTake takeFromJson(const juce::var &);
void validateTake(const PerformTake &);
void hashTake(PerformTake &);
PerformTake freezeTake(const DesignDocument &,const juce::String &name);
const PerformTake *selectedTake(const DesignDocument &);
DesignDocument takeDocument(const DesignDocument &);
DesignMix takeDisplayMix(const PerformTake &);
void verifyTakeSources(const PerformTake &);
bool recoverTakeDrafts(DesignDocument &);
}
