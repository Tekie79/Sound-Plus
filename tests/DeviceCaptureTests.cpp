#include "DeviceCaptureTests.h"
#include "AudioEngine.h"
#include "DesignSession.h"
#include "Performance.h"
#include <iostream>
#include <deque>
#include <future>
namespace
{
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
template <class F> void until(F f, const char *why, int ms = 10000)
{
    auto end = sp::monotonicMs() + ms;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    check(f(), why);
}
struct Device final : juce::AudioIODevice
{
    Device() : AudioIODevice("Controlled lifecycle device", "QA") {}
    double rate = 48000;
    juce::StringArray getOutputChannelNames() override
    {
        return {"L", "R"};
    }
    juce::StringArray getInputChannelNames() override
    {
        return {};
    }
    juce::Array<double> getAvailableSampleRates() override
    {
        return {44100, 48000};
    }
    juce::Array<int> getAvailableBufferSizes() override
    {
        return {512};
    }
    int getDefaultBufferSize() override
    {
        return 512;
    }
    juce::String open(const juce::BigInteger &, const juce::BigInteger &, double r, int) override
    {
        rate = r;
        return {};
    }
    void close() override {}
    bool isOpen() override
    {
        return true;
    }
    void start(juce::AudioIODeviceCallback *) override {}
    void stop() override {}
    bool isPlaying() override
    {
        return true;
    }
    juce::String getLastError() override
    {
        return {};
    }
    int getCurrentBufferSizeSamples() override
    {
        return 512;
    }
    double getCurrentSampleRate() override
    {
        return rate;
    }
    int getCurrentBitDepth() override
    {
        return 24;
    }
    juce::BigInteger getActiveOutputChannels() const override
    {
        return juce::BigInteger(3);
    }
    juce::BigInteger getActiveInputChannels() const override
    {
        return {};
    }
    int getOutputLatencyInSamples() override
    {
        return 0;
    }
    int getInputLatencyInSamples() override
    {
        return 0;
    }
};
sp::DesignDocument fixture(const juce::File &root)
{
    auto file = root.getChildFile("sustained.wav");
    auto stream = file.createOutputStream();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream.release(), 48000, 2, 24, {}, 0));
    juce::AudioBuffer<float> b(2, 48000 * 5);
    for (int n = 0; n < b.getNumSamples(); ++n)
        for (int c = 0; c < 2; ++c)
            b.setSample(c, n, .1f * std::sin(n * .03));
    check(w && w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()),
          "Generated native-handler fixture finalized");
    w.reset();
    sp::DesignDocument d;
    d.name = "Lifecycle QA";
    d.original.layers = {sp::makeDesignLayer(sp::inspectDesignSource(file), 0, -1, true)};
    return d;
}
} // namespace
namespace sp
{
struct CaptureDeviceLifecycleTestAccess
{
    static void holdPublication(CaptureDeviceLifecycle &l, uint64_t ticket)
    {
        l.state.store(ticket | l.writing, std::memory_order_release);
    }
    static void publish(CaptureDeviceLifecycle &l, uint64_t ticket)
    {
        l.cause.store(CaptureDeviceLifecycle::error, std::memory_order_relaxed);
        const char text[] = "controlled first fault";
        for (size_t i = 0; i < sizeof(text); ++i)
            l.detail[i].store(text[i], std::memory_order_relaxed);
        l.state.store(ticket | l.fault, std::memory_order_release);
    }
};
struct AudioEngineTestAccess
{
    static void start(AudioEngine &e, Device &d)
    {
        e.audioDeviceAboutToStart(&d);
    }
    static void stopped(AudioEngine &e)
    {
        e.audioDeviceStopped();
    }
    static void error(AudioEngine &e, const juce::String &m)
    {
        e.audioDeviceError(m);
    }
    static void handle(AudioEngine &e)
    {
        e.handleDeviceChanges();
    }
    static void send(AudioEngine &e, const juce::var &m)
    {
        e.hello = true;
        e.command(m);
    }
    static juce::var status(AudioEngine &e)
    {
        return e.playback.status();
    }
    static uint64_t epoch(AudioEngine &e)
    {
        return e.playback.deviceFaultEpoch();
    }
    static bool idle(AudioEngine &e)
    {
        return !e.playback.captureBusy() && e.lastPlay.isVoid();
    }
    static bool finishPending(AudioEngine &e)
    {
        return e.playback.captureFinish.load() != 0;
    }
    static void consume(AudioEngine &e, int blocks = 4)
    {
        juce::AudioBuffer<float> b(2, 512);
        for (int i = 0; i < blocks; ++i)
        {
            e.audioDeviceIOCallbackWithContext(nullptr, 0, b.getArrayOfWritePointers(), 2, 512, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
    }
    static void hooks(AudioEngine &e, std::function<void()> prepare, std::function<void()> commit)
    {
        e.playback.beforePrepare = std::move(prepare);
        e.playback.beforeCheckpointCommit = std::move(commit);
    }
};
} // namespace sp
void runDeviceCaptureContract(const juce::File &parent, bool layoutAware)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto base = fixture(root);
    if (layoutAware)
    {
        sp::setMixLayout(base.original, sp::channelLayout("5.1-side-v1"));
        base.renderSettings.layout = "same";
    }
    std::atomic<bool> holdPrepare{false}, prepareHeld{false}, holdCommit{false}, commitHeld{false};
    sp::AudioEngine engine(false);
    Device device;
    using A = sp::AudioEngineTestAccess;
    A::hooks(
        engine,
        [&]
        {
            if (holdPrepare)
            {
                prepareHeld = true;
                while (holdPrepare)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        },
        [&]
        {
            if (holdCommit)
            {
                commitHeld = true;
                while (holdCommit)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    struct Release
    {
        std::atomic<bool> &a;
        std::atomic<bool> &b;
        ~Release()
        {
            a = false;
            b = false;
        }
    } release{holdPrepare, holdCommit};
    A::start(engine, device);
    A::handle(engine);
    check(A::epoch(engine) == 0 && A::idle(engine),
          "Expected initial idle start does not create capture failure");
    A::stopped(engine);
    A::start(engine, device);
    A::handle(engine);
    check(A::idle(engine), "Idle stop/start creates no stale capture intent");
    uint64_t generation = 0, reservation = 0;
    juce::Array<juce::var> observations;
    auto begin = [&](const juce::String &name)
    {
        auto d = base;
        auto t = sp::freezeTake(d, name);
        t.checkpoint = root.getChildFile(t.id + "-checkpoint.soundplus").getFullPathName();
        d.takes = {t};
        auto file = root.getChildFile(t.id + "-input.soundplus");
        sp::saveDesignAtomic(file, d);
        auto r = sp::message("reserve_capture");
        auto *o = r.getDynamicObject();
        o->setProperty("reserve", true);
        o->setProperty("reservationId", (juce::int64)++reservation);
        o->setProperty("deviceFaultEpoch", (juce::int64)A::epoch(engine));
        A::send(engine, r);
        auto play = sp::message("play_design");
        o = play.getDynamicObject();
        o->setProperty("id", "design:" + d.id);
        o->setProperty("path", file.getFullPathName());
        o->setProperty("playGeneration", (juce::int64)++generation);
        o->setProperty("positionSample", 0);
        o->setProperty("mode", "record");
        A::send(engine, play);
        return t;
    };
    auto recording = [&](const sp::PerformTake &t)
    {
        until(
            [&]
            {
                auto s = A::status(engine);
                return s["takeId"].toString() == t.id && (int)s["takePhase"] == sp::TakeCapture::recording;
            },
            "Actual handlers/service adopt Recording");
        A::consume(engine);
    };
    auto failed = [&](const sp::PerformTake &t, const char *diagnostic)
    {
        until(
            [&]
            {
                auto s = A::status(engine);
                return s["takeId"].toString() == t.id && (int)s["takePhase"] == sp::TakeCapture::incomplete;
            },
            "Relevant device failure produces Incomplete");
        A::handle(engine);
        auto s = A::status(engine);
        if ((bool)s["takeCheckpointReady"] || !s["takeError"].toString().contains(diagnostic) ||
            !A::idle(engine))
            std::cerr << "Device failure observation (" << diagnostic << ") idle=" << A::idle(engine)
                      << " status=" << juce::JSON::toString(s, true) << std::endl;
        check(!(bool)s["takeCheckpointReady"] && s["takeError"].toString().contains(diagnostic) &&
                  A::idle(engine),
              "First useful diagnostic retained, ownership released, stale intent cleared");
        until(
            [&]
            {
                try
                {
                    auto d = sp::loadDesign(juce::File(t.checkpoint));
                    return !d.takes[0].complete && d.takes[0].bodyEnd == (int64_t)s["takeEnd"] &&
                           d.takes[0].reason.contains(diagnostic);
                }
                catch (...)
                {
                    return false;
                }
            },
            "Durable recoverable prefix is explicitly Incomplete");
        observations.add(s);
        return s;
    };
    for (int which = 0; which < 3; ++which)
    {
        device.rate = 48000;
        A::start(engine, device);
        A::handle(engine);
        auto t = begin("Device fault " + juce::String(which));
        recording(t);
        if (which == 0)
        {
            A::stopped(engine);
            A::start(engine, device);
        } // No timer between callbacks.
        if (which == 1)
        {
            A::error(engine, "unchanged-rate diagnostic");
            A::error(engine, "later diagnostic must not replace first");
        }
        if (which == 2)
        {
            device.rate = 44100;
            A::start(engine, device);
        }
        A::handle(engine);
        auto s = failed(t, which == 0   ? "stopped"
                           : which == 1 ? "unchanged-rate diagnostic"
                                        : "rate changed");
        check((int64_t)s["takeEnd"] > 0, "Interruption preserves actual callback-consumed prefix");
    }
    device.rate = 48000;
    A::start(engine, device);
    A::handle(engine);
    holdPrepare = true;
    auto arming = begin("Interrupted Arming");
    until([&] { return prepareHeld.load(); }, "Actual preparer held after draft acceptance");
    A::error(engine, "Arming diagnostic");
    A::start(engine, device);
    A::handle(engine);
    auto armedFailure = failed(arming, "Arming diagnostic");
    check((int64_t)armedFailure["takeEnd"] == 0, "Interrupted Arming cannot start a capture clock");
    holdPrepare = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // A fresh deliberate request takes a new baseline rather than inheriting the old fault.
    auto fresh = begin("Fresh deliberate recovery");
    recording(fresh);
    auto finish = sp::message("finish_take");
    finish.getDynamicObject()->setProperty("playGeneration", (juce::int64)generation);
    finish.getDynamicObject()->setProperty("cancel", false);
    A::send(engine, finish);
    until(
        [&]
        {
            auto s = A::status(engine);
            return s["takeId"].toString() == fresh.id && (int)s["takePhase"] == sp::TakeCapture::ready;
        },
        "Fresh deliberate capture after recovery reaches durable Ready");
    check(sp::loadDesign(juce::File(fresh.checkpoint)).takes[0].complete,
          "Fresh capture does not inherit stale diagnostic");
    // Manual Stop can arrive after the final sample already started natural finalization.
    holdCommit = true;
    commitHeld = false;
    const auto savedDuration = base.original.duration;
    base.original.duration = 3072;
    auto natural = begin("Natural completion followed by late Stop");
    base.original.duration = savedDuration;
    recording(natural);
    A::consume(engine, 5);
    until([&] { return commitHeld.load(); }, "Final consumed frame entered checkpoint commit");
    const auto finished = A::status(engine);
    check((int)finished["takePhase"] == sp::TakeCapture::finalizing && (int64_t)finished["takeEnd"] == 3072 &&
              !(bool)finished["playing"],
          "Natural end retired the stream with its full consumed prefix");
    finish.getDynamicObject()->setProperty("playGeneration", (juce::int64)generation);
    A::send(engine, finish);
    until([&] { return !A::finishPending(engine); }, "Producer consumed late Stop");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    check((int)A::status(engine)["takePhase"] == sp::TakeCapture::finalizing &&
              (int64_t)A::status(engine)["takeEnd"] == 3072,
          "Late Stop does not invalidate natural finalization or reset its consumed prefix");
    holdCommit = false;
    until([&] { return (int)A::status(engine)["takePhase"] == sp::TakeCapture::ready; },
          "Natural completion reaches durable Ready despite late Stop");
    const auto retained = sp::loadDesign(juce::File(natural.checkpoint)).takes.front();
    A::send(engine, finish);
    until([&] { return !A::finishPending(engine); }, "Producer consumed a second late Stop");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    check((int)A::status(engine)["takePhase"] == sp::TakeCapture::ready &&
              sp::loadDesign(juce::File(natural.checkpoint)).takes.front().eventHash == retained.eventHash,
          "Repeated Stop leaves a published Ready take intact");
    holdCommit = true;
    commitHeld = false;
    auto late = begin("Late checkpoint failure");
    recording(late);
    finish.getDynamicObject()->setProperty("playGeneration", (juce::int64)generation);
    A::send(engine, finish);
    until([&] { return commitHeld.load(); }, "Durable final checkpoint held before Ready CAS");
    A::error(engine, "late finalization diagnostic");
    A::stopped(engine);
    A::start(engine, device);
    A::handle(engine);
    until([&] { return (int)A::status(engine)["takePhase"] == sp::TakeCapture::incomplete; },
          "Finalizing failure wins before durable Ready publication");
    holdCommit = false;
    failed(late, "late finalization diagnostic");
    std::this_thread::sleep_for(std::chrono::milliseconds(280));
    check((int)A::status(engine)["takePhase"] == sp::TakeCapture::incomplete &&
              !sp::loadDesign(juce::File(late.checkpoint)).takes[0].complete,
          "Late checkpoint completion cannot promote affected capture to Ready");
    // UI-side Arming has a different asynchronous boundary, before helper draft preparation.
    std::deque<std::function<void()>> work;
    auto state = sp::message("status");
    auto *o = state.getDynamicObject();
    o->setProperty("captureReserved", true);
    o->setProperty("captureLifecycleFailed", true);
    o->setProperty("captureReservationId", 7);
    o->setProperty("deviceReservationId", 6);
    o->setProperty("captureDeviceError", "Arming device interruption");
    sp::DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.submit = [&](auto f)
    {
        work.push_back(std::move(f));
        return true;
    };
    services.post = [](auto f) { f(); };
    services.resolve = [](auto &) {};
    services.stop = [] {};
    services.status = [&] { return state; };
    services.play = [](const auto &, const auto &, int64_t, const auto &) { return uint64_t(1); };
    services.retain = [](const auto &, const auto &, bool) {};
    services.write = [](const auto &, const auto &) {};
    sp::DesignSession session(root.getChildFile("arming/catalog.sqlite"), services);
    session.document = base;
    session.recordTake();
    session.poll();
    check(session.takeState == sp::DesignSession::TakeState::arming,
          "Old reservation failure cannot contaminate fresh UI Arming");
    o->setProperty("deviceReservationId", 7);
    session.poll();
    check(session.takeState == sp::DesignSession::TakeState::incomplete,
          "Matching lifecycle failure interrupts held UI source verification");
    while (!work.empty())
    {
        auto f = std::move(work.front());
        work.pop_front();
        f();
    }
    check(session.document.takes.empty(),
          "Stale held Arming worker cannot publish Ready or replace document");
    auto result = sp::message("device-capture-results");
    o = result.getDynamicObject();
    o->setProperty("checks", checks);
    o->setProperty("layoutAware", layoutAware);
    o->setProperty("handlerAndServiceTransitions", true);
    o->setProperty("physicalDeviceUnplug", false);
    o->setProperty("observations", observations);
    o->setProperty("freshCapture", fresh.id);
    o->setProperty("lateCheckpointRemainsIncomplete", true);
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(result, true));
    std::cout << "PASS actual device lifecycle handlers/service: " << checks << " checks\n";
}

void runDeviceCaptureTests(const juce::File &parent)
{
    sp::CaptureDeviceLifecycle latch;
    auto ticket = latch.reserve(0);
    sp::CaptureDeviceLifecycleTestAccess::holdPublication(latch, ticket);
    auto diagnostic = std::async(std::launch::async, [&] { return latch.diagnostic(ticket); });
    check(diagnostic.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
          "Fault reader does not retain a placeholder while callback publication is held");
    sp::CaptureDeviceLifecycleTestAccess::publish(latch, ticket);
    check(diagnostic.get().contains("controlled first fault"),
          "First capture diagnostic is read only after the release publication");
    auto legacy = parent.getChildFile("legacy");
    legacy.createDirectory();
    runDeviceCaptureContract(legacy, false);
    auto layout = parent.getChildFile("layout");
    layout.createDirectory();
    runDeviceCaptureContract(layout, true);
}
