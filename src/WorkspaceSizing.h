#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp::ui::workspace
{
// Shared logical-point widths stay equal on Retina and standard displays.
inline constexpr int sidebarWidth = 320;
inline constexpr int minimumWindowWidth = 1040, minimumWindowHeight = 680;
inline constexpr int maximumPadExtent = 640;
inline juce::Point<int> boundedPadSize(juce::Point<int> available)
{
    int width = std::clamp(available.x, 0, maximumPadExtent);
    int height = std::clamp(available.y, 0, maximumPadExtent);
    // Permit a responsive rectangle, with an aspect ratio bounded to 2:3 .. 3:2.
    if (width * 2 > height * 3)
        width = height * 3 / 2;
    if (height * 2 > width * 3)
        height = width * 3 / 2;
    return {width, height};
}
} // namespace sp::ui::workspace
