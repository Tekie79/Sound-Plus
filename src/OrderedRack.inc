// Decoder/render producer only. All processors, exchanges and aligned dry storage
// are prepared before graph adoption. Device callbacks never enter this class.
class OrderedRack
{
    struct Step
    {
        InsertRef ref;
        std::unique_ptr<StockProcessor> stock;
        std::unique_ptr<HostedRackClient> hosted;
    };
    std::vector<Step> steps;
    juce::AudioBuffer<float> dry, ring;
    juce::SmoothedValue<float> wet;
    int delay = 0, position = 0, channels = 2;
    bool fixed = false, initialBypass = false;
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
                bool immutable, const std::function<bool()> &cancelled)
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
                step.stock =
                    std::make_unique<StockProcessor>(stock(c, ref.id), rate, invariant, channels, aware);
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
            if (s.stock)
                s.stock->update(stock(c, s.ref.id));
            else
                s.hosted->update(hosted(c, s.ref.id));
        }
        wet.setTargetValue(c.bypass ? 0.f : 1.f);
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
        for (auto &s : steps)
            if (s.stock)
                s.stock->process(b, count);
            else
                s.hosted->process(b, count);
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
