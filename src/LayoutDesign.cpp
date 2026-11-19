#include "DesignModel.h"
namespace sp
{
juce::StringArray outputFiles(const DesignOutput &o)
{
    juce::StringArray result;
    if (!juce::File(o.path).existsAsFile())
        return result;
    if (auto *a = o.files.getArray())
    {
        if (a->isEmpty() || a->size() > 16)
            return {};
        for (auto &v : *a)
        {
            auto p = v["path"].toString();
            if (!juce::File(p).existsAsFile() || v["sha256"].toString().length() != 64)
                return {};
            result.add(p);
        }
    }
    else
        result.add(o.path);
    return result;
}

bool simplePanAvailable(const DesignMix &m, const DesignLayer &l)
{
    if (m.layout.id.isEmpty())
        return true;
    if (l.routing.spatial || !l.routing.routes.empty())
        return false;
    return (l.routing.interpretation.size() == 1 &&
            (m.layout.id == "stereo-v1" || (horizontalLayout(m.layout) && !m.layout.roles.contains("C")))) ||
           (l.routing.interpretation.id == "stereo-v1" && m.layout.id == "stereo-v1");
}
void initialiseRouting(DesignLayer &l, const ChannelLayout &bus, bool preserveLegacySelection)
{
    if (l.source.detectedChannels.id.isEmpty())
        l.source.detectedChannels = l.source.channels <= 2
                                        ? channelLayout(l.source.channels == 1 ? "mono-v1" : "stereo-v1")
                                        : discreteLayout(l.source.channels, true);
    l.routing = {};
    if (preserveLegacySelection)
    {
        if (l.region.auditionPair < 0)
        {
            l.routing.legacyAnalysisMean = true;
            for (int i = 0; i < l.source.channels; ++i)
                l.routing.inputs.push_back(i);
            l.routing.interpretation = channelLayout("mono-v1");
        }
        else
        {
            int first = l.region.auditionPair * 2;
            if (first < 0 || first >= l.source.channels)
                throw std::runtime_error(
                    "Legacy pair is unavailable; choose source channels before conversion");
            l.routing.inputs = {first};
            if (first + 1 < l.source.channels)
                l.routing.inputs.push_back(first + 1);
            l.routing.interpretation = channelLayout(l.routing.inputs.size() == 1 ? "mono-v1" : "stereo-v1");
        }
        return;
    }
    if (l.source.channels > 16)
    {
        l.routing.needsSelection = true;
        l.routing.interpretation = discreteLayout(16);
        return;
    }
    l.routing.interpretation =
        l.source.layoutOverride.id.isNotEmpty() ? l.source.layoutOverride : l.source.detectedChannels;
    l.routing.inputs.clear();
    l.routing.routes.clear();
    for (int i = 0; i < l.source.channels; ++i)
        l.routing.inputs.push_back(i);
    // Unknown beds are never guessed as surround. They remain explicit discrete inputs.
    juce::ignoreUnused(bus);
}
void setMixLayout(DesignMix &m, const ChannelLayout &l)
{
    validateLayout(l);
    auto candidate = m;
    bool legacy = candidate.layout.id.isEmpty();
    candidate.layout = l;
    candidate.routingVersion = 2;
    for (auto &v : candidate.layers)
    {
        if (legacy || v.routing.inputs.empty())
            initialiseRouting(v, l, legacy);
        else
        {
            v.routing.routes.clear();
            v.routing.spatial = false;
        }
        validateRouting(v.routing, v.source.channels, l);
        if (legacy && !v.routing.needsSelection)
            (void)routingMatrix(v.routing, l, v.pan);
    }
    validateLayoutMix(candidate, 48000);
    m = std::move(candidate); // Commit only after every layer can be represented.
}

bool upgradeLiveSpatial(DesignDocument &d)
{
    bool changed = false;
    auto upgrade = [&](DesignMix &m)
    {
        if (!m.layout.id.isEmpty() && m.routingVersion == 1)
        {
            m.routingVersion = 2;
            changed = true;
        }
    };
    upgrade(d.original);
    for (auto &v : d.variations)
        upgrade(v.mix);
    return changed;
}
ChannelLayout designLayout(const DesignDocument &d)
{
    auto *t = selectedTake(d);
    auto &m = t ? (const DesignMix &)t->recipe : d.activeMix();
    return m.layout.id.isEmpty() ? channelLayout("stereo-v1") : m.layout;
}
int designChannels(const DesignDocument &d)
{
    return designLayout(d).size();
}
ChannelLayout exportLayout(const DesignDocument &d, const DesignRenderSettings &s)
{
    return (s.layout == "same" ||
            (s.layout.isEmpty() &&
             !(selectedTake(d) ? selectedTake(d)->recipe->layout : d.activeMix().layout).id.isEmpty()))
               ? designLayout(d)
           : s.layout.isEmpty() ? channelLayout(s.channels == 1 ? "mono-v1" : "stereo-v1")
                                : channelLayout(s.layout);
}
void validateLayoutMix(const DesignMix &m, double rate)
{
    if (m.layout.id.isEmpty())
        return;
    validateLayout(m.layout);
    if (m.routingVersion != 1 && m.routingVersion != 2)
        throw std::runtime_error("Unsupported spatial routing version");
    int work = 0, spatial = 0, matrixWork = 0;
    double delayBytes = 0, preparedBytes = 0;
    for (auto &l : m.layers)
    {
        validateRouting(l.routing, l.source.channels, m.layout);
        validateLayout(l.source.detectedChannels, 64);
        if (!l.source.layoutOverride.id.isEmpty())
        {
            validateLayout(l.source.layoutOverride, 64);
            if (l.source.layoutOverride.size() != l.source.channels)
                throw std::runtime_error("Source override/count mismatch");
        }
        if (l.source.detectedChannels.size() != l.source.channels)
            throw std::runtime_error("Detected source layout/count mismatch");
        if (l.routing.spatial)
            ++spatial;
        if (l.routing.legacyAnalysisMean && m.routingVersion != 2)
            throw std::runtime_error("Legacy analysis conversion requires routing v2");
        int n = l.routing.interpretation.size();
        matrixWork += n * m.layout.size();
        preparedBytes += (l.source.channels + 2 * n) * 8192. * 4 + 131072 + n * 512 * 4;
        for (auto &fx : l.effects)
            if (fx.type == "reverb")
                preparedBytes += n * 262144.;
        work += n * (1 + (int)l.effects.size());
        for (auto &fx : l.effects)
            if (fx.type == "delay")
                delayBytes += n * std::ceil(rate * 2) * 4;
    }
    work += m.layout.size() * (int)m.masterEffects.size();
    for (auto &fx : m.masterEffects)
        if (fx.type == "delay")
            delayBytes += m.layout.size() * std::ceil(rate * 2) * 4;
    preparedBytes += delayBytes + 8 * 1024 * 1024;
    for (auto &fx : m.masterEffects)
        if (fx.type == "reverb")
            preparedBytes += m.layout.size() * 262144.;
    if (matrixWork > 2048 || work > 512 || delayBytes > 128 * 1024 * 1024 ||
        preparedBytes > 256 * 1024 * 1024 || spatial > 16)
        throw std::runtime_error(
            "Layout DSP admission exceeds2048 routing cells,512 channel-inserts,128MiB delay history,256MiB "
            "prepared-buffer estimate or16 spatial targets; reduce the design");
}
PerformControls mixControls(const DesignMix &m)
{
    auto c = performControls(m.performance);
    if (!m.layout.id.isEmpty())
        for (size_t i = 0; i < m.layers.size(); ++i)
            if (m.layers[i].routing.spatial)
            {
                if (c.spatialCount >= 16)
                    throw std::runtime_error("16 spatial targets maximum");
                auto &r = m.layers[i].routing;
                c.spatial[(size_t)c.spatialCount++] = {(int)i, r.azimuth, r.width};
            }
    return c;
}
void applyMixControls(DesignMix &m, const PerformControls &c)
{
    applyPerformControls(m.performance, c);
    int expected = 0;
    for (size_t i = 0; i < m.layers.size(); ++i)
        if (m.layers[i].routing.spatial)
        {
            if (expected >= c.spatialCount || c.spatial[(size_t)expected].layer != (int)i)
                throw std::runtime_error("Spatial event target differs from frozen recipe");
            auto &v = c.spatial[(size_t)expected++];
            m.layers[i].routing.azimuth = v.azimuth;
            m.layers[i].routing.width = v.width;
        }
    if (expected != c.spatialCount)
        throw std::runtime_error("Unexpected spatial event target");
}
} // namespace sp
