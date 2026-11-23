struct Convolver final : Engine
{
    using Complex = std::complex<float>;
    struct Route
    {
        int input, output, kernel;
    };
    enum
    {
        Partition,
        Predelay,
        Width,
        Lfe,
        Mix,
        Output
    };
    int partition, fftSize, parts, head = 0, offset = 0, channels, left, right, resampleLatency = 0;
    double rate;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<int> inputs;
    std::vector<Route> routes;
    std::vector<Complex> kernels, history, time, frequency, sum, inverse;
    std::vector<float> block, ready, overlap;
    std::array<Ring, 16> pre;
    size_t at(int channel, int part) const
    {
        return ((size_t)channel * (size_t)parts + (size_t)part) * (size_t)fftSize;
    }
    Convolver(const StockEffect &effect, const ChannelLayout &bus, double sr, const IRCancel &cancelled)
        : partition(128 << (int)nativeValue(effect, "partition")), fftSize(partition * 2),
          channels(bus.size()), left(bus.roles.indexOf("L")), right(bus.roles.indexOf("R")), rate(sr)
    {
        if (!effect.impulse)
            throw std::runtime_error("Convolution Reverb unresolved: choose an impulse response");
        auto impulse = prepareImpulse(*effect.impulse, bus, sr, cancelled);
        resampleLatency = impulse.resampleLatency;
        parts = (impulse.pcm.getNumSamples() + partition - 1) / partition;
        int order = 0;
        while ((1 << order) < fftSize)
            ++order;
        fft = std::make_unique<juce::dsp::FFT>(order);
        time.resize((size_t)fftSize);
        frequency.resize(time.size());
        sum.resize(time.size());
        inverse.resize(time.size());
        kernels.resize((size_t)impulse.pcm.getNumChannels() * (size_t)parts * (size_t)fftSize);
        for (int c = 0; c < impulse.pcm.getNumChannels(); ++c)
            for (int part = 0; part < parts; ++part)
            {
                if (cancelled())
                    throw std::runtime_error("Impulse partition preparation cancelled");
                std::fill(time.begin(), time.end(), Complex{});
                for (int n = 0; n < partition; ++n)
                {
                    int source = part * partition + n;
                    if (source < impulse.pcm.getNumSamples())
                        time[(size_t)n] = {impulse.pcm.getSample(c, source), 0};
                }
                fft->perform(time.data(), kernels.data() + at(c, part), false);
            }
        for (auto &map : effect.impulse->routes)
        {
            int channel = bus.roles.indexOf(map.input);
            auto found = std::find(inputs.begin(), inputs.end(), channel);
            int lane = (int)(found - inputs.begin());
            if (found == inputs.end())
                inputs.push_back(channel);
            routes.push_back({lane, bus.roles.indexOf(map.output), map.channel});
        }
        history.resize(inputs.size() * (size_t)parts * (size_t)fftSize);
        block.resize(inputs.size() * (size_t)partition);
        ready.resize((size_t)channels * (size_t)partition);
        overlap.resize(ready.size());
        for (int c = 0; c < channels; ++c)
            pre[(size_t)c].prepare((int)std::ceil(rate * .5) + 2);
    }
    int latency() const override
    {
        return partition + resampleLatency;
    }
    void transform()
    {
        for (size_t input = 0; input < inputs.size(); ++input)
        {
            std::fill(time.begin(), time.end(), Complex{});
            for (int n = 0; n < partition; ++n)
                time[(size_t)n] = {block[input * (size_t)partition + (size_t)n], 0};
            fft->perform(time.data(), history.data() + at((int)input, head), false);
        }
        for (int output = 0; output < channels; ++output)
        {
            std::fill(sum.begin(), sum.end(), Complex{});
            for (auto &route : routes)
                if (route.output == output)
                    for (int part = 0; part < parts; ++part)
                    {
                        auto *x = history.data() + at(route.input, (head - part + parts) % parts),
                             *h = kernels.data() + at(route.kernel, part);
                        for (int bin = 0; bin < fftSize; ++bin)
                            sum[(size_t)bin] += x[bin] * h[bin];
                    }
            fft->perform(sum.data(), inverse.data(), true);
            for (int n = 0; n < partition; ++n)
            {
                auto index = (size_t)output * (size_t)partition + (size_t)n;
                ready[index] = inverse[(size_t)n].real() + overlap[index];
                overlap[index] = inverse[(size_t)(n + partition)].real();
            }
        }
        head = (head + 1) % parts;
    }
    void tick(const Frame &input, Frame &wet, const Parameters &p) override
    {
        for (int c = 0; c < channels; ++c)
        {
            auto raw = ready[(size_t)c * (size_t)partition + (size_t)offset];
            auto delay = p.values[Predelay] * rate / 1000.;
            wet[(size_t)c] =
                delay < 1 ? raw * (1 - delay) + pre[(size_t)c].read(1) * delay : pre[(size_t)c].read(delay);
            pre[(size_t)c].push(raw);
        }
        for (size_t lane = 0; lane < inputs.size(); ++lane)
            block[lane * (size_t)partition + (size_t)offset] = (float)input[(size_t)inputs[lane]];
        if (++offset == partition)
        {
            transform();
            offset = 0;
        }
        if (left >= 0 && right >= 0 && p.values[Width] != 1)
        {
            auto mid = (wet[(size_t)left] + wet[(size_t)right]) * .5;
            wet[(size_t)left] = mid + (wet[(size_t)left] - mid) * p.values[Width];
            wet[(size_t)right] = mid + (wet[(size_t)right] - mid) * p.values[Width];
        }
    }
};
