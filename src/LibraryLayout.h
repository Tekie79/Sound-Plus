#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp
{
inline juce::Range<int> visibleLibraryRows(juce::Rectangle<int> area, int height, int total)
{
    if (area.isEmpty() || height <= 0)
        return {};
    int first = std::clamp(area.getY() / height, 0, total);
    int last = std::clamp((area.getBottom() + height - 1) / height, first, total);
    return {first, last};
}
struct LibraryLayout
{
    juce::Rectangle<int> collections, results, details, selection, waveform, auditionMeter, auditionTools,
        referenceWave, preview;
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
        result.auditionMeter = result.waveform.removeFromRight(18);
        result.waveform.removeFromRight(4);
        return result;
    }
    const int sidebar = std::clamp(width / 5, 272, 320);
    const int left = sidebar + 14, contentWidth = std::max(1, width - left - 14);
    const int detailWidth = std::clamp(width / 5, 218, 280);
    const int auditionHeight = result.small ? 238 : 300;
    const int auditionTop = height - auditionHeight;
    result.collections = {0, 0, sidebar, height};
    result.preview = {left, auditionTop - 8, contentWidth, auditionHeight - 22};
    result.selection = result.preview.reduced(12, 0).withTrimmedTop(8).withHeight(26);
    result.referenceTop = 101;
    result.referenceHeight = reference ? (result.small ? 136 : 156) : 0;
    const int top = reference ? result.referenceTop + result.referenceHeight + 8 : 103;
    const int resultsWidth = std::max(1, contentWidth - detailWidth - 14);
    result.results = {left, top, resultsWidth, std::max(0, auditionTop - 16 - top)};
    result.details = {result.results.getRight() + 14, 12, detailWidth, result.results.getBottom() - 12};
    result.referenceWave = {left, result.referenceTop + 59, resultsWidth,
                            std::max(0, result.referenceHeight - 79)};
    result.waveform = {result.selection.getX(), auditionTop + 34, result.selection.getWidth(),
                       auditionHeight - 148};
    result.auditionTools = result.waveform.withX(result.details.getX()).withWidth(result.details.getWidth())
                               .withTop(result.selection.getY());
    result.waveform.setRight(result.auditionTools.getX() - 12);
    result.auditionMeter = result.waveform.removeFromRight(24);
    result.waveform.removeFromRight(5);
    result.controlsY = height - 105;
    result.monitorY = height - 58;
    result.statusY = height - 28;
    return result;
}
} // namespace sp
