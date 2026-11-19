#pragma once
#include <juce_core/juce_core.h>
void runLibraryStabilityTests(const juce::File &, const juce::File &);
void runWaveformSchedulingTests(const juce::File &);
void prepareReliabilityWorkload(const juce::File &);
void runAudioHealthTests(const juce::File &, const juce::File &);
