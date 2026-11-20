#pragma once
#include "Theme.h"
namespace sp
{
enum class ActionIcon
{
    play,
    match,
    whole,
    previous,
    next,
    pause,
    refresh,
    folder,
    download,
    cancel,
    similar,
    file,
    zoom,
    fit,
    reconnect,
    markIn,
    markOut,
    scanFolder,
    retryAnalysis,
    addLayer,
    duplicate,
    up,
    down,
    trash,
    save,
    open,
    undo,
    redo,
    variations,
    lock,
    render,
    drag,
    keep,
    settings,
    layers,
    browser,
    chevron,
    mute,
    solo,
    newDocument,
    rename,
    history,
    compare,
    zoomOut,
    fitSelection,
    snap,
    scrub,
    verticalIn,
    verticalOut,
    accurate,
    colour,
    stop,
    installed,
    unlocked,
    unmuted,
    chevronDown,
    renderSelection,
    copyOriginal,
    inspector,
    processing,
    check,
    repeat,
    repeatOne,
    effectEnabled,
    addEffect,
    more,
    count
};
namespace icons
{
enum class Tone
{
    normal,
    hover,
    selected,
    disabled,
    primary,
    focus,
    count
};
// Called by controls at construction, never lazily by paint or an audio callback.
void preload();
void draw(juce::Graphics &, ActionIcon, juce::Rectangle<float>, Tone = Tone::normal);
ActionIcon forState(ActionIcon, bool selected);
const char *resourceName(ActionIcon);
int parsedAssetCount();
juce::AffineTransform viewBoxTransform(juce::Rectangle<float>);
} // namespace icons
} // namespace sp
