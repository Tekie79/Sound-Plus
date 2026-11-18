#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace sp
{
enum class FadeShape
{
    linear,
    equalPower,
    slow,
    fast,
    sCurve,
    custom
};
inline constexpr std::array fadeShapes{FadeShape::linear, FadeShape::equalPower, FadeShape::slow,
                                       FadeShape::fast, FadeShape::sCurve};
inline constexpr const char *fadeShapeId(FadeShape shape)
{
    switch (shape)
    {
    case FadeShape::equalPower:
        return "equal-power";
    case FadeShape::slow:
        return "slow";
    case FadeShape::fast:
        return "fast";
    case FadeShape::sCurve:
        return "s-curve";
    case FadeShape::custom:
        return "custom";
    default:
        return "linear";
    }
}
inline constexpr const char *fadeShapeName(FadeShape shape)
{
    switch (shape)
    {
    case FadeShape::equalPower:
        return "Equal power";
    case FadeShape::slow:
        return "Slow";
    case FadeShape::fast:
        return "Fast";
    case FadeShape::sCurve:
        return "S curve";
    case FadeShape::custom:
        return "Custom";
    default:
        return "Linear";
    }
}
// The same normalized amplitude law drives the waveform overlay and stock DSP.
// OUT evaluates the rising law backwards; overlapping fades multiply.
inline double fadeGain(FadeShape shape, double progress)
{
    auto x = std::clamp(progress, 0., 1.);
    switch (shape)
    {
    case FadeShape::equalPower:
        return std::sin(x * 1.57079632679489661923);
    case FadeShape::slow:
        return x * x;
    case FadeShape::fast:
        return std::sqrt(x);
    case FadeShape::sCurve:
        return x * x * (3 - 2 * x);
    default:
        return x;
    }
}
struct FadePoint
{
    double x = .5, y = .5;
    bool operator==(const FadePoint &) const = default;
};
inline constexpr int maxFadePoints = 32;
inline constexpr double fadePointEpsilon = 1e-9;
// Prepared only when the recipe changes, never in the device callback. PCHIP
// interpolates every control point, permits rises/dips and avoids overshoot.
class PreparedFade
{
    struct Segment
    {
        double first, last, inverseWidth, y, a, b, c;
    };
    std::array<Segment, maxFadePoints + 1> segments{};
    size_t count = 0;
    FadeShape shape = FadeShape::linear;

  public:
    PreparedFade() = default;
    PreparedFade(FadeShape preset, const std::vector<FadePoint> &points = {})
    {
        reset(preset, points);
    }
    void reset(FadeShape preset, const std::vector<FadePoint> &points)
    {
        shape = preset;
        count = 0;
        if (shape != FadeShape::custom)
            return;
        std::array<double, maxFadePoints + 2> x{}, y{}, h{}, slope{}, tangent{};
        auto n = std::min(points.size(), (size_t)maxFadePoints) + 2;
        for (size_t i = 1; i + 1 < n; ++i)
        {
            x[i] = points[i - 1].x;
            y[i] = points[i - 1].y;
        }
        x[n - 1] = y[n - 1] = 1;
        for (size_t i = 0; i + 1 < n; ++i)
        {
            h[i] = x[i + 1] - x[i];
            slope[i] = (y[i + 1] - y[i]) / h[i];
        }
        if (n == 2)
            tangent[0] = tangent[1] = 1;
        else
        {
            auto endpoint = [](double width, double nextWidth, double delta, double nextDelta)
            {
                auto value = ((2 * width + nextWidth) * delta - width * nextDelta) / (width + nextWidth);
                if (value * delta <= 0)
                    return 0.;
                if (delta * nextDelta < 0 && std::abs(value) > 3 * std::abs(delta))
                    return 3 * delta;
                return value;
            };
            tangent[0] = endpoint(h[0], h[1], slope[0], slope[1]);
            tangent[n - 1] = endpoint(h[n - 2], h[n - 3], slope[n - 2], slope[n - 3]);
            for (size_t i = 1; i + 1 < n; ++i)
                if (slope[i - 1] * slope[i] > 0)
                {
                    auto w1 = 2 * h[i] + h[i - 1], w2 = h[i] + 2 * h[i - 1];
                    tangent[i] = (w1 + w2) / (w1 / slope[i - 1] + w2 / slope[i]);
                }
        }
        for (size_t i = 0; i + 1 < n; ++i)
        {
            auto difference = y[i + 1] - y[i];
            segments[count++] = {x[i],
                                 x[i + 1],
                                 1 / h[i],
                                 y[i],
                                 h[i] * tangent[i],
                                 3 * difference - h[i] * (2 * tangent[i] + tangent[i + 1]),
                                 -2 * difference + h[i] * (tangent[i] + tangent[i + 1])};
        }
    }
    double value(double progress) const
    {
        auto x = std::clamp(progress, 0., 1.);
        if (shape != FadeShape::custom || count == 0)
            return fadeGain(shape, x);
        auto end = segments.begin() + (ptrdiff_t)count;
        auto found = std::lower_bound(segments.begin(), end, x,
                                      [](auto &segment, double v) { return segment.last < v; });
        if (found == end)
            --found;
        auto t = (x - found->first) * found->inverseWidth;
        return std::clamp(((found->c * t + found->b) * t + found->a) * t + found->y, 0., 1.);
    }
};
inline std::vector<FadePoint> seedFadePoints(FadeShape shape, const std::vector<FadePoint> &points = {})
{
    if (shape == FadeShape::custom)
        return points;
    PreparedFade curve(shape, points);
    std::vector<FadePoint> result;
    result.push_back({.5, curve.value(.5)});
    return result;
}
} // namespace sp
