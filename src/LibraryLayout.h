#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp
{
struct LibraryLayout
{
    juce::Rectangle<int> results, selection, waveform, referenceWave;
    int referenceTop = 176, referenceHeight = 0, controlsY = 0, monitorY = 0, statusY = 0;
    bool small = false;
};
inline LibraryLayout libraryLayout(int width, int height, bool compact, bool reference)
{
    LibraryLayout result;
    result.small = height < 780;
    if (compact)
    {
        result.results = {8, 136, std::max(1, width - 16), std::max(60, height - 338)};
        result.waveform = {12, height - 180, std::max(1, width - 24), 94};
        return result;
    }
    int audition = result.small ? 238 : 300;
    result.selection = {264, height - audition, std::max(1, width - 292), 26};
    result.referenceHeight = reference ? (result.small ? 112 : 180) : 0;
    int top = reference ? result.referenceTop + result.referenceHeight + 8 : 181;
    result.results = {253, top, std::max(1, width - 277), std::max(44, result.selection.getY() - 12 - top)};
    result.referenceWave = {264, result.referenceTop + (result.small ? 52 : 108), std::max(1, width - 292),
                            result.small ? 60 : 72};
    result.waveform = {264, result.selection.getY() + 46, std::max(1, width - 292), audition - 160};
    result.controlsY = height - 105;
    result.monitorY = height - 58;
    result.statusY = height - 28;
    return result;
}
} // namespace sp
