#pragma once
#include "DesignSession.h"
namespace sp
{
void reviewCollection(DesignSession &, const juce::File &, bool includeSelectedOutput,
                      juce::Component &anchor);
}
