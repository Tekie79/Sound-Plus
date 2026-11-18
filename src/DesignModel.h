#pragma once
#include "Catalog.h"
#include "AudioRegion.h"
#include <array>
#include <atomic>
namespace sp
{
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
    bool external=false,mute=false,solo=false,reverse=false,locked=false;
    PlaybackRegion region;
    int64_t placement=0,sync=0,fadeIn=0,fadeOut=0;
    double gainDb=0,pan=0,rate=1;
    uint32_t locks=0;
    std::vector<StockEffect> effects;
};
struct DesignMix
{
    std::vector<DesignLayer> layers;
    std::vector<StockEffect> masterEffects;
    double masterDb=-6;
    int64_t duration=0; // Zero means the complete audible layer extent.
};
struct DesignVariation
{
    juce::String id=juce::Uuid().toString(),name;
    uint64_t seed=0;
    bool kept=false;
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
};
struct DesignOutput
{
    juce::String path,sha256,variation,kind="render",created;
    int64_t samples=0,revision=0;
    double peak=0;
    int64_t clipped=0;
    DesignRenderSettings settings;
};
struct DesignDocument
{
    juce::String id=juce::Uuid().toString(),name="Untitled design",activeVariation,selectedLayer,outputFolder;
    int64_t revision=1,playhead=0;
    double sampleRate=48000;
    DesignMix original;
    VariationSettings variationSettings;
    DesignRenderSettings renderSettings;
    std::vector<DesignVariation> variations;
    std::vector<DesignOutput> outputs;
    DesignMix &activeMix();
    const DesignMix &activeMix() const;
};
int64_t layerDuration(const DesignLayer &,double designRate);
int64_t mixDuration(const DesignMix &,double designRate);
int64_t designPreviewEnd(const DesignDocument &);
int64_t designPlayStart(const DesignDocument &,int64_t parkedFrame);
int64_t layerSyncFrame(const DesignLayer &,double designRate);
DesignLayer makeDesignLayer(const Asset &,int64_t first,int64_t last,bool external=false);
StockEffect makeStockEffect(const juce::String &type);
juce::var designToJson(const DesignDocument &);
DesignDocument designFromJson(const juce::var &);
void validateDesign(const DesignDocument &);
void saveDesignAtomic(const juce::File &,const DesignDocument &,const std::function<bool()> &cancelled=[] {return false;});
DesignDocument loadDesign(const juce::File &);
std::vector<DesignVariation> generateVariations(const DesignDocument &,uint64_t seed);
Asset inspectDesignSource(const juce::File &);
void resolveDesignSources(DesignDocument &,const juce::File &catalog);
}
