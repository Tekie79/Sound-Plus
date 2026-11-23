#pragma once
#include "DesignModel.h"
namespace sp
{
juce::String designPlaybackContext(const DesignDocument &);
juce::String designStructure(const DesignDocument &);
// Mapping topology excludes only the two qualified continuous panner controls.
juce::var liveRoutingStructure(const LayerRouting &);
juce::var designParameters(const DesignDocument &, bool resolve = true);
void applyDesignParameters(DesignDocument &, const juce::var &);
} // namespace sp
