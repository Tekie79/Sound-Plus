#pragma once
#include "Catalog.h"
namespace sp
{
void addWaveTags(const juce::File &, juce::StringPairArray &);
juce::var embeddedFields(const juce::StringPairArray &);
juce::var namedFields(const juce::String &filename);
juce::String metadataTokens(const juce::String &, bool filters);
juce::String suggestedCategory(const juce::String &);
bool isMetadataQuery(const juce::String &);
juce::StringArray editableMetadataKeys();
} // namespace sp
