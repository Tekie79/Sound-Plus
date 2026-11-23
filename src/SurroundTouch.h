#pragma once
#include "TrackModel.h"
#include <bit>

namespace sp
{
inline constexpr size_t squareTouchPointLimit = 2044;

// Owned and serviced by a prepared graph on the PCM producer only.
struct SquareTouchPass
{
    struct Point
    {
        int64_t frame;
        juce::Point<double> position;
    };
    juce::String id, owner;
    juce::var generation, graphEpoch;
    DesignTrack underlying;
    std::vector<Point> points;
    std::optional<juce::Point<double>> pending;
    juce::Point<double> position, resume;
    bool ending = false, interrupted = false;
    int64_t release = -1, returnEnd = -1;
    juce::var receipt;

    void finish(int64_t frame, double rate, bool interruption)
    {
        if (release >= 0)
            return;
        interrupted = interruption;
        release = points.empty() ? frame : std::max(frame, points.back().frame);
        returnEnd = release + (int64_t)std::llround(rate * .1);
        auto target = surroundPositionAt(underlying, returnEnd);
        resume = {target.x, target.y};
        pending.reset();
    }
    void apply(int64_t frame, double rate)
    {
        if (release >= 0)
            return;
        if (pending)
        {
            if (!points.empty() && points.back().frame == frame)
                points.back().position = position = *pending;
            else if (points.size() < squareTouchPointLimit)
            {
                position = *pending;
                points.push_back({frame, position});
            }
            else
            {
                finish(frame, rate, true);
                return;
            }
            pending.reset();
        }
        if (ending)
            finish(frame, rate, false);
    }
    juce::Point<double> value(int64_t frame) const
    {
        if (release < 0 || frame <= release)
            return position;
        const auto fraction = std::clamp((double)(frame - release) / (returnEnd - release), 0., 1.);
        return position + (resume - position) * fraction;
    }
    juce::var result()
    {
        if (release < 0)
            return {};
        if (receipt.isObject())
            return receipt;
        auto *o = new juce::DynamicObject;
        receipt = juce::var(o);
        o->setProperty("pass", id);
        o->setProperty("owner", owner);
        o->setProperty("playGeneration", generation);
        o->setProperty("graphEpoch", graphEpoch);
        o->setProperty("release", (juce::int64)release);
        o->setProperty("returnEnd", (juce::int64)returnEnd);
        o->setProperty("returnX", resume.x);
        o->setProperty("returnY", resume.y);
        o->setProperty("interrupted", interrupted);
        juce::MemoryOutputStream applied((size_t)12 * points.size());
        for (auto &p : points)
        {
            applied.writeInt((int)p.frame);
            applied.writeInt(std::bit_cast<int>((float)p.position.x));
            applied.writeInt(std::bit_cast<int>((float)p.position.y));
        }
        o->setProperty("wireVersion", 1);
        o->setProperty("pointCount", (int)points.size());
        o->setProperty("packedPoints", juce::Base64::toBase64(applied.getData(), applied.getDataSize()));
        return receipt;
    }
};
std::vector<SquareTouchPass::Point> unpackSquareTouch(const juce::var &);
void retainSquareTouch(DesignMix &, const juce::String &owner, const juce::var &, double rate);
} // namespace sp
