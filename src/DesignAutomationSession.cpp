#include "DesignSession.h"
#include "AutomationWriter.h"
#include "AudioIPC.h"

namespace sp
{
namespace
{
juce::String curveRevision(const DesignMix &m, const juce::String &owner, const AutomationTarget &target)
{
    AutomationLane isolated;
    if (auto *l = automationLane(m, owner))
        if (auto *c = automationCurve(*l, target))
            isolated.curves.push_back(*c);
    return juce::JSON::toString(automationLaneJson(isolated), true);
}
} // namespace
void DesignSession::sendAutomationWrite(const char *phase, double value)
{
    if (!writerPass || !services.squareTouchCommand)
        return;
    auto &p = *writerPass;
    auto cmd = sp::message("square_touch");
    auto *o = cmd.getDynamicObject();
    o->setProperty("scalar", true);
    o->setProperty("phase", phase);
    o->setProperty("id", "design:" + p.document);
    o->setProperty("owner", p.owner);
    o->setProperty("instance", p.target.instance);
    o->setProperty("parameter", p.target.parameter);
    o->setProperty("pass", p.id);
    o->setProperty("playGeneration", (juce::int64)p.generation);
    o->setProperty("graphEpoch", (juce::int64)p.epoch);
    o->setProperty("value", value);
    o->setProperty("first", (juce::int64)p.first);
    o->setProperty("last", (juce::int64)p.last);
    services.squareTouchCommand(std::move(cmd));
}
bool DesignSession::beginAutomationWrite(const juce::String &owner, const AutomationTarget &target,
                                         double value, bool merge, bool preview)
{
    const auto start = automationFrame();
    if (writerPass || replacing || takeBusy() || midiBusy() || inputBusy() ||
        document.activeTake.isNotEmpty() || !services.squareTouchCommand || !start)
        return false;
    const auto &mix = document.activeMix();
    auto *lane = automationLane(mix, owner);
    if (!lane || !lane->touch || lane->armed != target || target.instance == "square-surround-v1")
        return false;
    auto opts = automationTargets(mix, owner);
    auto at = std::find_if(opts.begin(), opts.end(),
                           [&](auto &p) { return p.target == target && p.writable && !p.hostedBinding; });
    if (at == opts.end() || !std::isfinite(value) || value < at->minimum || value > at->maximum)
        return false;
    auto state = services.status();
    WriterPass pass;
    pass.id = juce::Uuid().toString();
    pass.owner = owner;
    pass.target = target;
    pass.document = document.id;
    pass.variation = document.activeVariation;
    pass.lifetime = lifetime;
    pass.generation = expectedGeneration;
    pass.epoch = (int64_t)state["graphEpoch"];
    pass.helper = (int64_t)state["helperPid"];
    pass.first = document.renderSettings.first;
    pass.last = document.renderSettings.last < 0 ? mixDuration(mix, document.sampleRate)
                                                 : document.renderSettings.last;
    if (pass.last <= pass.first || *start >= pass.last)
        return false;
    pass.merge = merge;
    pass.preview = preview;
    pass.base = curveRevision(mix, owner, target);
    writerPass = std::move(pass);
    sendAutomationWrite("begin", value);
    return true;
}
bool DesignSession::ownsAutomationWrite(const juce::String &owner, const AutomationTarget &target) const
{
    return writerPass && writerPass->owner == owner && writerPass->target == target && !writerPass->ending;
}
void DesignSession::moveAutomationWrite(const juce::String &owner, const AutomationTarget &target,
                                        double value)
{
    if (ownsAutomationWrite(owner, target))
        sendAutomationWrite("move", value);
}
void DesignSession::endAutomationWrite(bool punch)
{
    if (!writerPass || writerPass->ending)
        return;
    auto &p = *writerPass;
    p.ending = true;
    p.commit = !p.preview || punch;
    p.deadline = services.now() + 2500;
    sendAutomationWrite(p.commit ? "end" : "discard");
}
void DesignSession::pollAutomationWrite(const juce::var &state)
{
    if (!writerPass)
        return;
    auto &p = *writerPass;
    auto receipt = state["automationWrite"];
    bool match = receipt["pass"].toString() == p.id && receipt["owner"].toString() == p.owner &&
                 (uint64_t)(int64_t)receipt["playGeneration"] == p.generation &&
                 (int64_t)receipt["graphEpoch"] == p.epoch;
    bool valid = p.lifetime == lifetime && p.document == document.id &&
                 p.variation == document.activeVariation && document.activeTake.isEmpty() && !replacing;
    if (!match)
    {
        bool lost = !valid || !(bool)state["playing"] || (int64_t)state["helperPid"] != p.helper ||
                    (int64_t)state["graphEpoch"] != p.epoch ||
                    (uint64_t)(int64_t)state["playGeneration"] != p.generation;
        if (lost)
            endAutomationWrite(false);
        if (!p.ending || services.now() < p.deadline)
            return;
    }
    else
        sendAutomationWrite("ack");
    auto saved = std::move(*writerPass);
    writerPass.reset();
    if (match && valid && (!saved.preview || saved.commit) && !(bool)receipt["discard"] &&
        (int)receipt["pointCount"] > 0)
    {
        if (saved.base == curveRevision(document.activeMix(), saved.owner, saved.target))
            edit("Punch automation", [&](auto &d)
                 { retainAutomationWrite(d.activeMix(), saved.owner, saved.target, receipt, saved.merge); });
        else
            message = "Target changed during the pass. Recorded values were not applied.";
    }
    else if (!match)
        message = "Automation pass ended without an audio-engine receipt";
    if (auto next = std::exchange(afterWriter, {}))
        services.post(std::move(next));
}
} // namespace sp
