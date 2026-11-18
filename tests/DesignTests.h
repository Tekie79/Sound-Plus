#pragma once
#include <juce_core/juce_core.h>
void runDesignTests(const juce::File &,const juce::File &);
void prepareDesignWorkload(const juce::File &);
void interruptDesignOperation(bool render,const juce::File &target,const juce::File &snapshot,const juce::File &checkpoint);
