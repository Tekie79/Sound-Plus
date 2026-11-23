#include "TrackModel.h"
#include "SurroundTouch.h"
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
                       [](const auto &t)
                       {
                           return t.surroundXY ||
                                  std::any_of(t.automation.curves.begin(), t.automation.curves.end(),
                                              [](const auto &c)
                                              {
                                                  return !c.points.empty() &&
                                                         (c.target == surroundX || c.target == surroundY ||
                                                          c.target == surroundSpread);
                                              });
                       });
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

namespace sp
{
namespace
{
double axisAt(const DesignTrack &track, const AutomationTarget &target, double base, int64_t frame,
              bool readOnly)
{
    auto *curve = automationCurve(track.automation, target);
    if (!curve || (readOnly && !curve->read))
        return base;
    AutomationParameter parameter;
    parameter.base = base;
    return automationValue(*curve, parameter, frame);
}
} // namespace

SurroundPosition surroundPositionAt(const DesignTrack &track, int64_t frame, bool readOnly)
{
    auto p = track.surroundXY.value_or(SurroundPosition{});
    return {axisAt(track, surroundX, p.x, frame, readOnly), axisAt(track, surroundY, p.y, frame, readOnly),
            axisAt(track, surroundSpread, p.spread, frame, readOnly)};
}

std::vector<SurroundNode> surroundNodes(const DesignTrack &track)
{
    std::set<int64_t> frames;
    for (const auto &axis : {surroundX, surroundY})
        if (auto *curve = automationCurve(track.automation, axis))
            for (const auto &p : curve->points)
                frames.insert(p.frame);
    std::vector<SurroundNode> nodes;
    nodes.reserve(frames.size());
    for (auto frame : frames)
    {
        const auto p = surroundPositionAt(track, frame, false);
        MotionShape shape = MotionShape::linear;
        if (auto *curve = automationCurve(track.automation, surroundX))
        {
            auto i = std::lower_bound(curve->points.begin(), curve->points.end(), frame,
                                      [](const auto &point, auto f) { return point.frame < f; });
            if (i != curve->points.end() && i->frame == frame)
                shape = i->hold ? MotionShape::hold : i->smooth ? MotionShape::smooth : MotionShape::linear;
        }
        nodes.push_back({frame, p.x, p.y, shape});
    }
    return nodes;
}
void setSurroundNode(DesignTrack &track, SurroundNode node)
{
    for (const auto &[target, value] : {std::pair{surroundX, node.x}, std::pair{surroundY, node.y}})
    {
        auto *c = automationCurve(track.automation, target, true);
        auto at = std::lower_bound(c->points.begin(), c->points.end(), node.frame,
                                   [](const auto &p, auto frame) { return p.frame < frame; });
        if (at == c->points.end() || at->frame != node.frame)
            at = c->points.insert(at, {juce::Uuid().toString(), node.frame});
        at->value = std::clamp(value, -1., 1.);
        at->hold = node.shape == MotionShape::hold;
        bool smooth = node.shape == MotionShape::smooth;
        if (at->smooth != smooth)
        {
            at->smoothStart = 0;
            at->smoothEnd = 1;
        }
        at->smooth = smooth;
    }
}

int64_t squareMotionFrame(double seconds, const DesignMix &mix, double rate)
{
    const auto end = mixDuration(mix, rate);
    if (!std::isfinite(seconds) || seconds < 0 || seconds >= end / rate)
        throw std::runtime_error("Choose a time within the design.");
    const auto frame = (int64_t)std::llround(seconds * rate);
    if (frame >= end)
        throw std::runtime_error("Choose a time within the design.");
    return frame;
}

std::vector<SurroundNode> squareMotionPaste(const juce::String &text, const DesignMix &mix,
                                            const DesignTrack &track, double rate, int64_t anchor)
{
    if (!text.startsWith(squareMotionClipboardHeader))
        throw std::runtime_error("Copy motion points from a Square Surround panner.");
    if (text.getNumBytesAsUTF8() > 512 * 1024)
        throw std::runtime_error("Copy a smaller selection of motion points.");
    const auto decoded = juce::JSON::parse(text.fromFirstOccurrenceOf("\n", false, false));
    const auto *rows = decoded.getArray();
    if (!rows || rows->isEmpty() || rows->size() > maxAutomationCurvePoints)
        throw std::runtime_error("Copy up to 2,048 motion points from the panner.");

    const auto end = mixDuration(mix, rate);
    if (anchor < 0 || anchor >= end)
        throw std::runtime_error("Move the playhead inside the design before pasting.");
    std::vector<SurroundNode> nodes;
    nodes.reserve((size_t)rows->size());
    std::set<int64_t> frames;
    double firstSeconds = 0;
    for (const auto &value : *rows)
    {
        const auto *row = value.getArray();
        if (!row || row->size() != 4)
            throw std::runtime_error("Invalid motion point. Copy the points again.");
        const auto numeric = [](const juce::var &v) { return v.isInt() || v.isInt64() || v.isDouble(); };
        if (!numeric((*row)[0]) || !numeric((*row)[1]) || !numeric((*row)[2]) ||
            !((*row)[3].isInt() || (*row)[3].isInt64()))
            throw std::runtime_error("Invalid motion point. Copy the points again.");
        const auto seconds = (double)(*row)[0], x = (double)(*row)[1], y = (double)(*row)[2];
        const auto shape = (int64_t)(*row)[3];
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 3600 || !std::isfinite(x) ||
            !std::isfinite(y) || std::abs(x) > 1 || std::abs(y) > 1 || shape < (int)MotionShape::linear ||
            shape > (int)MotionShape::smooth)
            throw std::runtime_error("Motion time, position, or shape is outside its range.");
        if (nodes.empty())
            firstSeconds = seconds;
        const auto offset = seconds - firstSeconds;
        if (offset < 0 || offset >= (end - anchor) / rate)
            throw std::runtime_error("Motion extends past the design. Paste earlier.");
        const auto frame = anchor + (int64_t)std::llround(offset * rate);
        if (frame >= end)
            throw std::runtime_error("Motion extends past the design. Paste earlier.");
        if (!frames.insert(frame).second)
            throw std::runtime_error("Two motion points land on the same frame.");
        nodes.push_back({frame, x, y, (MotionShape)shape});
    }

