namespace
{
void runHostedTouchCaptureTests()
{
    sp::HostedTouchCapture capture;
    capture.arm(3, "owner-a");
    check(!capture.begin(4, .5f), "Native Touch cannot write an unarmed parameter");
    check(capture.begin(3, .5f), "Native Touch captures exact armed parameter");
    check(capture.value(3, .2f), "Native Touch accepts value inside gesture");
    capture.arm(4, "owner-b");
    check(capture.value(3, .8f), "Selector/arm change cannot redirect active gesture");
    check(!capture.value(4, .9f), "Another target cannot acquire an in-flight gesture");
    check(capture.end(3), "Matching native End accepted");
    check(!capture.value(3, .9f), "Late callback cannot append after End");
    check(capture.prepare(100, 48000, [](int) { return .4f; }),
          "Raw callbacks acknowledge actual processing boundary");
    auto passes = capture.json();
    check(passes.size() == 1 && passes[0]["owner"].toString() == "owner-a" && (int)passes[0]["index"] == 3,
          "Captured owner and target remain stable despite arm changes");
    check((int64_t)passes[0]["points"][0]["frame"] == 100 && passes[0]["points"].size() == 1,
          "Values applied at one boundary coalesce instead of inventing prior audio");
    check(std::abs(*capture.overrideValue(3, 100) - .8) < 1e-6, "Acknowledged native value overrides Read");
    check(!capture.overrideValue(4, 100), "Read for unrelated parameter remains independent");
    auto expected = .8 + (.4 - .8) * (2496. - 100) / 4800.;
    check(std::abs(*capture.overrideValue(3, 2500) - expected) < 1e-6,
          "Native Touch return uses shared anchored32 grid and100ms policy");
    check(!capture.finishBlock(4900 - 1), "Return is not completed before actual frames pass");
    check(capture.finishBlock(4900), "Completed return acknowledged only after processing boundary");
    check((bool)capture.json()[0]["complete"],
          "Completion is exposed separately from input callback acceptance");
    check(std::abs(*capture.overrideValue(3, 4901) - .4) < 1e-6,
          "Exact return endpoint fences next grid until compiled curve adoption");
    capture.arm(4, "owner-b", 1);
    check(capture.json().size() == 0, "Only retained/acknowledged complete passes are pruned");
    sp::HostedTouchCapture discrete;
    discrete.arm(1, "owner", 0, true);
    discrete.begin(1, 0);
    discrete.value(1, 1);
    discrete.end(1);
    discrete.prepare(0, 48000, [](int) { return 0.f; });
    check(*discrete.overrideValue(1, 4700) == 1 && *discrete.overrideValue(1, 4800) == 0,
          "Discrete native Touch holds then steps at exact100ms return");
    auto checkpoint = juce::var(new juce::DynamicObject);
    checkpoint.getDynamicObject()->setProperty("touchPasses", discrete.json());
    checkpoint.getDynamicObject()->setProperty("touchFrame", (juce::int64)1024);
    auto recovered = sp::HostedTouchCapture::interruptedCheckpoint(checkpoint, "Helper stopped");
    check((bool)recovered["touchPasses"][0]["complete"] && (bool)recovered["touchPasses"][0]["interrupted"] &&
              (int64_t)recovered["touchPasses"][0]["end"] == 1024 &&
              !(bool)checkpoint["touchPasses"][0]["complete"],
          "Crash recovery closes a copy of acknowledged checkpoint without inventing input");
    check(capture.begin(4, .3f), "Independent following pass accepted once");
    capture.prepare(6000, 44100, [](int) { return .7f; });
    capture.finishBlock(6100);
    check(capture.stop(44100, [](int) { return .7f; }),
          "Transport/editor interruption retains acknowledged partial pass");
    check((bool)capture.json()[0]["complete"] && (int64_t)capture.json()[0]["end"] == 6100,
          "Interrupted native pass closes at last actual source boundary");
}
} // namespace
