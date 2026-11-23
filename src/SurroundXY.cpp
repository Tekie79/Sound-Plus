#include "TrackModel.h"
#include <cmath>

namespace sp
{
bool squareSurroundLayout(const ChannelLayout &layout)
{
    if (layout.size() != 6 || layout.roles[0] != "L" || layout.roles[1] != "R" || layout.roles[2] != "C" ||
        layout.roles[3] != "LFE")
        return false;
    return (layout.id == "5.1-side-v1" && layout.roles[4] == "Ls" && layout.roles[5] == "Rs") ||
           (layout.id == "5.1-rear-v1" && layout.roles[4] == "Lb" && layout.roles[5] == "Rb");
}

juce::Point<double> squareSpeakerPosition(const juce::String &role)
{
    if (role == "L")
        return {-1, 1};
    if (role == "R")
        return {1, 1};
    if (role == "C")
        return {0, 1};
    if (role == "Ls")
        return {-1, 0};
    if (role == "Rs")
        return {1, 0};
    if (role == "Lb")
        return {-1, -1};
    if (role == "Rb")
        return {1, -1};
    throw std::runtime_error("Unknown square surround speaker role");
}

std::array<float, 16> squareSurroundGains(const ChannelLayout &bus, double x, double y)
{
    if (!squareSurroundLayout(bus) || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1 ||
        std::abs(y) > 1)
        throw std::runtime_error("Square panning requires a named 5.1 bus and positions within the square");

    constexpr auto pi = juce::MathConstants<double>::pi;
    const bool side = bus.id == "5.1-side-v1";
    const double surroundAngle = side ? pi / 2 : 3 * pi / 4;
    const std::array<double, 6> angles{-surroundAngle, -pi / 4,       0,
                                       pi / 4,         surroundAngle, 2 * pi - surroundAngle};
    const std::array<int, 6> speakers{bus.roles.indexOf(side ? "Ls" : "Lb"),
                                      bus.roles.indexOf("L"),
                                      bus.roles.indexOf("C"),
                                      bus.roles.indexOf("R"),
                                      bus.roles.indexOf(side ? "Rs" : "Rb"),
                                      bus.roles.indexOf(side ? "Ls" : "Lb")};
    const double radius = std::max(std::abs(x), std::abs(y));
    double direction = radius > 0 ? std::atan2(x, y) : 0;
    if (direction < angles.front())
        direction += 2 * pi;
    auto upper = std::upper_bound(angles.begin() + 1, angles.end(), direction);
    const auto segment = std::min<size_t>(4, (size_t)(upper - angles.begin() - 1));
    const double fraction =
        std::clamp((direction - angles[segment]) / (angles[segment + 1] - angles[segment]), 0., 1.);
    std::array<double, 16> power{};
    for (size_t i = 0; i < 5; ++i)
        power[(size_t)speakers[i]] = (1 - radius) / 5;
    power[(size_t)speakers[segment]] += radius * (1 - fraction);
    power[(size_t)speakers[segment + 1]] += radius * fraction;
    std::array<float, 16> gains{};
    for (int c = 0; c < bus.size(); ++c)
        gains[(size_t)c] = (float)std::sqrt(power[(size_t)c]);
    return gains;
}

ChannelMatrix squareSurroundMatrix(const ChannelLayout &input, const ChannelLayout &output,
                                   SurroundPosition position)
{
    if (!(input.id == "mono-v1" && input.size() == 1 && input.roles[0] == "C") &&
        !(input.id == "stereo-v1" && input.size() == 2 && input.roles[0] == "L" && input.roles[1] == "R"))
        throw std::runtime_error("Square panning requires a named Mono or Stereo track");
    if (!std::isfinite(position.spread) || position.spread < 0 || position.spread > 1 ||
        !std::isfinite(position.x) || std::abs(position.x) > 1)
        throw std::runtime_error("Invalid square object position or stereo spread");
    ChannelMatrix result{};
    for (int c = 0; c < input.size(); ++c)
    {
        const bool stereo = input.size() == 2;
        const auto half = stereo ? position.spread * .5 : 0.;
        auto gains =
            squareSurroundGains(output, position.x * (1 - half) + (c == 0 ? -half : half), position.y);
        for (int channel = 0; channel < output.size(); ++channel)
            result[channel][c] = gains[(size_t)channel] * (stereo ? std::sqrt(.5f) : 1.f);
    }
    return result;
}

bool trackSquareSurroundSupported(const DesignMix &mix, const DesignTrack &track)
{
    if ((track.kind != TrackKind::audio && track.kind != TrackKind::midiInstrument) ||
        (track.layout != channelLayout("mono-v1") && track.layout != channelLayout("stereo-v1")) ||
        !squareSurroundLayout(trackOutputLayout(mix, track)))
        return false;
    for (auto &clip : mix.layers)
        if (clip.trackId == track.id &&
            (clip.routing.spatial || clip.routing.legacyAnalysisMean || clip.routing.needsSelection ||
             (clip.routing.interpretation != channelLayout("mono-v1") &&
              clip.routing.interpretation != channelLayout("stereo-v1")) ||
             (clip.routing.interpretation.size() == 2 && track.layout.size() != 2)))
            return false;
    return true;
}

void validateSquareSurround(const DesignMix &mix, const DesignTrack &track)
{
    if (!trackSquareSurroundSupported(mix, track))
        throw std::runtime_error("Square Surround requires Mono/Stereo audio into named 5.1 Side or Rear. "
                                 "Use a compatible clip routing and preserve stereo channels.");
    squareSurroundMatrix(track.layout, trackOutputLayout(mix, track), *track.surroundXY);
}

bool hasSquareSurround(const DesignMix &mix)
{
    return std::any_of(mix.tracks.begin(), mix.tracks.end(),
                       [](const auto &t) { return t.surroundXY.has_value(); });
}

bool hasSquareSurround(const DesignDocument &doc)
{
    return hasSquareSurround(doc.original) ||
           std::any_of(doc.variations.begin(), doc.variations.end(),
                       [](const auto &v) { return hasSquareSurround(v.mix); }) ||
           std::any_of(doc.takes.begin(), doc.takes.end(),
                       [](const auto &t) { return hasSquareSurround(t.recipe); });
}

juce::var squareSurroundJson(SurroundPosition p)
{
    auto v = juce::var(new juce::DynamicObject);
    auto *o = v.getDynamicObject();
    o->setProperty("policy", "square-surround-v1");
    o->setProperty("x", p.x);
    o->setProperty("y", p.y);
    o->setProperty("spread", p.spread);
    return v;
}

SurroundPosition squareSurroundFromJson(const juce::var &v)
{
    const auto number = [](const juce::var &x) { return x.isDouble() || x.isInt() || x.isInt64(); };
    if (v["policy"].toString() != "square-surround-v1" || !number(v["x"]) || !number(v["y"]) ||
        !number(v["spread"]))
        throw std::runtime_error("Unsupported square panning policy");
    return {(double)v["x"], (double)v["y"], (double)v["spread"]};
}
} // namespace sp
