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
juce::var DesignSession::automationWriteCommand(const char *phase, double value) const
{
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
    if (p.loop)
        o->setProperty("loopPunch", true);
    return cmd;
}
void DesignSession::sendAutomationWrite(const char *phase, double value)
{
    if (writerPass && services.squareTouchCommand)
        services.squareTouchCommand(automationWriteCommand(phase, value));
}
bool DesignSession::beginAutomationWrite(const juce::String &owner, const AutomationTarget &target,
                                         double value, bool merge, bool preview)
{
    const auto start = automationFrame();
    if (!start || !reserveAutomationWrite(owner, target, value, merge, preview, *start))
        return false;
    sendAutomationWrite("begin", value);
    return true;
}
bool DesignSession::reserveAutomationWrite(const juce::String &owner, const AutomationTarget &target,
                                           double value, bool merge, bool preview, int64_t start)
{
    if (writerPass || replacing || takeBusy() || midiBusy() || inputBusy() ||
        document.activeTake.isNotEmpty() || !services.squareTouchCommand)
        return false;
    const auto &mix = document.activeMix();
    auto *lane = automationLane(mix, owner);
    if (!lane || !lane->touch || lane->armed != target || target.instance == "square-surround-v1")
        return false;
    auto opts = automationTargets(mix, owner);
    auto at =
        std::find_if(opts.begin(), opts.end(), [&](auto &p) { return p.target == target && p.writable; });
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
    if (pass.last <= pass.first || start >= pass.last)
        return false;
    pass.merge = merge;
    pass.preview = preview;
    pass.base = curveRevision(mix, owner, target);
    writerPass = std::move(pass);
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
    auto receipt = p.completedPreview.isObject() ? p.completedPreview : state["automationWrite"];
    if (p.epoch < 0)
    {
        if (receipt["pass"].toString() == p.id &&
            (uint64_t)(int64_t)receipt["playGeneration"] == p.generation)
            p.epoch = (int64_t)receipt["graphEpoch"];
        else if ((uint64_t)(int64_t)state["playGeneration"] == p.generation)
        {
            p.epoch = (int64_t)state["graphEpoch"];
            p.helper = (int64_t)state["helperPid"];
        }
        else if (!p.ending && services.now() < p.deadline && state["error"].toString().isEmpty())
            return;
    }
    bool match = receipt["pass"].toString() == p.id && receipt["owner"].toString() == p.owner &&
                 (uint64_t)(int64_t)receipt["playGeneration"] == p.generation &&
                 (int64_t)receipt["graphEpoch"] == p.epoch;
    bool valid = p.lifetime == lifetime && p.document == document.id &&
                 p.variation == document.activeVariation && document.activeTake.isEmpty() && !replacing;
    if (match && valid && p.preview && !p.ending && active && (bool)state["playing"])
    {
        if (!p.completedPreview.isObject())
        {
            p.completedPreview = receipt.clone();
            sendAutomationWrite("ack");
        }
        return;
    }
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
    if (match && valid && saved.loop && !saved.ending && active && !previewWaiting &&
        (int)receipt["pointCount"] > 0 && (int64_t)receipt["end"] == saved.last &&
        (uint64_t)(int64_t)state["playGeneration"] == saved.generation &&
        laneWriteSettings(saved.owner).loopPunch && laneWriteSettings(saved.owner).armed)
    {
        document.playhead = document.renderSettings.first;
        schedulePreview();
    }
    if (auto next = std::exchange(afterWriter, {}))
        services.post(std::move(next));
}
} // namespace sp

