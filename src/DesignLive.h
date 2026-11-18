#pragma once
#include "DesignModel.h"
namespace sp
{
juce::String designStructure(const DesignDocument &);
juce::var designParameters(const DesignDocument &);
void applyDesignParameters(DesignDocument &, const juce::var &);
} // namespace sp
