#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <utility>
#include <mutex>
#include <optional>
namespace sp
{
// Isolated helper only. Native callbacks enqueue bounded raw notifications;
// the helper message thread acknowledges their first actual processing boundary.
// Neither this class nor any plugin API is called by the device callback.
class HostedTouchCapture
{
    struct Raw
    {
        int kind = 0, index = -1;
        float value = 0;
        uint64_t id = 0;
        juce::String owner;
        bool discrete = false;
    };
    struct Point
    {
        int64_t frame = 0;
        float value = 0;
    };
    struct Pass
    {
        uint64_t id = 0;
        int index = -1;
        juce::String owner, reason;
        int64_t end = -1, returnEnd = -1;
        float value = 0, returnValue = 0;
        bool complete = false, discrete = false;
        std::vector<Point> points;
    };
    std::mutex mutex;
    std::array<Raw, 4096> queue;
    size_t queued = 0;
    int armed = -1, callbackIndex = -1;
    juce::String armOwner;
    bool armedDiscrete = false;
    uint64_t callbackId = 0, nextId = 0, sequence = 0;
    bool overflow = false;
    std::vector<Pass> passes;
    int64_t lastFrame = 0;
    void push(Raw raw)
    {
        if (queued == queue.size())
        {
            overflow = true;
            return;
        }
        queue[queued++] = std::move(raw);
    }
    Pass *flight()
    {
        for (auto it = passes.rbegin(); it != passes.rend(); ++it)
            if (!it->complete)
                return &*it;
        return nullptr;
    }
    void point(Pass &p, int64_t frame, float value)
    {
        if (!std::isfinite(value) || value < 0 || value > 1)
            throw std::runtime_error("Invalid native Touch value");
        frame = std::max(frame, p.points.empty() ? frame : p.points.back().frame);
        if (!p.points.empty() && p.points.back().frame == frame)
            p.points.back().value = value;
        else if (p.points.size() < 2044)
        {
            size_t retained = 0;
            for (auto &pass : passes)
                retained += pass.points.size();
            if (retained >= 4096)
            {
                p.reason = "Native Touch retained-point bound reached";
                p.end = frame;
                return;
            }
            p.points.push_back({frame, value});
        }
        else
        {
            p.reason = "Native Touch point limit reached; retained legal pass";
            p.end = frame;
            return;
        }
        p.value = value;
    }

