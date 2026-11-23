#include "IconRegistry.h"
#include "IconAssets.h"
#include <array>
#include <map>
#include <stdexcept>
namespace sp::icons
{
namespace
{
constexpr const char *resources[]{
    "lucide_play_svg",                        // play
    "audio_play_range_svg",                   // match
    "audio_play_whole_svg",                   // whole
    "lucide_skip_back_svg",                   // previous
    "lucide_skip_forward_svg",                // next
    "lucide_pause_svg",                       // pause
    "lucide_refresh_cw_svg",                  // refresh
    "lucide_folder_plus_svg",                 // folder
    "lucide_download_svg",                    // download
    "lucide_x_svg",                           // cancel
    "lucide_search_svg",                      // similar
    "lucide_file_volume_svg",                 // file
    "lucide_zoom_in_svg",                     // zoom
    "lucide_scan_svg",                        // fit
    "lucide_plug_zap_svg",                    // reconnect
    "mark_in_svg",                            // markIn
    "mark_out_svg",                           // markOut
    "lucide_folder_search_svg",               // scanFolder
    "audio_retry_svg",                        // retryAnalysis
    "lucide_list_plus_svg",                   // addLayer
    "lucide_copy_plus_svg",                   // duplicate
    "lucide_arrow_up_svg",                    // up
    "lucide_arrow_down_svg",                  // down
    "lucide_trash_svg",                       // trash
    "lucide_save_svg",                        // save
    "lucide_folder_open_svg",                 // open
    "lucide_undo_2_svg",                      // undo
    "lucide_redo_2_svg",                      // redo
    "lucide_dices_svg",                       // variations
    "lucide_lock_svg",                        // lock
    "lucide_file_output_svg",                 // render
    "audio_file_drag_svg",                    // drag
    "lucide_star_svg",                        // keep
    "lucide_settings_2_svg",                  // settings
    "lucide_layers_svg",                      // layers
    "lucide_panel_left_svg",                  // browser
    "lucide_chevron_right_svg",               // chevron
    "lucide_volume_x_svg",                    // mute
    "lucide_headphones_svg",                  // solo
    "lucide_file_plus_svg",                   // newDocument
    "lucide_pencil_svg",                      // rename
    "lucide_rotate_ccw_clock_svg",            // history
    "lucide_columns_2_svg",                   // compare
    "lucide_zoom_out_svg",                    // zoomOut
    "lucide_focus_svg",                       // fitSelection
    "lucide_magnet_svg",                      // snap
    "lucide_audio_waveform_svg",              // scrub
    "lucide_arrows_from_axis_horizontal_svg", // verticalIn
    "lucide_arrows_to_axis_horizontal_svg",   // verticalOut
    "lucide_circle_check_svg",                // accurate
    "lucide_palette_svg",                     // colour
    "lucide_square_svg",                      // stop
    "lucide_circle_check_svg",                // installed
    "lucide_lock_open_svg",                   // unlocked
    "lucide_volume_2_svg",                    // unmuted
    "lucide_chevron_down_svg",                // chevronDown
    "render_selection_svg",                   // renderSelection
    "lucide_folder_input_svg",                // copyOriginal
    "lucide_panel_right_svg",                 // inspector
    "lucide_sliders_horizontal_svg",          // processing
    "lucide_check_svg",                       // check
    "lucide_repeat_svg",                      // repeat
    "lucide_repeat_1_svg",                    // repeatOne
    "lucide_power_svg",                       // effectEnabled
    "lucide_plus_svg",                        // addEffect
    "lucide_ellipsis_svg",                    // more
    "timeline_marker_svg",
    "timeline_marker_previous_svg",
    "timeline_marker_next_svg",
    "lucide_circle_svg",
    "lucide_mic_svg",
};
static_assert(std::size(resources) == (size_t)ActionIcon::count);
using Images = std::array<std::unique_ptr<juce::Drawable>, (size_t)Tone::count>;
struct Registry
{
    std::map<juce::String, Images> cache;
    std::array<const Images *, (size_t)ActionIcon::count> actions{};
    int parsed = 0;
    static std::array<juce::Colour, (size_t)Tone::count> palette()
    {
        return {ui::button::normal, ui::button::hover, ui::accent, ui::button::disabled,
                ui::contrastingInk(), ui::button::focus};
    }
    decltype(palette()) inks = palette();
    void refresh()
    {
        auto next = palette();
        if (inks == next)
            return;
        for (auto &[name, images] : cache)
            for (size_t tone = 0; tone < inks.size(); ++tone)
                images[tone]->replaceColour(inks[tone], next[tone]);
        inks = next;
    }
    Registry()
    {
        for (size_t i = 0; i < std::size(resources); ++i)
        {
            auto [entry, added] = cache.try_emplace(resources[i]);
            if (added)
            {
                int length = 0;
                auto *data = IconAssets::getNamedResource(resources[i], length);
                auto xml = data ? juce::XmlDocument::parse(juce::String::fromUTF8(data, length)) : nullptr;
                if (!xml || xml->getStringAttribute("viewBox") != "0 0 24 24")
                    throw std::runtime_error(std::string("Missing/invalid icon: ") + resources[i]);
                // Resolve SVG currentColor in memory; local upstream assets stay unmodified.
                xml->setAttribute("stroke", "#000000");
                auto original = juce::Drawable::createFromSVG(*xml);
                if (!original)
                    throw std::runtime_error("Icon SVG could not be parsed");
                ++parsed;
                for (size_t tone = 0; tone < std::size(inks); ++tone)
                {
                    auto image = original->createCopy();
                    image->replaceColour(juce::Colours::black, inks[tone]);
                    entry->second[tone] = std::move(image);
                }
            }
            actions[i] = &entry->second;
        }
    }
};
// Message-thread construction before any control paints. No device-thread callers.
Registry &registry()
{
    static Registry value;
    return value;
}
Registry *ready = nullptr;
} // namespace
void preload()
{
    ready = &registry();
    ready->refresh();
}
int parsedAssetCount()
{
    return ready ? ready->parsed : 0;
}
const char *resourceName(ActionIcon action)
{
    return resources[(size_t)action];
}
juce::AffineTransform viewBoxTransform(juce::Rectangle<float> target)
{
    auto size = std::min(target.getWidth(), target.getHeight());
    auto square = target.withSizeKeepingCentre(size, size);
    return juce::AffineTransform::scale(size / 24.f).translated(square.getX(), square.getY());
}
void draw(juce::Graphics &g, ActionIcon action, juce::Rectangle<float> target, Tone tone)
{
    jassert(ready != nullptr);
    if (ready)
        (*ready->actions[(size_t)action])[(size_t)tone]->draw(g, 1, viewBoxTransform(target));
}
ActionIcon forState(ActionIcon action, bool selected)
{
    switch (action)
    {
    case ActionIcon::lock:
        return selected ? ActionIcon::lock : ActionIcon::unlocked;
    case ActionIcon::mute:
        return selected ? ActionIcon::mute : ActionIcon::unmuted;
    case ActionIcon::chevron:
        return selected ? ActionIcon::chevronDown : ActionIcon::chevron;
    default:
        return action;
    }
}
} // namespace sp::icons
