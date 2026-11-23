// Decoder/render producer only. All processors, exchanges and aligned dry storage
// are prepared before graph adoption. Device callbacks never enter this class.
class OrderedRack
{
    struct Step
    {
        InsertRef ref;
        std::unique_ptr<StockProcessor> stock;
        std::unique_ptr<NativeEffectProcessor> native;
        StockEffect base;
        std::vector<AutomationTarget> targets;
        AutomationTarget bypassTarget;
        std::unique_ptr<HostedRackClient> hosted;
    };
    std::vector<Step> steps;
    juce::AudioBuffer<float> dry, ring;
    juce::SmoothedValue<float> wet;
    int delay = 0, position = 0, channels = 2;
    bool fixed = false, initialBypass = false;
    const CompiledAutomation *automation = nullptr;
    int64_t automationInput = 0;
    static const StockEffect &stock(const InsertChain &c, const juce::String &id)
    {
        for (auto &s : c.stock)
            if (s.id == id)
                return s;
        throw std::runtime_error("Missing ordered stock slot");
    }
    static HostedRack hosted(const InsertChain &c, const juce::String &id)
    {
        auto rack = c.hosted;
        for (auto &s : c.hosted.slots)
            if (s.id == id)
            {
                rack.slots = {s};
                rack.id = id;
                if (rack.runtimeEndpoint != juce::File())
                {
                    rack.runtimeOwnerRack =
                        c.hosted.runtimeOwnerRack.isEmpty() ? c.hosted.id : c.hosted.runtimeOwnerRack;
                    if (c.hosted.runtimeOwnerRack.isEmpty())
                        rack.runtimeEndpoint =
                            rack.runtimeEndpoint.getChildFile(id).getChildFile("endpoint.json");
                }
                return rack;
            }
        throw std::runtime_error("Missing ordered hosted slot");
    }

  public:
    OrderedRack(const InsertChain &c, const ChannelLayout &bus, double rate, bool invariant, bool aware,
                bool immutable, const std::function<bool()> &cancelled, MidiTiming timing = {})
        : channels(bus.size()), fixed(immutable), initialBypass(c.bypass)
    {
        negotiateStockBus({bus, bus, false});
        for (auto &ref : resolvedInsertOrder(c))
        {
            if (cancelled())
                throw QueryCancelled();
            Step step;
            step.ref = ref;
            if (ref.kind == InsertKind::stock)
            {
                step.base = stock(c, ref.id);
                for (int p = 0; p < effectParameterCount(step.base); ++p)
                    step.targets.push_back({ref.id, effectParameterId(step.base, p)});
                step.bypassTarget = {ref.id, "bypass"};
                if (step.base.nativeVersion)
                {
                    step.native =
                        std::make_unique<NativeEffectProcessor>(step.base, bus, rate, timing, cancelled);
                    delay += step.native->latency();
                }
                else
                    step.stock =
                        std::make_unique<StockProcessor>(step.base, rate, invariant, channels, aware);
            }
            else
            {
                auto recipe = hosted(c, ref.id);
                recipe.runtimeFixedState = fixed;
                step.hosted = std::make_unique<HostedRackClient>(recipe, bus, rate, cancelled);
                delay += step.hosted->latency();
            }
            steps.push_back(std::move(step));
        }
        if (delay > rate)
            throw std::runtime_error("Ordered chain latency exceeds one second");
        dry.setSize(channels, 512);
        ring.setSize(channels, std::max(1, delay));
        ring.clear();
        wet.reset(rate, .01);
        wet.setCurrentAndTargetValue(c.bypass ? 0.f : 1.f);
    }
    void appendMeters(juce::Array<juce::var> &rows) const
    {
        for (auto &step : steps)
            if (step.native)
            {
                auto row = juce::var(new juce::DynamicObject());
                row.getDynamicObject()->setProperty("id", step.ref.id);
                row.getDynamicObject()->setProperty("reductionDb", step.native->gainReductionDb());
                rows.add(row);
            }
    }
    int latency() const
    {
        return delay;
    }
    void update(const InsertChain &c)
    {
        if (fixed && c.bypass != initialBypass)
            throw std::runtime_error("Frozen take chain bypass cannot change");
        auto order = resolvedInsertOrder(c);
        if (order.size() != steps.size())
            throw std::runtime_error("Insert chain topology changed; prepare a graph");
        for (size_t i = 0; i < steps.size(); ++i)
        {
            auto &s = steps[i];
            if (s.ref != order[i])
                throw std::runtime_error("Insert order changed; prepare a graph");
            if (s.stock || s.native)
            {
                s.base = stock(c, s.ref.id);
                if (s.native)
                    s.native->update(s.base);
                else
                    s.stock->update(s.base);
            }
            else
                s.hosted->update(hosted(c, s.ref.id));
        }
        wet.setTargetValue(c.bypass ? 0.f : 1.f);
    }
    void automate(const CompiledAutomation &snapshot, int64_t inputFrame)
    {
        automation = &snapshot;
        automationInput = inputFrame;
    }
    void process(juce::AudioBuffer<float> &b, int count)
    {
        for (int n = 0; n < count; ++n)
        {
            for (int c = 0; c < channels; ++c)
            {
                auto input = b.getSample(c, n);
                dry.setSample(c, n, delay ? ring.getSample(c, position) : input);
                if (delay)
                    ring.setSample(c, position, input);
            }
            if (delay)
                position = (position + 1) % delay;
        }
        // Whole-chain bypass never changes slot flags or freezes wet history.
        int prefix = 0;
        for (auto &s : steps)
        {
            if ((s.stock || s.native) && automation)
            {
                for (int offset = 0; offset < count;)
                {
                    // Native lookahead controls address the sample leaving the
                    // processor, not the future detector/input sample.
                    auto nativeLatency = s.native ? s.native->latency() : 0;
                    auto frame = std::max<int64_t>(0, automationInput + offset - prefix - nativeLatency);
                    int n = automation->untilBoundary(frame, count - offset);
                    auto phase = automation->segmentFrame(frame);
                    auto effect = s.base;
                    for (int p = 0; p < effectParameterCount(effect); ++p)
                        setEffectParameterValue(
                            effect, p,
                            automation->value(s.targets[(size_t)p], effectParameterValue(effect, p), phase));
                    effect.bypass = automation->value(s.bypassTarget, effect.bypass ? 1 : 0, frame) >= .5;
                    if (s.native)
                        s.native->update(effect);
                    else
                        s.stock->update(effect);
                    juce::AudioBuffer<float> slice(b.getArrayOfWritePointers(), channels, offset, n);
                    if (s.native)
                        s.native->process(slice, n);
                    else
                        s.stock->process(slice, n);
                    offset += n;
                }
            }
            else if (s.stock)
                s.stock->process(b, count);
            else if (s.native)
                s.native->process(b, count);
            else
            {
                s.hosted->process(b, count);
                prefix += s.hosted->latency();
            }
            if (s.native)
                prefix += s.native->latency();
        }
        for (int n = 0; n < count; ++n)
        {
            auto amount = wet.getNextValue();
            if (amount == 1.f)
                continue; // Preserve historical full-wet PCM bit for bit.
            for (int c = 0; c < channels; ++c)
                b.setSample(c, n,
                            amount == 0.f
                                ? dry.getSample(c, n)
                                : dry.getSample(c, n) + (b.getSample(c, n) - dry.getSample(c, n)) * amount);
        }
    }
};