  public:
    HostedTouchCapture()
    {
        passes.reserve(8);
    }
    void arm(int index, juce::String owner, uint64_t acknowledged = 0, bool discrete = false)
    {
        std::lock_guard lock(mutex);
        armed = index;
        armOwner = std::move(owner);
        armedDiscrete = discrete;
        if (index < 0 && callbackIndex >= 0)
        {
            push({2, callbackIndex, 0, callbackId, {}});
            callbackIndex = -1;
        }
        // Called only on the helper message thread. Callbacks do not read passes.
        std::erase_if(passes, [&](auto &p) { return p.complete && p.id <= acknowledged; });
    }
    bool begin(int index, float value)
    {
        std::lock_guard lock(mutex);
        if (index != armed || index < 0 || callbackIndex >= 0)
            return false;
        callbackIndex = index;
        callbackId = ++nextId;
        push({0, index, value, callbackId, armOwner, armedDiscrete});
        return true;
    }
    bool value(int index, float value)
    {
        std::lock_guard lock(mutex);
        if (index != callbackIndex || index < 0)
            return false;
        push({1, index, value, callbackId, {}});
        return true;
    }
    bool end(int index)
    {
        std::lock_guard lock(mutex);
        if (index != callbackIndex || index < 0)
            return false;
        push({2, index, 0, callbackId, {}});
        callbackIndex = -1;
        return true;
    }
    int armedIndex()
    {
        std::lock_guard lock(mutex);
        return armed;
    }
    int callbackTarget()
    {
        std::lock_guard lock(mutex);
        return callbackIndex;
    }
    bool enabled()
    {
        std::lock_guard lock(mutex);
        return armed >= 0 || queued > 0 || flight() != nullptr;
    }
    template <class Return> bool prepare(int64_t first, double rate, Return returned)
    {
        std::vector<Raw> incoming;
        bool failed = false;
        {
            std::lock_guard lock(mutex);
            incoming.assign(queue.begin(), queue.begin() + queued);
            queued = 0;
            failed = std::exchange(overflow, false);
        }
        bool changed = false;
        for (auto &raw : incoming)
        {
            if (raw.kind == 0)
            {
                if (auto *old = flight())
                {
                    old->end = std::max(lastFrame, old->points.front().frame);
                    old->complete = true;
                    old->reason = "Interrupted by a new native gesture";
                }
                size_t points = 0;
                for (auto &p : passes)
                    points += p.points.size();
                if (passes.size() >= 8 || points >= 4096)
                    throw std::runtime_error("Native Touch retention queue full; acknowledge retained passes "
                                             "before recording more");
                Pass p;
                p.id = raw.id;
                p.index = raw.index;
                p.owner = raw.owner;
                p.discrete = raw.discrete;
                p.points.reserve(2044);
                point(p, first, raw.value);
                passes.push_back(std::move(p));
                changed = true;
            }
            else if (auto *p = flight(); p && p->id == raw.id && p->reason.isEmpty())
            {
                if (raw.kind == 1)
                    point(*p, first, raw.value);
                else
                {
                    point(*p, first, p->value);
                    p->end = std::max(first, p->points.front().frame);
                    p->returnEnd = p->end + (int64_t)std::llround(rate * .1);
                    p->returnValue = returned(p->index);
                }
                changed = true;
            }
        }
        if (failed)
        {
            if (auto *p = flight())
            {
                p->reason = "Native Touch callback queue overflow; retained acknowledged points";
                p->end = std::max(first, p->points.front().frame);
                p->returnEnd = p->end + (int64_t)std::llround(rate * .1);
                p->returnValue = returned(p->index);
            }
            changed = true;
        }
        if (auto *p = flight(); p && p->end >= 0 && p->returnEnd < 0)
        {
            p->returnEnd = p->end + (int64_t)std::llround(rate * .1);
            p->returnValue = returned(p->index);
            changed = true;
        }
        if (changed)
            ++sequence;
        return changed;
    }
    std::optional<float> overrideValue(int index, int64_t frame)
    {
        auto *p = flight();
        if (!p)
            for (auto it = passes.rbegin(); it != passes.rend(); ++it)
                if (it->index == index && it->returnEnd >= 0 && frame >= it->returnEnd &&
                    frame < ((it->returnEnd + 31) / 32) * 32)
                    return it->returnValue;
        if (!p || p->index != index)
            return {};
        if (p->end < 0)
            return p->value;
        if (p->discrete)
            return frame < p->returnEnd ? p->value : p->returnValue;
        auto phase = std::max(p->end, frame - frame % 32);
        double x = p->returnEnd <= p->end
                       ? 1
                       : std::clamp((double)(phase - p->end) / (p->returnEnd - p->end), 0., 1.);
        return (float)(p->value + (p->returnValue - p->value) * x);
    }
    int activeIndex()
    {
        auto *p = flight();
        return p ? p->index : -1;
    }
    int nextBoundary(int64_t frame, int maximum)
    {
        auto *p = flight();
        if (!p || p->end < 0)
            return maximum;
        int n = std::min(maximum, 32 - (int)(std::max<int64_t>(0, frame) % 32));
        if (p->returnEnd > frame)
            n = (int)std::min<int64_t>(n, p->returnEnd - frame);
        return std::max(1, n);
    }
    bool finishBlock(int64_t end)
    {
        lastFrame = end;
        auto *p = flight();
        if (!p || p->end < 0 || end < p->returnEnd)
            return false;
        p->complete = true;
        ++sequence;
        return true;
    }
    template <class Return> bool stop(double rate, Return returned)
    {
        {
            std::lock_guard lock(mutex);
            callbackIndex = -1;
            queued = 0;
        }
        auto *p = flight();
        if (!p)
            return false;
        if (p->end < 0)
            p->end = std::max(lastFrame, p->points.front().frame);
        if (p->returnEnd < 0)
        {
            p->returnEnd = p->end + (int64_t)std::llround(rate * .1);
            p->returnValue = returned(p->index);
        }
        p->complete = true;
        p->reason = "Native Touch completed at transport/editor boundary";
        ++sequence;
        return true;
    }
    int64_t acknowledgedFrame() const
    {
        return lastFrame;
    }
    // Complete only points from the coherent helper checkpoint. No queued UI
    // callback is promoted to audio evidence after a process failure.
    static juce::var interruptedCheckpoint(const juce::var &checkpoint, const juce::String &reason)
    {
        auto state = checkpoint.clone();
        if (auto *passes = state["touchPasses"].getArray())
            for (auto &pass : *passes)
                if (!(bool)pass["complete"] && pass["points"].size() > 0)
                {
                    auto *o = pass.getDynamicObject();
                    o->setProperty("complete", true);
                    o->setProperty("interrupted", true);
                    o->setProperty("reason", reason);
                    o->setProperty("end",
                                   std::max((juce::int64)state["touchFrame"],
                                            (juce::int64)pass["points"][pass["points"].size() - 1]["frame"]));
                }
        return state;
    }
    uint64_t serial() const
    {
        return sequence;
    }
    juce::var json() const
    {
        juce::Array<juce::var> all;
        for (auto &p : passes)
        {
            auto v = juce::var(new juce::DynamicObject);
            auto *o = v.getDynamicObject();
            o->setProperty("id", (juce::int64)p.id);
            o->setProperty("index", p.index);
            o->setProperty("owner", p.owner);
            o->setProperty("end", (juce::int64)p.end);
            o->setProperty("returnEnd", (juce::int64)p.returnEnd);
            o->setProperty("returnValue", p.returnValue);
            o->setProperty("complete", p.complete);
            o->setProperty("reason", p.reason);
            juce::Array<juce::var> points;
            for (auto &point : p.points)
            {
                auto q = juce::var(new juce::DynamicObject);
                q.getDynamicObject()->setProperty("frame", (juce::int64)point.frame);
                q.getDynamicObject()->setProperty("value", point.value);
                points.add(q);
            }
            o->setProperty("points", points);
            all.add(v);
        }
        return all;
    }
};
} // namespace sp
