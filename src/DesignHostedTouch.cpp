#include "DesignSession.h"
#include "TrackModel.h"
namespace sp
{
bool DesignSession::hostedTouch(const juce::String &slot, const juce::String &parameter, uint64_t incarnation,
                                int kind, float value)
{
    if (kind < 0 || kind > 2 || !std::isfinite(value) || value < 0 || value > 1 || incarnation != lifetime ||
        replacing || takeBusy() || !document.activeTake.isEmpty())
        return false;
    if (touchLifetime != lifetime)
    {
        inlineTouches.clear();
        nativeTouches.clear();
        retainedTouch.clear();
        completedTouches.clear();
        touchLifetime = lifetime;
    }
    auto key = slot + ":" + parameter;
    if (kind == 0)
    {
        if (!automationFrame())
            return false;
        std::vector<juce::String> owners{"master"};
        for (auto &t : document.activeMix().tracks)
            owners.push_back(t.id);
        bool found = false;
        for (auto &owner : owners)
            if (auto *lane = automationLane(document.activeMix(), owner);
                lane && lane->touch && lane->armed == AutomationTarget{slot, parameter})
            {
                auto *curve = automationCurve(*lane, lane->armed);
                if (curve && !curve->read)
                    continue;
                for (auto &p : automationTargets(document.activeMix(), owner))
                    if (p.target == lane->armed && p.writable && p.hostedBinding)
                    {
                        inlineTouches[key] = {owner, document.activeVariation, *p.hostedBinding};
                        found = true;
                        break;
                    }
            }
        if (!found)
            return false;
    }
    auto at = inlineTouches.find(key);
    if (at == inlineTouches.end() || at->second.variation != document.activeVariation)
        return false;
    auto context = at->second;
    bool valid = false;
    for (auto &p : automationTargets(document.activeMix(), context.owner))
        if (p.target == AutomationTarget{slot, parameter} && p.writable && p.hostedBinding &&
            *p.hostedBinding == context.binding)
            valid = true;
    if (!valid)
    {
        inlineTouches.erase(key);
        return false;
    }
    bool accepted = services.hostedGesture
                        ? services.hostedGesture(slot, parameter, context.owner, kind, value)
                        : liveInserts && liveInserts->gesture(slot, parameter, context.owner, kind, value,
                                                              lifetime, document.id);
    if (kind == 2)
        inlineTouches.erase(key);
    if (!accepted && kind != 0)
    {
        message = "Hosted Touch is unavailable or full; last acknowledged points remain in checkpoint";
        notify();
    }
    return accepted;
}
void DesignSession::receiveHostedTouches(const LiveInsertSession::Capture &capture)
{
    if (capture.incarnation != lifetime || capture.document != document.id ||
        capture.variation != document.activeVariation || !document.activeTake.isEmpty())
        return;
    if (touchLifetime != lifetime)
    {
        inlineTouches.clear();
        nativeTouches.clear();
        retainedTouch.clear();
        completedTouches.clear();
        touchLifetime = lifetime;
    }
    auto *passes = capture.evidence["touchPasses"].getArray();
    if (!passes || passes->isEmpty())
        return;
    auto settings = juce::JSON::parse(capture.settings);
    for (auto &pass : *passes)
    {
        auto id = (uint64_t)(int64_t)pass["id"];
        if (!id || id <= retainedTouch[capture.generation] ||
            completedTouches[capture.generation].contains(id))
            continue;
        auto key = capture.generation + ":" + juce::String((juce::int64)id);
        auto owner = pass["owner"].toString();
        int index = (int)pass["index"];
        juce::String native;
        if (auto *parameters = settings["parameters"].getArray())
            for (auto &p : *parameters)
                if ((int)p["index"] == index)
                    native = p["stableId"].toString();
        if (native.isEmpty())
            continue;
        AutomationTarget target{capture.slot, native};
        HostedAutomationBinding captured{owner,
                                         capture.evidence["binding"]["source"].toString(),
                                         capture.evidence["binding"]["instrument"].toString(),
                                         capture.slot,
                                         settings["plugin"]["format"].toString(),
                                         settings["plugin"]["id"].toString(),
                                         settings["plugin"]["fingerprint"].toString(),
                                         native};
        auto registry = automationTargets(document.activeMix(), owner);
        auto parameter =
            std::find_if(registry.begin(), registry.end(), [&](auto &p)
                         { return p.target == target && p.writable && p.hostedBinding.has_value(); });
        const bool resolved = parameter != registry.end() && *parameter->hostedBinding == captured;
        auto found = nativeTouches.find(key);
        if (found == nativeTouches.end())
        {
            if (!automationLane(document.activeMix(), owner))
            {
                if ((bool)pass["complete"])
                {
                    message = "Native Touch ignored after its owning Track was removed";
                    acknowledgeRetainedTouch(capture.slot, capture.generation, id);
                }
                continue;
            }
            if (nativeTouches.size() >= 64)
            {
                message = "Native Touch retention limit reached";
                continue;
            }
            HostedTouchContext c;
            c.document = document.id;
            c.variation = document.activeVariation;
            c.incarnation = lifetime;
            c.generation = capture.generation;
            c.slot = capture.slot;
            c.owner = owner;
            c.id = id;
            c.target = target;
            c.binding = captured;
            found = nativeTouches.emplace(key, std::move(c)).first;
        }
        auto &context = found->second;
        if (context.owner != owner || context.target != target || context.binding != captured)
            continue;
        auto *points = pass["points"].getArray();
        if (!points || points->size() > 2044)
            continue;
        std::vector<AutomationPoint> values;
        int64_t previous = -1;
        bool valid = true;
        for (auto &p : *points)
        {
            auto frame = (int64_t)p["frame"];
            double value = (double)p["value"];
            if (frame < 0 || frame <= previous || frame > document.sampleRate * 86400 * 7 ||
                !std::isfinite(value) || value < 0 || value > 1)
            {
                valid = false;
                break;
            }
            values.push_back({{}, frame, value, true});
            previous = frame;
        }
        if (!valid || values.empty())
            continue;
        context.points = values;
        if (!(bool)pass["complete"])
            continue;
        const auto end = (int64_t)pass["end"];
        double returned = (double)pass["returnValue"];
        if ((bool)pass["interrupted"])
        {
            if (resolved)
            {
                AutomationCurve underlying;
                if (auto *lane = automationLane(document.activeMix(), owner))
                    if (auto *curve = automationCurve(*lane, target))
                        underlying = *curve;
                returned = automationValue(underlying, *parameter,
                                           end + (int64_t)std::llround(document.sampleRate * .1));
            }
            else
                for (auto &p : *settings["parameters"].getArray())
                    if (p["stableId"].toString() == native)
                        returned = (double)p["value"];
        }
        if (end < values.back().frame || !std::isfinite(returned) || returned < 0 || returned > 1)
            continue;
        if (values.back().frame == end)
            values.back().hold = false;
        if (!context.applied)
            for (auto &point : values)
                point.id = juce::Uuid().toString();
        if (!context.applied)
        {
            const juce::ScopedValueSetter<bool> input(inputMutation, true), midi(midiMutation, true),
                take(takeMutation, takeMutation || (takeState == TakeState::arming && takeCapturingPlugins));
            edit("Plugin automation Touch",
                 [&](auto &d)
                 {
                     if (resolved)
                         writeTouchPass(d.activeMix(), context.owner, context.target, values, end,
                                        d.sampleRate, returned);
                     else
                     {
                         auto *lane = automationLane(d.activeMix(), context.owner);
                         if (!lane)
                             return;
                         auto *curve = automationCurve(*lane, context.target, true);
                         if (curve->hostedBinding && *curve->hostedBinding != context.binding)
                             throw std::runtime_error("Native Touch original target conflicts with a "
                                                      "replacement curve; checkpoint retained");
                         curve->hostedBinding = context.binding;
                         curve->read = false;
                         const auto finish = end + (int64_t)std::llround(d.sampleRate * .1);
                         std::erase_if(curve->points, [&](auto &p)
                                       { return p.frame >= values.front().frame && p.frame <= finish; });
                         curve->points.insert(curve->points.end(), values.begin(), values.end());
                         if (values.back().frame < end)
                             curve->points.push_back(
                                 {juce::Uuid().toString(), end, values.back().value, false});
                         curve->points.push_back({juce::Uuid().toString(), finish, returned, false});
                         std::sort(curve->points.begin(), curve->points.end(),
                                   [](auto &a, auto &b) { return a.frame < b.frame; });
                     }
                 });
            auto *lane = automationLane(document.activeMix(), context.owner);
            auto *curve = lane ? automationCurve(*lane, context.target) : nullptr;
            bool retained = curve && curve->hostedBinding && *curve->hostedBinding == context.binding;
            if (retained)
                for (auto &point : values)
                {
                    auto p = std::find_if(
                        curve->points.begin(), curve->points.end(), [&](auto &p)
                        { return p.frame == point.frame && std::abs(p.value - point.value) < 1e-7; });
                    if (p == curve->points.end())
                    {
                        retained = false;
                        break;
                    }
                }
            if (!retained)
                continue;
            context.applied = true;
            if (!resolved)
                message =
                    "Native Touch retained as unavailable; restore the original target or repair its curve";
            else if (pass["reason"].toString().isNotEmpty())
                message = pass["reason"].toString();
        }
        retainHostedTouch(key);
    }
}
void DesignSession::retainHostedTouch(const juce::String &key)
{
    auto at = nativeTouches.find(key);
    if (at == nativeTouches.end() || !at->second.applied || at->second.retentionPending)
        return;
    at->second.retentionPending = true;
    auto context = at->second;
    auto snapshot = document;
    auto path = savedFile;
    auto retain = services.retain;
    auto post = services.post;
    juce::Component::SafePointer<DesignSession> safe(this);
    bool accepted = services.submit(
        [safe, key, context, snapshot = std::move(snapshot), path, retain, post]
        {
            juce::String error;
            try
            {
                retain(snapshot, path, true);
            }
            catch (const std::exception &e)
            {
                error = e.what();
            }
            post(
                [safe, key, context, error]
                {
                    if (!safe || safe->lifetime != context.incarnation ||
                        safe->document.id != context.document)
                        return;
                    auto at = safe->nativeTouches.find(key);
                    if (at == safe->nativeTouches.end())
                        return;
                    at->second.retentionPending = false;
                    if (error.isNotEmpty())
                    {
                        safe->message = "Native Touch remains in recovery checkpoint: " + error;
                        safe->notify();
                        return;
                    }
                    safe->acknowledgeRetainedTouch(context.slot, context.generation, context.id);
                    safe->nativeTouches.erase(at);
                });
        });
    if (!accepted)
    {
        at->second.retentionPending = false;
        message = "Native Touch retained in plugin checkpoint; background queue is busy";
    }
}
void DesignSession::acknowledgeRetainedTouch(const juce::String &slot, const juce::String &generation,
                                             uint64_t id)
{
    auto &ready = completedTouches[generation];
    ready.insert(id);
    auto &ack = retainedTouch[generation];
    auto before = ack;
    while (ready.erase(ack + 1))
        ++ack;
    if (ack == before)
        return;
    if (services.hostedTouchAck)
        services.hostedTouchAck(slot, generation, ack);
    if (liveInserts)
        liveInserts->acknowledgeTouch(slot, generation, ack);
}
} // namespace sp