    size_t added = 0, newCurves = 0;
    for (const auto &axis : {surroundX, surroundY})
    {
        const auto *curve = automationCurve(track.automation, axis);
        const auto replacements = curve
                                      ? std::count_if(curve->points.begin(), curve->points.end(),
                                                      [&](const auto &p) { return frames.contains(p.frame); })
                                      : 0;
        const auto growth = nodes.size() - (size_t)replacements;
        if (growth + (curve ? curve->points.size() : 0) > maxAutomationCurvePoints)
            throw std::runtime_error("Remove some X/Y points before pasting.");
        newCurves += curve == nullptr;
        added += growth;
    }
    if (track.automation.curves.size() + newCurves > 256)
        throw std::runtime_error("Remove unused automation curves before pasting.");
    const auto count = [](const AutomationLane &lane)
    {
        size_t total = 0;
        for (const auto &curve : lane.curves)
            total += curve.points.size();
        return total;
    };
    size_t pointCount = count(mix.masterAutomation);
    for (const auto &t : mix.tracks)
        pointCount += count(t.automation);
    if (pointCount + added > maxAutomationPoints)
        throw std::runtime_error("Remove some design automation points before pasting.");
    return nodes;
}

AutomationPoint &splitSquareSegment(AutomationCurve &curve, int64_t frame, double base)
{
    auto next = std::lower_bound(curve.points.begin(), curve.points.end(), frame,
                                 [](auto &p, int64_t time) { return p.frame < time; });
    if (next != curve.points.end() && next->frame == frame)
        return *next;
    AutomationParameter parameter;
    parameter.base = base;
    AutomationPoint split{juce::Uuid().toString(), frame, automationValue(curve, parameter, frame)};
    if (next != curve.points.begin() && next != curve.points.end())
    {
        auto &previous = *std::prev(next);
        split.hold = previous.hold;
        if (previous.smooth)
        {
            auto fraction = (double)(frame - previous.frame) / (next->frame - previous.frame);
            auto phase = std::lerp(previous.smoothStart, previous.smoothEnd, fraction);
            if (phase > previous.smoothStart && phase < previous.smoothEnd)
            {
                split.smooth = true;
                split.smoothStart = phase;
                split.smoothEnd = previous.smoothEnd;
                previous.smoothEnd = phase;
            }
        }
    }
    else
        split.hold = true;
    return *curve.points.insert(next, std::move(split));
}
void eraseSurroundNodes(DesignTrack &track, const std::set<int64_t> &frames)
{
    for (const auto &axis : {surroundX, surroundY})
        if (auto *c = automationCurve(track.automation, axis))
            std::erase_if(c->points, [&](const auto &point) { return frames.contains(point.frame); });
}
} // namespace sp

