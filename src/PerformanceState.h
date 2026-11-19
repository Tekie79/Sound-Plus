#pragma once
#include "DesignModel.h"
namespace sp
{
struct PerformanceTargetInfo
{
    PerformanceTarget target;
    juce::String name, units;
    double minimum, maximum;
    bool logarithmic = false;
};
bool hasPerformance(const PerformanceState &);
juce::var performanceToJson(const PerformanceState &);
PerformanceState performanceFromJson(const juce::var &);
void validatePerformance(const DesignMix &);
std::array<double, 4> blendWeights(const PerformanceState &, const DesignMix &);
std::vector<PerformanceTargetInfo> performanceTargets(const DesignMix &);
juce::String performanceLayerName(const DesignMix &, const DesignLayer &);
double *performanceTargetValue(DesignMix &, const PerformanceTarget &);
const double *performanceTargetValue(const DesignMix &, const PerformanceTarget &);
bool performanceOwns(const DesignMix &, const PerformanceTarget &);
PerformanceTarget performanceEffectTarget(const juce::String &layer, const StockEffect &, int parameter);
void unlinkPerformanceTarget(DesignMix &, const PerformanceTarget &);
bool performanceEffectOwned(const DesignMix &, const juce::String &layer, const StockEffect &, int parameter);
void assignBlendLayer(DesignMix &, int slot, const juce::String &layer);
void addMacroMapping(DesignMix &, int macro, PerformanceMapping);
void unlinkManuallyEditedTargets(const DesignMix &before, DesignMix &after);
DesignMix resolvePerformance(const DesignMix &);
DesignDocument resolvePerformance(const DesignDocument &);
} // namespace sp
