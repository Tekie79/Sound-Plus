#pragma once
#include "DesignModel.h"
namespace sp
{
struct AutomationParameter;
bool hasMultiPad(const DesignMix &);
bool hasMultiPad(const DesignDocument &);
PerformanceState &padState(DesignMix &, int);
const PerformanceState &padState(const DesignMix &, int);
PerformanceState &editingPerformance(DesignDocument &);
juce::String padIdentity(const DesignMix &, int);
int padLayerOwner(const DesignMix &, const juce::String &);
void addPerformancePad(DesignDocument &);
juce::var padsToJson(const DesignMix &);
void padsFromJson(DesignMix &, const juce::var &);
void appendPadAutomationTargets(const DesignMix &, std::vector<AutomationParameter> &);
struct AutomationWriter;
DesignDocument evaluatePadAutomation(const DesignDocument &, int64_t, AutomationWriter * = nullptr,
                                     int64_t appliedFrame = -1);
void disablePadAutomation(DesignMix &, int);
void retainPadTake(DesignMix &, int, const PerformTake &);
DesignDocument padRenderDocument(const DesignDocument &, int);

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
void assignBlendLayer(DesignMix &, int slot, const juce::String &layer, int pad = 0);
void addMacroMapping(DesignMix &, int macro, PerformanceMapping, int pad = 0);
void unlinkManuallyEditedTargets(const DesignMix &before, DesignMix &after);
DesignMix resolvePerformance(const DesignMix &);
DesignDocument resolvePerformance(const DesignDocument &);
} // namespace sp