namespace sp
{
std::vector<SquareTouchPass::Point> unpackSquareTouch(const juce::var &receipt)
{
    const auto count = (int)receipt["pointCount"];
    const auto packed = receipt["packedPoints"].toString();
    if ((int)receipt["wireVersion"] != 1 || count < 0 || count > (int)squareTouchPointLimit ||
        packed.length() != count * 16)
        throw std::runtime_error("Invalid Square Surround receipt envelope");
    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, packed) || decoded.getDataSize() != (size_t)count * 12)
        throw std::runtime_error("Invalid Square Surround receipt payload");
    juce::MemoryInputStream input(decoded.getData(), decoded.getDataSize(), false);
    std::vector<SquareTouchPass::Point> points;
    points.reserve((size_t)count);
    for (int i = 0; i < count; ++i)
    {
        auto frame = (int64_t)input.readInt();
        double x = std::bit_cast<float>(input.readInt());
        double y = std::bit_cast<float>(input.readInt());
        points.push_back({frame, {x, y}});
    }
    return points;
}
void retainSquareTouch(DesignMix &mix, const juce::String &owner, const juce::var &receipt, double rate)
{
    auto *track = findTrack(mix, owner);
    auto applied = unpackSquareTouch(receipt);
    if (!track || !track->surroundXY || applied.empty())
        throw std::runtime_error("No matching applied Square Surround Touch motion");
    for (auto key : {"returnX", "returnY"})
    {
        const auto v = (double)receipt[key];
        if (!std::isfinite(v) || std::abs(v) > 1)
            throw std::runtime_error("Invalid Square Surround return value");
    }
    const auto release = (int64_t)receipt["release"];
    const auto returnEnd = (int64_t)receipt["returnEnd"];
    if (returnEnd - release != (int64_t)std::llround(rate * .1))
        throw std::runtime_error("Invalid Square Surround return frame");
    std::array<std::vector<AutomationPoint>, 2> axes;
    int64_t previous = -1;
    for (auto &row : applied)
    {
        const auto frame = row.frame;
        if (frame <= previous || frame > release)
            throw std::runtime_error("Invalid Square Surround gesture frame");
        previous = frame;
        for (int axis = 0; axis < 2; ++axis)
        {
            const auto value = axis == 0 ? row.position.x : row.position.y;
            if (!std::isfinite(value) || std::abs(value) > 1)
                throw std::runtime_error("Invalid Square Surround gesture value");
            axes[(size_t)axis].push_back({juce::Uuid().toString(), frame, value, frame != release});
        }
    }
    for (int axis = 0; axis < 2; ++axis)
    {
        const auto &target = axis == 0 ? surroundX : surroundY;
        auto *curve = automationCurve(track->automation, target, true);
        auto base = axis == 0 ? track->surroundXY->x : track->surroundXY->y;
        const auto first = axes[(size_t)axis].front().frame;
        if (first > 0)
        {
            auto &edge = splitSquareSegment(*curve, first - 1, base);
            edge.hold = true;
            edge.smooth = false;
            edge.smoothStart = 0;
            edge.smoothEnd = 1;
        }
        auto suffix = splitSquareSegment(*curve, returnEnd, base);
        curve->read = true;
        auto endpoint = (double)receipt[axis == 0 ? "returnX" : "returnY"];
        writeTouchPass(mix, owner, target, axes[(size_t)axis], release, rate, endpoint);
        curve = automationCurve(track->automation, target);
        for (auto &point : curve->points)
            if (point.frame == returnEnd)
            {
                point = suffix;
                point.value = endpoint;
                break;
            }
    }
}
} // namespace sp
