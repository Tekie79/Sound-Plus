#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
namespace sp
{
// Callback-safe first-fault latch. The armed->failed and armed->committed CAS
// linearize device failure against durable Ready publication. No callback locks,
// allocation, formatting, I/O, or access to a ref-counted capture object.
class CaptureDeviceLifecycle
{
  public:
    enum Cause
    {
        stopped = 1,
        error,
        rateChanged,
        armingChanged
    };
    uint64_t epoch() const noexcept
    {
        return faultEpoch.load(std::memory_order_acquire);
    }
    uint64_t reserve(uint64_t baseline) noexcept
    {
        auto old = state.load(std::memory_order_acquire);
        if ((old & 7) == writing)
            return 0;
        auto ticket = (old & ~uint64_t(7)) + 8;
        state.store(ticket | armed, std::memory_order_release);
        if (epoch() != baseline)
            latch(armingChanged, nullptr);
        return ticket;
    }
    void signal(Cause cause, const juce::String *message = nullptr) noexcept
    {
        faultEpoch.fetch_add(1, std::memory_order_acq_rel);
        latch(cause, message);
    }
    bool failed(uint64_t ticket) const noexcept
    {
        auto s = state.load(std::memory_order_acquire);
        return ticket == 0 || (s & ~uint64_t(7)) != ticket || (s & 7) == writing || (s & 7) == fault;
    }
    bool commit(uint64_t ticket) noexcept
    {
        auto expected = ticket | armed;
        return ticket &&
               state.compare_exchange_strong(expected, ticket | committed, std::memory_order_acq_rel);
    }
    void cancel(uint64_t ticket) noexcept
    {
        auto expected = ticket | armed;
        state.compare_exchange_strong(expected, ticket | committed, std::memory_order_acq_rel);
    }
    juce::String diagnostic(uint64_t ticket) const
    {
        auto s = state.load(std::memory_order_acquire);
        // Worker/message-thread reader only. `writing` has already won the fault
        // CAS, but its text is not published yet. Never retain a generic first
        // error simply because that callback was preempted during the short copy.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while ((s & ~uint64_t(7)) == ticket && (s & 7) == writing)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return "Audio device fault diagnostic publication stalled";
            std::this_thread::yield();
            s = state.load(std::memory_order_acquire);
        }
        if ((s & ~uint64_t(7)) != ticket || (s & 7) != fault)
            return "Audio device lifecycle interrupted during Arming";
        char text[192]{};
        for (size_t i = 0; i < detail.size(); ++i)
            text[i] = detail[i].load(std::memory_order_relaxed);
        auto c = cause.load(std::memory_order_relaxed);
        if (state.load(std::memory_order_acquire) != s)
            return "Audio device capture generation interrupted";
        if (c == stopped)
            return "Audio device stopped during capture";
        if (c == rateChanged)
            return "Audio device rate changed during capture";
        if (c == error)
            return "Audio device error during capture: " + juce::String(text);
        return "Audio device lifecycle changed while Arming; retry deliberately";
    }

  private:
    friend struct CaptureDeviceLifecycleTestAccess;
    static constexpr uint64_t armed = 1, writing = 2, fault = 3, committed = 4;
    std::atomic<uint64_t> state{0}, faultEpoch{0};
    std::atomic<int> cause{0};
    std::array<std::atomic<char>, 192> detail{};
    void latch(Cause c, const juce::String *message) noexcept
    {
        auto expected = state.load(std::memory_order_acquire);
        if ((expected & 7) != armed ||
            !state.compare_exchange_strong(expected, (expected & ~uint64_t(7)) | writing,
                                           std::memory_order_acq_rel))
            return;
        cause.store(c, std::memory_order_relaxed);
        size_t i = 0;
        if (message)
        {
            auto p = message->getCharPointer();
            while (*p && i + 1 < detail.size())
            {
                auto ch = *p++;
                detail[i++].store(ch >= 32 && ch < 127 ? (char)ch : '?', std::memory_order_relaxed);
            }
        }
        while (i < detail.size())
            detail[i++].store(0, std::memory_order_relaxed);
        state.store((expected & ~uint64_t(7)) | fault, std::memory_order_release);
    }
};
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<char>::is_always_lock_free);
} // namespace sp
