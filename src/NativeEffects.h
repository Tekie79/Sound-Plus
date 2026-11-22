#pragma once
#include "DesignModel.h"
namespace sp
{
// Version 0 is the untouched legacy four-slot representation. Version 1 is
// keyed by these explicit IDs; definition order is only a prepared-storage adapter.
struct NativeParameter
{
    juce::String id, name, group, units;
    double minimum = 0, maximum = 1, initial = 0, smoothingMs = 10;
    bool logarithmic = false, discrete = false, structural = false;
    juce::StringArray choices;
};
struct NativeEffectDefinition
{
    juce::String type, name, policy = "native-effects-v1";
    int version = 1;
    std::vector<NativeParameter> parameters;
    juce::String latencyPolicy = "zero", tailPolicy = "bounded-output-tail";
};
const std::vector<NativeEffectDefinition> &nativeEffectDefinitions();
const NativeEffectDefinition *nativeDefinition(const juce::String &);
const NativeEffectDefinition &nativeDefinition(const StockEffect &);
void initialiseNativeEffect(StockEffect &);
void validateNativeEffect(const StockEffect &);
juce::var nativeEffectJson(const StockEffect &);
void nativeEffectFromJson(StockEffect &, const juce::var &);
bool hasNativeEffects(const DesignMix &);
bool hasNativeEffects(const DesignDocument &);
int effectParameterCount(const StockEffect &);
juce::String effectParameterId(const StockEffect &, int);
double effectParameterValue(const StockEffect &, int);
void setEffectParameterValue(StockEffect &, int, double);
bool effectParameterLocked(const StockEffect &, int);
void toggleEffectParameterLock(StockEffect &, int);
juce::String effectName(const StockEffect &);
// Presets contain settings/locks, never owner IDs, media or generated audio.
juce::var nativeFactoryPresets(const juce::String &type);
juce::var nativePresetJson(const StockEffect &, const juce::String &name);
void applyNativePreset(StockEffect &, const juce::var &);
} // namespace sp
