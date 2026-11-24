#include "AutomationWriter.h"
namespace sp
{
juce::var AutomationWriter::result()
{
    if (release < 0)
        return {};
    if (receipt.isObject())
        return receipt;
    receipt = juce::var(new juce::DynamicObject());
    auto *o = receipt.getDynamicObject();
    o->setProperty("pass", id);
    o->setProperty("owner", owner);
    o->setProperty("instance", target.instance);
    o->setProperty("parameter", target.parameter);
    o->setProperty("playGeneration", (juce::int64)generation);
    o->setProperty("graphEpoch", (juce::int64)epoch);
    o->setProperty("end", (juce::int64)release);
    o->setProperty("discard", discard);
    juce::MemoryOutputStream data;
    for (auto &e : events)
    {
        data.writeInt64(e.frame);
        data.writeDouble(e.value);
    }
    o->setProperty("pointCount", (int)events.size());
    o->setProperty("packedPoints", juce::Base64::toBase64(data.getData(), data.getDataSize()));
    return receipt;
}
void retainAutomationWrite(DesignMix &mix, const juce::String &owner, const AutomationTarget &target,
                           const juce::var &receipt, bool merge)
{
    if ((bool)receipt["discard"])
        return;
    const int count = receipt["pointCount"];
    if (count == 0)
        return;
    const int64_t end = receipt["end"];
    auto *lane = automationLane(mix, owner);
    auto list = automationTargets(mix, owner);
    auto def =
        std::find_if(list.begin(), list.end(), [&](auto &p) { return p.target == target && p.writable; });
    if (!lane || def == list.end() || receipt["owner"].toString() != owner ||
        receipt["instance"].toString() != target.instance ||
        receipt["parameter"].toString() != target.parameter || count < 1 ||
        count > maxAutomationCurvePoints - 4)
        throw std::runtime_error("Automation pass target or point count changed");
    juce::MemoryOutputStream bytes;
    if (receipt["packedPoints"].toString().length() > 44000 ||
        !juce::Base64::convertFromBase64(bytes, receipt["packedPoints"].toString()) ||
        bytes.getDataSize() != size_t(count) * 16)
        throw std::runtime_error("Incomplete automation receipt");
    juce::MemoryInputStream data(bytes.getData(), bytes.getDataSize(), false);
    const auto *prior = automationCurve(*lane, target);
    AutomationCurve initial = prior ? *prior : AutomationCurve{target, true, {}};
    auto next = initial;
    next.read = true;
    auto insert = [&](int64_t t, double v, bool hold)
    {
        auto pos = std::lower_bound(next.points.begin(), next.points.end(), t,
                                    [](auto &p, int64_t n) { return p.frame < n; });
        if (pos != next.points.end() && pos->frame == t)
        {
            pos->value = v;
            pos->hold = hold;
            pos->smooth = false;
            pos->smoothStart = 0;
            pos->smoothEnd = 1;
            return;
        }
        next.points.insert(pos, {juce::Uuid().toString(), t, v, hold});
    };
    int64_t prev = -1;
    for (int j = 0; j < count; ++j)
    {
        int64_t t = data.readInt64();
        double v = data.readDouble();
        if (t < 0 || t <= prev || t >= end || !std::isfinite(v) || v < def->minimum || v > def->maximum)
            throw std::runtime_error("Invalid applied automation event");
        if (j == 0)
        {
            if (t > 0)
            {
                auto &guard = splitSquareSegment(next, t - 1, def->base);
                guard.value = automationValue(initial, *def, t - 1);
            }
            auto &after = splitSquareSegment(next, end, def->base);
            after.value = automationValue(initial, *def, end);
            if (!merge)
                std::erase_if(next.points, [&](auto &p) { return p.frame >= t && p.frame < end; });
            if (t > 0)
                insert(t - 1, automationValue(initial, *def, t - 1), true);
        }
        insert(t, v, true);
        prev = t;
    }
    if (next.points.size() > maxAutomationCurvePoints)
        throw std::runtime_error("Automation curve point limit reached");
    *automationCurve(*lane, target, true) = std::move(next);
}
} // namespace sp
