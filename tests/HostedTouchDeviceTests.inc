namespace
{
void runHostedTouchDeviceTests(const sp::DesignDocument &fixture, const juce::File &root,
                               const juce::File &app, const juce::String &owner,
                               const sp::AutomationTarget &target)
{
    using namespace sp;
    using namespace juce;
    auto folder = root.getChildFile("device");
    folder.createDirectory();
    {
        Catalog initialize(folder.getChildFile("library.sqlite"));
    }
    AudioClient audio(app);
    DesignSession live(folder.getChildFile("library.sqlite"), audio);
    live.protectStartupEdits();
    live.document = fixture;
    live.selectedIds = {fixture.selectedLayer};
    auto pump = [&](int ms)
    {
        auto end = monotonicMs() + ms;
        while (monotonicMs() < end)
        {
            live.poll();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .005, false);
        }
    };
    auto wait = [&](auto ready, const char *why)
    {
        auto end = monotonicMs() + 15000;
        while (!ready() && monotonicMs() < end)
            pump(10);
        check(ready(), why);
    };
    wait([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native Touch device opens");
    audio.deviceRate(48000);
    audio.gain(.04f);
    pump(250);
    live.play();
    wait(
        [&]
        {
            return (bool)audio.status()["playing"] &&
                   audio.status()["id"].toString() == "design:" + live.document.id;
        },
        "Native hosted Read audition starts");
    auto before = audio.status();
    File endpoint;
    for (auto &file :
         folder.getChildFile("live-inserts").findChildFiles(File::findFiles, true, "endpoint.json"))
        if (file.getParentDirectory().getFileName() == target.instance)
            endpoint = file;
    check(endpoint.existsAsFile(), "Native isolated plugin endpoint is available for resource measurement");
    auto isolatedPid = (int)JSON::parse(endpoint)["pid"];
    auto isolatedResources = [&]
    {
        rusage_info_v4 usage{};
        check(proc_pid_rusage(isolatedPid, RUSAGE_INFO_V4, (rusage_info_t *)&usage) == 0,
              "Read actual retained plugin helper resource counters");
        auto v = obj();
        put(v, "pid", isolatedPid);
        put(v, "cpuSeconds", (usage.ri_user_time + usage.ri_system_time) / 1e9);
        put(v, "residentBytes", (int64_t)usage.ri_resident_size);
        put(v, "physicalFootprintBytes", (int64_t)usage.ri_phys_footprint);
        return v;
    };
    auto isolatedBefore = isolatedResources();
    auto cpuBefore = performance.json();
    HostedRackView view(live, false, folder.getChildFile("plugins.json"), true, {}, {}, false);
    view.bind(owner);
    view.selectSlot(target.instance);
    view.refresh();
    auto *slider = gainSlider(view);
    check(slider != nullptr, "Real inline control exists for native-device test");
    slider->onDragStart();
    int inputs = 0;
    auto started = monotonicMs();
    for (int n = 0; n < 160; ++n)
    {
        slider->setValue(.5 + .2 * std::sin(n * .13), sendNotificationSync);
        ++inputs;
        pump(25);
        auto state = audio.status();
        check((bool)state["playing"] && state["playGeneration"] == before["playGeneration"] &&
                  state["graphEpoch"] == before["graphEpoch"] && state["helperPid"] == before["helperPid"],
              "Continuous hosted Touch preserves native playback generation, graph and helper");
    }
    slider->onDragEnd();
    wait(
        [&]
        {
            auto *lane = automationLane(live.document.original, owner);
            return lane && automationCurve(*lane, target)->points.size() > 10;
        },
        "Native-device Touch points actually reach the retained model");
    auto after = audio.status();
    auto report = obj();
    put(report, "inputs", inputs);
    put(report, "wallMs", monotonicMs() - started);
    put(report, "isolatedBefore", isolatedBefore);
    put(report, "isolatedAfter", isolatedResources());
    auto commands =
        endpoint.getParentDirectory().findChildFiles(File::findFiles, true, "touch-commands.json");
    int64_t largestCommand = 0;
    for (auto &file : commands)
        largestCommand = std::max(largestCommand, file.getSize());
    put(report, "largestCommandBytes", largestCommand);
    check(largestCommand > 0 && largestCommand <= 64 * 1024,
          "Actual inline control traffic remains inside64KiB bounded window");
    put(report, "audioBefore", before);
    put(report, "audioAfter", after);
    put(report, "mainBefore", cpuBefore);
    put(report, "mainAfter", performance.json());
    put(report, "retainedPoints",
        (int)automationCurve(*automationLane(live.document.original, owner), target)->points.size());
    check((int64_t)after["underruns"] == (int64_t)before["underruns"],
          "Native hosted Touch workload reports no additional underruns");
    check((int64_t)after["sourceSample"] > (int64_t)before["sourceSample"] &&
              (double)after["preMonitorPeak"] > 0,
          "Native device consumes progressing nonzero hosted PCM");
    folder.getChildFile("result.json").replaceWithText(JSON::toString(report, true));
    live.stop();
    pump(150);
    saveDesignAtomic(folder.getChildFile("Retained native Touch.soundplus"), live.document);
}
} // namespace