namespace sp
{
DesignSession::LaneWriteSettings &DesignSession::laneWriteSettings(const juce::String &owner)
{
    if (laneModesLifetime != lifetime || laneModesDocument != document.id ||
        laneModesVariation != document.activeVariation)
    {
        laneModes.clear();
        laneModesLifetime = lifetime;
        laneModesDocument = document.id;
        laneModesVariation = document.activeVariation;
    }
    return laneModes[owner];
}
bool DesignSession::armLaneWrite(const juce::String &owner, bool arm)
{
    auto &options = laneWriteSettings(owner);
    if (arm && options.mode != WriteMode::touch)
        for (const auto &[id, other] : laneModes)
            if (id != owner && other.armed && other.mode != WriteMode::touch)
            {
                message = "Disarm the other automation writer first";
                notify();
                return false;
            }
    options.armed = arm;
    options.started = 0;
    return true;
}
juce::var DesignSession::prepareArmedWrite(int64_t position)
{
    if (!services.playArmed || laneModesLifetime != lifetime || laneModesDocument != document.id ||
        laneModesVariation != document.activeVariation)
        return {};
    for (auto &[owner, options] : laneModes)
    {
        if (!options.armed || (options.mode != WriteMode::replace && options.mode != WriteMode::merge))
            continue;
        auto &mix = document.activeMix();
        auto *lane = automationLane(mix, owner);
        if (!lane || !lane->touch)
            continue;
        auto registry = automationTargets(mix, owner);
        auto p = std::find_if(registry.begin(), registry.end(),
                              [&](auto &d) { return d.target == lane->armed && d.writable; });
        if (p == registry.end())
            continue;
        auto *curve = automationCurve(*lane, lane->armed);
        auto first = std::max(position, document.renderSettings.first);
        auto value = curve && curve->read ? automationValue(*curve, *p, first) : p->base;
        if (!reserveAutomationWrite(owner, lane->armed, value, options.mode == WriteMode::merge, false,
                                    position))
            continue;
        writerPass->first = first;
        writerPass->loop = options.loopPunch && document.renderSettings.last > first;
        writerPass->epoch = -1;
        writerPass->deadline = services.now() + 120000;
        return automationWriteCommand("begin", value);
    }
    return {};
}
void DesignSession::pollArmedWrites(const juce::var &state)
{
    if (writerPass || !active || previewWaiting || !(bool)state["playing"] ||
        document.activeTake.isNotEmpty())
        return;
    if (laneModesLifetime != lifetime || laneModesDocument != document.id ||
        laneModesVariation != document.activeVariation)
        return;
    for (auto &[owner, options] : laneModes)
    {
        if (!options.armed || options.started == expectedGeneration ||
            (options.mode != WriteMode::replace && options.mode != WriteMode::merge))
            continue;
        auto *l = automationLane(document.activeMix(), owner);
        if (!l || !l->touch)
            continue;
        auto all = automationTargets(document.activeMix(), owner);
        auto p =
            std::find_if(all.begin(), all.end(), [&](auto &d) { return d.target == l->armed && d.writable; });
        if (p == all.end())
            continue;
        auto *c = automationCurve(*l, l->armed);
        auto v = c && c->read ? automationValue(*c, *p, (int64_t)state["sourceSample"]) : p->base;
        if (beginAutomationWrite(owner, l->armed, v, options.mode == WriteMode::merge, false))
        {
            options.started = expectedGeneration;
            return;
        }
    }
}
void DesignSession::learnAutomationMidi(const juce::String &owner, const AutomationTarget &target,
                                        AutomationMidiBinding settings)
{
    if (settings.device.isEmpty() || settings.low < 0 || settings.high > 1 ||
        !(settings.low < settings.high) || settings.hysteresis < 1 || settings.hysteresis > 16 ||
        document.activeTake.isNotEmpty())
        return;
    auto registry = automationTargets(document.activeMix(), owner);
    if (std::none_of(registry.begin(), registry.end(),
                     [&](const auto &p)
                     {
                         return p.target == target && p.writable && !p.hostedBinding &&
                                p.target.instance != "square-surround-v1";
                     }))
        return;
    learning =
        LearnTarget{owner, document.id, document.activeVariation, target, std::move(settings), lifetime};
    controlRevision = -1;
    nextControlScan = 0;
    message = "Move a controller";
    notify();
}
void DesignSession::cancelMidiLearn()
{
    learning.reset();
    controlRevision = -1;
    nextControlScan = 0;
}
void DesignSession::pollAutomationMidi(const juce::var &state)
{
    const auto context = document.id + "/" + document.activeVariation + "/" + document.activeTake;
    if (learning && (learning->lifetime != lifetime || learning->document != document.id ||
                     learning->variation != document.activeVariation || document.activeTake.isNotEmpty()))
        cancelMidiLearn();
    if (controlContext != context || controlRevision != document.revision)
    {
        controlContext = context;
        controlRevision = document.revision;
        controlRoutes.clear();
        nextControlScan = 0;
        auto collect = [&](const juce::String &owner, const AutomationLane &lane)
        {
            if (!lane.touch || document.activeTake.isNotEmpty())
                return;
            auto *c = automationCurve(lane, lane.armed);
            if (!c || !c->midi)
                return;
            auto registry = automationTargets(document.activeMix(), owner);
            auto p = std::find_if(registry.begin(), registry.end(), [&](auto &v)
                                  { return v.target == c->target && v.writable && !v.hostedBinding; });
            if (p != registry.end())
                controlRoutes.push_back(
                    {{owner, document.id, document.activeVariation, c->target, *c->midi, lifetime}, *p, {}});
        };
        auto &m = document.activeMix();
        collect("master", m.masterAutomation);
        for (auto &t : m.tracks)
            collect(t.id, t.automation);
    }
    if (controlRoutes.empty() && !learning)
    {
        controlInputs.clear();
        return;
    }
    if (controlGeneration != (uint64_t)(int64_t)state["playGeneration"])
    {
        controlGeneration = (uint64_t)(int64_t)state["playGeneration"];
        for (auto &r : controlRoutes)
            r.pickup = {};
    }
    const auto now = services.now();
    if (now >= nextControlScan)
    {
        nextControlScan = now + 1500;
        std::set<juce::String> wanted;
        for (auto &r : controlRoutes)
            wanted.insert(r.destination.binding.device);
        if (learning)
            wanted.insert(learning->binding.device);
        auto devices = juce::MidiInput::getAvailableDevices();
        std::erase_if(controlInputs,
                      [&](auto &entry)
                      {
                          bool gone = !wanted.contains(entry.first) ||
                                      std::none_of(devices.begin(), devices.end(),
                                                   [&](auto &d) { return d.identifier == entry.first; });
                          if (gone)
                              for (auto &r : controlRoutes)
                                  if (r.destination.binding.device == entry.first)
                                  {
                                      r.pickup = {};
                                      if (writerPass && writerPass->id == midiWriterId &&
                                          ownsAutomationWrite(r.destination.owner, r.destination.target))
                                          endAutomationWrite();
                                  }
                          return gone;
                      });
        for (auto &dev : devices)
            if (wanted.contains(dev.identifier) && !controlInputs.contains(dev.identifier))
            {
                auto port = std::make_unique<AutomationMidiInput>();
                if (port->open(dev.identifier))
                    controlInputs.emplace(dev.identifier, std::move(port));
            }
    }
    for (auto &[uid, port] : controlInputs)
    {
        if (port->overflowed())
        {
            port->drain([](auto) {});
            if (writerPass && writerPass->id == midiWriterId)
                endAutomationWrite();
            for (auto &r : controlRoutes)
                r.pickup = {};
            message = "Controller queue full. Move the controller again.";
            continue;
        }
        port->drain(
            [&](auto e)
            {
                if (learning && learning->binding.device == uid)
                {
                    auto dst = *learning;
                    cancelMidiLearn();
                    dst.binding.channel = e.channel;
                    dst.binding.cc = e.number;
                    edit(
                        "Learn MIDI controller",
                        [&](auto &d)
                        {
                            if (auto *l = automationLane(d.activeMix(), dst.owner))
                                automationCurve(*l, dst.target, true)->midi = dst.binding;
                        },
                        ChangeImpact::metadata);
                    message = "Learned CC " + juce::String(e.number);
                    return;
                }
                if (!active || previewWaiting || !(bool)state["playing"] || takeBusy() || inputBusy() ||
                    midiBusy())
                    return;
                MidiControlRoute *match = nullptr;
                for (auto &r : controlRoutes)
                    if (r.destination.binding.device == uid && r.destination.binding.channel == e.channel &&
                        r.destination.binding.cc == e.number)
                    {
                        if (match)
                        {
                            message = "Controller assigned to multiple armed targets";
                            return;
                        }
                        match = &r;
                    }
                if (!match)
                    return;
                auto &dst = match->destination;
                auto *lane = automationLane(document.activeMix(), dst.owner);
                if (!lane || !lane->touch || lane->armed != dst.target)
                    return;
                auto *c = automationCurve(*lane, dst.target);
                double base = c && c->read
                                  ? automationValue(*c, match->parameter, (int64_t)state["sourceSample"])
                                  : match->parameter.base;
                auto value = match->pickup.move(dst.binding, match->parameter, e.value, base);
                if (!value)
                    return;
                if (!writerPass)
                {
                    if (beginAutomationWrite(dst.owner, dst.target, *value, false, false))
                        midiWriterId = writerPass->id;
                }
                else if (writerPass->id == midiWriterId)
                    moveAutomationWrite(dst.owner, dst.target, *value);
            });
    }
}
} // namespace sp
