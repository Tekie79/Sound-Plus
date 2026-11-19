#include "ChannelLayout.h"
#include <cmath>
#include <set>
namespace sp
{
namespace
{
using CT = juce::AudioChannelSet::ChannelType;
struct Role
{
    const char *name;
    CT type;
    int bit;
    double angle;
};
const Role roles[] = {{"L", CT::left, 0, -30},
                      {"R", CT::right, 1, 30},
                      {"C", CT::centre, 2, 0},
                      {"LFE", CT::LFE, 3, 1000},
                      {"Lb", CT::leftSurround, 4, -150},
                      {"Rb", CT::rightSurround, 5, 150},
                      {"Ls", CT::leftSurroundSide, 9, -90},
                      {"Rs", CT::rightSurroundSide, 10, 90}};
const Role *role(const juce::String &s)
{
    for (auto &r : roles)
        if (s == r.name)
            return &r;
    return nullptr;
}
juce::var obj()
{
    return juce::var(new juce::DynamicObject());
}
void finite(double n, double a, double b)
{
    if (!std::isfinite(n) || n < a || n > b)
        throw std::runtime_error("Invalid channel routing parameter");
}
} // namespace
std::vector<ChannelLayout> namedLayouts()
{
    return {{"mono-v1", {"C"}},
            {"stereo-v1", {"L", "R"}},
            {"lcr-v1", {"L", "R", "C"}},
            {"quad-v1", {"L", "R", "Lb", "Rb"}},
            {"5.0-v1", {"L", "R", "C", "Ls", "Rs"}},
            {"5.1-side-v1", {"L", "R", "C", "LFE", "Ls", "Rs"}},
            {"5.1-rear-v1", {"L", "R", "C", "LFE", "Lb", "Rb"}},
            {"7.1-v1", {"L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs"}}};
}
ChannelLayout discreteLayout(int n, bool unknown)
{
    if (n < 1 || n > 64)
        throw std::runtime_error("Invalid discrete channel count");
    ChannelLayout l;
    l.id = (unknown ? "unknown-" : "discrete-") + juce::String(n) + "-v1";
    for (int i = 0; i < n; ++i)
        l.roles.add("D" + juce::String(i + 1));
    return l;
}
ChannelLayout channelLayout(const juce::String &id)
{
    for (auto l : namedLayouts())
        if (l.id == id)
            return l;
    for (auto prefix : {"discrete-", "unknown-"})
        if (id.startsWith(prefix) && id.endsWith("-v1"))
        {
            auto l = discreteLayout(id.substring((int)std::strlen(prefix)).getIntValue(),
                                    juce::String(prefix) == "unknown-");
            if (l.id == id)
                return l;
        }
    throw std::runtime_error("Unsupported channel layout: " + id.toStdString());
}
void validateLayout(const ChannelLayout &l, int maximum)
{
    if (l.size() < 1 || l.size() > maximum || channelLayout(l.id) != l)
        throw std::runtime_error("Unsupported layout identity/order");
}
juce::String layoutName(const ChannelLayout &l)
{
    if (l.id.isEmpty())
        return "Legacy stereo";
    if (l.id.startsWith("unknown-"))
        return "Unknown layout";
    if (l.id.startsWith("discrete-"))
        return "Discrete " + juce::String(l.size());
    if (l.id == "5.1-side-v1")
        return "5.1 Side";
    if (l.id == "5.1-rear-v1")
        return "5.1 Rear";
    return l.id.upToLastOccurrenceOf("-v1", false, false).toUpperCase();
}
juce::String layoutBadge(const ChannelLayout &l)
{
    return juce::String(l.size()) + juce::String::fromUTF8(" ch · ") + layoutName(l);
}
juce::var layoutJson(const ChannelLayout &l)
{
    auto o = obj();
    o.getDynamicObject()->setProperty("id", l.id);
    juce::Array<juce::var> a;
    for (auto &r : l.roles)
        a.add(r);
    o.getDynamicObject()->setProperty("roles", a);
    return o;
}
ChannelLayout layoutFromJson(const juce::var &v)
{
    ChannelLayout l;
    l.id = v["id"].toString();
    if (auto *a = v["roles"].getArray())
        for (auto &r : *a)
            l.roles.add(r.toString());
    validateLayout(l, 64);
    return l;
}
namespace
{
ChannelLayout producerLayout(const juce::AudioFormatReader &r)
{
    auto text = r.metadataValues.getValue(juce::WavAudioFormat::bwavDescription, {});
    if (text.startsWith("SoundPlus layout:") && text.contains("; order:"))
    {
        try
        {
            auto l = channelLayout(
                text.fromFirstOccurrenceOf(":", false, false).upToFirstOccurrenceOf(";", false, false));
            if (l.size() == (int)r.numChannels &&
                text.fromFirstOccurrenceOf("; order:", false, false).trim() == l.roles.joinIntoString(","))
                return l;
        }
        catch (...)
        {
        }
    }
    return {};
}
} // namespace
ChannelLayout detectedLayout(const juce::AudioFormatReader &r)
{
    auto mask = (uint32_t)r.metadataValues.getValue("ChannelMask", {}).getLargeIntValue();
    if (mask)
    {
        juce::StringArray a;
        uint32_t known = 0;
        for (auto &p : roles)
            if (mask & (1u << p.bit))
            {
                a.add(p.name);
                known |= 1u << p.bit;
            }
        if (known == mask && a.size() == (int)r.numChannels)
            for (auto l : namedLayouts())
                if (l.roles == a)
                    return l;
    }
    // A normative recognized channel mask takes precedence over free-form BWF text.
    if (auto declared = producerLayout(r); declared.id.isNotEmpty())
        return declared;
    if (mask)
        return discreteLayout((int)r.numChannels, true);
    if (r.numChannels == 1)
        return channelLayout("mono-v1");
    if (r.numChannels == 2)
        return channelLayout("stereo-v1");
    return discreteLayout((int)r.numChannels, true);
}
juce::String detectedLayoutConfidence(const juce::AudioFormatReader &r)
{
    auto l = detectedLayout(r);
    if (l.id.startsWith("unknown-"))
        return "unknown";
    auto mask = (uint32_t)r.metadataValues.getValue("ChannelMask", {}).getLargeIntValue();
    uint32_t expected = 0;
    for (auto &name : l.roles)
        if (auto *p = role(name))
            expected |= 1u << p->bit;
    if (mask && mask == expected)
        return "container-mask";
    if (producerLayout(r) == l)
        return "producer-declaration";
    return "conventional";
}
juce::AudioChannelSet juceChannelSet(const ChannelLayout &l)
{
    validateLayout(l, 64);
    if (l.id.startsWith("discrete-") || l.id.startsWith("unknown-"))
        return juce::AudioChannelSet::discreteChannels(l.size());
    juce::AudioChannelSet s;
    for (auto &r : l.roles)
        s.addChannel(role(r)->type);
    return s;
}
std::vector<int> wavOrder(const ChannelLayout &l)
{
    auto set = juceChannelSet(l);
    std::vector<int> a;
    for (int i = 0; i < set.size(); ++i)
    {
        auto type = set.getTypeOfChannel(i);
        if (l.id.startsWith("discrete-") || l.id.startsWith("unknown-"))
            a.push_back(i);
        else
        {
            auto it =
                std::find_if(std::begin(roles), std::end(roles), [&](auto &r) { return r.type == type; });
            if (it == std::end(roles))
                throw std::runtime_error("WAV role adapter unavailable");
            a.push_back(l.roles.indexOf(it->name));
        }
    }
    return a;
}
ChannelMatrix conversionMatrix(const ChannelLayout &a, const ChannelLayout &b)
{
    validateLayout(a);
    validateLayout(b);
    ChannelMatrix m{};
    if (a == b || (a.roles == b.roles && (a.id.startsWith("unknown-") || a.id.startsWith("discrete-")) &&
                   b.id.startsWith("discrete-")))
    {
        for (int i = 0; i < a.size(); ++i)
            m[i][i] = 1;
        return m;
    }
    if (a.id.startsWith("discrete-") || a.id.startsWith("unknown-") || b.id.startsWith("discrete-") ||
        b.id.startsWith("unknown-"))
        throw std::runtime_error("Discrete/unknown conversion needs an explicit channel matrix");
    if (b.id == "mono-v1" || b.id == "stereo-v1")
    {
        if (a.size() == 1)
        {
            if (b.size() == 1)
                m[0][0] = 1;
            else
                m[0][0] = m[1][0] = .70710678f;
            return m;
        }
        for (int i = 0; i < a.size(); ++i)
        {
            auto r = a.roles[i];
            if (r == "LFE")
                continue;
            float l = 0, v = 0;
            if (r == "L")
                l = 1;
            else if (r == "R")
                v = 1;
            else if (r == "C")
                l = v = .70710678f;
            else if (r.startsWith("L"))
                l = .70710678f;
            else if (r.startsWith("R"))
                v = .70710678f;
            float head = a.size() > 2 ? .5f : 1.f;
            if (b.size() == 1)
                m[0][i] = (l + v) * .5f * head;
            else
            {
                m[0][i] = l * head;
                m[1][i] = v * head;
            }
        }
        return m;
    }
    for (int i = 0; i < a.size(); ++i)
    {
        int j = b.roles.indexOf(a.roles[i]);
        if (j < 0)
            throw std::runtime_error(
                "Conversion would drop a channel; choose stereo/mono or explicit routing");
        m[j][i] = 1;
    }
    return m;
}
bool horizontalLayout(const ChannelLayout &l)
{
    return l.size() >= 4 && !l.id.startsWith("unknown-") && !l.id.startsWith("discrete-");
}
std::array<float, 16> spatialGains(const ChannelLayout &l, double degrees, double divergence)
{
    if (!horizontalLayout(l))
        throw std::runtime_error("Spatial Panner requires a horizontal speaker layout");
    std::array<std::pair<double, int>, 16> ring{};
    size_t size = 0;
    for (int i = 0; i < l.size(); ++i)
        if (l.roles[i] != "LFE")
            ring[size++] = {role(l.roles[i])->angle, i};
    std::sort(ring.begin(), ring.begin() + (ptrdiff_t)size);
    double a = std::fmod(degrees + 540., 360.) - 180.;
    std::array<float, 16> g{};
    for (size_t i = 0; i < size; ++i)
    {
        auto lo = ring[i], hi = ring[(i + 1) % size];
        if (i + 1 == size)
            hi.first += 360;
        auto p = a;
        if (p < lo.first)
            p += 360;
        if (p >= lo.first && p <= hi.first)
        {
            double t = (p - lo.first) / (hi.first - lo.first);
            g[lo.second] = (float)std::cos(t * juce::MathConstants<double>::halfPi);
            g[hi.second] = (float)std::sin(t * juce::MathConstants<double>::halfPi);
            break;
        }
    }
    double norm = 0;
    for (size_t i = 0; i < size; ++i)
    {
        auto &v = g[ring[i].second];
        v = (float)((1 - divergence) * v + divergence / std::sqrt((double)size));
        norm += v * v;
    }
    if (norm > 0)
        for (auto &v : g)
            v /= (float)std::sqrt(norm);
    return g;
}
juce::var routingJson(const LayerRouting &r)
{
    auto o = obj();
    auto *p = o.getDynamicObject();
    p->setProperty("interpretation", layoutJson(r.interpretation));
    juce::Array<juce::var> ins, rs;
    for (auto i : r.inputs)
        ins.add(i);
    for (auto &v : r.routes)
    {
        auto n = obj();
        auto *q = n.getDynamicObject();
        q->setProperty("in", v.input);
        q->setProperty("out", v.output);
        q->setProperty("db", v.gainDb);
        q->setProperty("invert", v.invert);
        q->setProperty("mute", v.mute);
        rs.add(n);
    }
    p->setProperty("inputs", ins);
    p->setProperty("matrix", rs);
    p->setProperty("spatial", r.spatial);
    p->setProperty("needsSelection", r.needsSelection);
    p->setProperty("azimuth", r.azimuth);
    p->setProperty("width", r.width);
    p->setProperty("divergence", r.divergence);
    p->setProperty("lfeDb", r.lfeDb);
    p->setProperty("lfeCutoff", r.lfeCutoff);
    return o;
}
LayerRouting routingFromJson(const juce::var &v)
{
    LayerRouting r;
    r.interpretation = layoutFromJson(v["interpretation"]);
    if (auto *a = v["inputs"].getArray())
    {
        if (a->size() > 16)
            throw std::runtime_error("Input selection exceeds16");
        for (auto &i : *a)
            r.inputs.push_back((int)i);
    }
    if (auto *a = v["matrix"].getArray())
    {
        if (a->size() > 256)
            throw std::runtime_error("Matrix exceeds256 routes");
        for (auto &q : *a)
            r.routes.push_back(
                {(int)q["in"], (int)q["out"], (double)q["db"], (bool)q["invert"], (bool)q["mute"]});
    }
    r.spatial = (bool)v["spatial"];
    r.needsSelection = (bool)v["needsSelection"];
    r.azimuth = (double)v["azimuth"];
    r.width = (double)v["width"];
    r.divergence = (double)v["divergence"];
    r.lfeDb = (double)v["lfeDb"];
    r.lfeCutoff = (double)v["lfeCutoff"];
    return r;
}
void validateRouting(const LayerRouting &r, int count, const ChannelLayout &bus)
{
    validateLayout(r.interpretation);
    if (r.needsSelection)
    {
        if (count <= 16 || !r.inputs.empty() || !r.routes.empty() || r.spatial)
            throw std::runtime_error("Invalid unresolved input selection");
        return;
    }
    if (r.inputs.size() != (size_t)r.interpretation.size())
        throw std::runtime_error("Select explicit source channels before playback");
    std::set<int> used;
    for (int i : r.inputs)
        if (i < 0 || i >= count || !used.insert(i).second)
            throw std::runtime_error("Invalid/duplicate source selection");
    std::set<std::pair<int, int>> routes;
    for (auto &v : r.routes)
    {
        if (v.input < 0 || v.input >= r.interpretation.size() || v.output < 0 || v.output >= bus.size() ||
            !routes.insert({v.input, v.output}).second)
            throw std::runtime_error("Invalid/duplicate route");
        finite(v.gainDb, -90, 24);
    }
    finite(r.azimuth, -180, 180);
    finite(r.width, 0, 360);
    finite(r.divergence, 0, 1);
    finite(r.lfeDb, -90, 0);
    finite(r.lfeCutoff, 20, 200);
    if (r.spatial && (!horizontalLayout(bus) || r.inputs.size() > 2))
        throw std::runtime_error("Spatial Panner supports mono/stereo points; beds use channel routing");
}
ChannelMatrix routingMatrix(const LayerRouting &r, const ChannelLayout &bus, double pan)
{
    ChannelMatrix m{};
    if (r.spatial)
        return m;
    if (!r.routes.empty())
    {
        for (auto &v : r.routes)
            if (!v.mute)
                m[v.output][v.input] =
                    (float)(juce::Decibels::decibelsToGain(v.gainDb) * (v.invert ? -1 : 1));
        return m;
    }
    if (r.interpretation.size() == 1 && bus.id == "stereo-v1")
    {
        m[0][0] = (float)std::cos((pan + 1) * juce::MathConstants<double>::pi / 4);
        m[1][0] = (float)std::sin((pan + 1) * juce::MathConstants<double>::pi / 4);
        return m;
    }
    if (r.interpretation.size() == 1 && horizontalLayout(bus) && !bus.roles.contains("C"))
    {
        auto g = spatialGains(bus, pan * 30);
        for (int c = 0; c < bus.size(); ++c)
            m[c][0] = g[c];
        return m;
    }
    auto result = conversionMatrix(r.interpretation, bus);
    if (r.interpretation.id == "stereo-v1" && bus.id == "stereo-v1")
    {
        result[0][0] *= (float)(pan > 0 ? std::cos(pan * juce::MathConstants<double>::halfPi) : 1);
        result[1][1] *= (float)(pan < 0 ? std::cos(pan * juce::MathConstants<double>::halfPi) : 1);
    }
    return result;
}
} // namespace sp
