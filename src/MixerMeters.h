#pragma once
#include "ChannelLayout.h"
#include <array>
#include <cmath>

namespace sp
{
// A fixed 30 Hz window on the render producer. Clip counters let each
// console reset its indicator without modifying the shared audio graph.
class MixerMeter
{
  public:
    void prepare(double sampleRate) { window = juce::jmax(1, (int)std::lround(sampleRate / 30.)); }
    void measure(int channel, float sample)
    {
        const auto magnitude = std::abs(sample);
        pendingPeak[channel] = std::max(pendingPeak[channel], magnitude);
        power[channel] += (double)sample * sample;
        clips[channel] += magnitude >= 1.f;
    }
    void advance(int channels)
    {
        if (++frames == window)
        {
            for (int c = 0; c < channels; ++c)
                rms[c] = (float)std::sqrt(power[c] / window);
            peak = pendingPeak;
            pendingPeak.fill(0);
            power.fill(0);
            frames = 0;
        }
    }
    juce::var snapshot(const juce::String &owner, const ChannelLayout &layout) const
    {
        auto *object = new juce::DynamicObject();
        object->setProperty("owner", owner);
        object->setProperty("layout", layoutJson(layout));
        juce::Array<juce::var> rows;
        for (int c = 0; c < layout.size(); ++c)
        {
            auto *row = new juce::DynamicObject();
            row->setProperty("peak", std::max(peak[c], pendingPeak[c]));
            row->setProperty("rms", rms[c]);
            row->setProperty("clips", clips[c]);
            rows.add(juce::var(row));
        }
        object->setProperty("channels", rows);
        return juce::var(object);
    }

  private:
    int window = 1600, frames = 0;
    std::array<double, 16> power{};
    std::array<float, 16> peak{}, rms{}, pendingPeak{};
    std::array<juce::int64, 16> clips{};
};
} // namespace sp
