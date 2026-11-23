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
    juce::Rectangle<int> results, selection, waveform, referenceWave, preview;
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
    const int left = 212, inset = 14;
    const int previewWidth = std::clamp(width * 34 / 100, 390, 540);
    const int split = width - previewWidth;
    result.preview = {split + 5, 12, previewWidth - 17, height - 24};
    result.selection = result.preview.reduced(12, 0).withTrimmedTop(54).withHeight(26);
    result.referenceTop = 101;
    result.referenceHeight = reference ? (result.small ? 112 : 139) : 0;
    const int top = reference ? result.referenceTop + result.referenceHeight + 8 : 103;
    result.results = {left + inset, top, std::max(1, split - left - inset * 2),
                      std::max(44, height - top - 42)};
    result.referenceWave = {left + inset, result.referenceTop + 59, result.results.getWidth(),
                            result.referenceHeight - 64};
    auto inner = result.preview.reduced(12, 0);
    result.waveform = {inner.getX(), 142, inner.getWidth(), std::clamp(height - 402, 96, 460)};
    result.controlsY = result.waveform.getBottom() + 12;
    result.monitorY = result.controlsY + 108;
    result.statusY = height - 32;
    return result;
}
} // namespace sp
