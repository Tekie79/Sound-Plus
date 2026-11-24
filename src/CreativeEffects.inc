// Prepared by AdvancedEffectProcessor. All channel and FFT storage is owned by the instance.
struct Granular final : Engine
{
    enum
    {
        Size,
        Density,
        Position,
        Spray,
        Pitch,
        Reverse,
        Window,
        Width,
        Seed,
        Freeze
    };
    struct Grain
    {
        int age = 0, length = 0;
        double source = 0, speed = 1, spread = 0;
        int window = 0;
    };
    std::array<Grain, 32> voices{};
    std::vector<float> history;
    const double rate;
    const int channels, capacity, right;
    int head = 0;
    double untilNext = 0;
    uint64_t sequence;
    std::array<float, 2049> hann{};
    Granular(const StockEffect &e, const ChannelLayout &bus, double sr)
        : rate(sr), channels(bus.size()), capacity((int)std::ceil(2 * rate) + 2),
          right(bus.roles.indexOf("R")),
          sequence((uint64_t)e.id.hashCode64() ^ (uint64_t)nativeValue(e, "seed"))
    {
        history.resize((size_t)channels * capacity);
        for (size_t i = 0; i < hann.size(); ++i)
            hann[i] = (float)(.5 - .5 * std::cos(2 * pi * i / (hann.size() - 1)));
    }
    double random()
    {
        auto z = (sequence += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        return double((z ^ (z >> 31)) >> 11) * 0x1p-53;
    }
    void tick(const Frame &dry, Frame &wet, const Parameters &p) override
    {
        const auto &v = p.values;
        if (untilNext <= 0)
        {
            auto it = std::find_if(voices.begin(), voices.end(), [](auto &a) { return a.age >= a.length; });
            // 60 grains/s times 400 ms permits at most 25 simultaneous voices.
            if (it != voices.end())
            {
                it->age = 0;
                it->length = std::max(2, (int)std::round(rate * v[Size] / 1000));
                it->source =
                    head - 1 - rate * std::clamp(v[Position] + (2 * random() - 1) * v[Spray], 0., 1.);
                it->speed = std::exp2(v[Pitch] / 12.) * (random() < v[Reverse] ? -1 : 1);
                it->spread = (random() - .5) * rate * .05 * v[Width];
                it->window = (int)v[Window];
            }
            untilNext += rate / v[Density];
        }
        untilNext -= 1;
        double total = 0;
        for (auto &voice : voices)
        {
            if (voice.age >= voice.length)
                continue;
            double t = (double)voice.age / (voice.length - 1);
            double a = voice.window ? 1 - std::abs(2 * t - 1) : hann[(size_t)std::lround(t * 2048)];
            total += a;
            for (int c = 0; c < channels; ++c)
            {
                double pos = voice.source + (c == right ? voice.spread : 0);
                while (pos < 0)
                    pos += capacity;
                while (pos >= capacity)
                    pos -= capacity;
                int i = (int)pos;
                auto *pcm = history.data() + (size_t)c * capacity;
                wet[(size_t)c] += a * (pcm[i] + (pcm[(i + 1) % capacity] - pcm[i]) * (pos - i));
            }
            voice.source += voice.speed;
            ++voice.age;
        }
        for (int c = 0; c < channels; ++c)
        {
            wet[(size_t)c] /= std::max(1., total);
            if (v[Freeze] < .5)
                history[(size_t)c * capacity + head] = (float)dry[(size_t)c];
        }
        if (v[Freeze] < .5)
            head = (head + 1) % capacity;
    }
};

struct Spectral final : Engine
{
    using Complex = std::complex<float>;
    enum
    {
        Size,
        Overlap,
        Smoothing,
        Blur,
        Hold,
        Phase
    };
    const int size, hop, channels;
    const double rate;
    const bool freeze;
    juce::dsp::FFT fft;
    std::vector<float> input, ola, window, magnitude, envelope;
    std::vector<Complex> captured, frame, bins;
    int cursor = 0, hopClock = 0, captureOffset = 0;
    bool holding = false;
    Spectral(const StockEffect &e, const ChannelLayout &bus, double sr)
        : size(256 << (int)nativeValue(e, "fft-size")), hop(size / (nativeValue(e, "overlap") < .5 ? 2 : 4)),
          channels(bus.size()), rate(sr), freeze(e.type == "spectral-freeze"),
          fft((int)nativeValue(e, "fft-size") + 8), input((size_t)channels * size), ola(input.size()),
          window(size), magnitude(size / 2 + 1), envelope((size_t)channels * magnitude.size()),
          captured(freeze ? input.size() : 0), frame(size), bins(size)
    {
        for (int j = 0; j < size; ++j)
            window[j] = (float)std::sin(pi * (j + .5) / size);
    }
    int latency() const override
    {
        return size;
    }
    void analyse(int ch)
    {
        const float *in = input.data() + (size_t)ch * size;
        for (int j = 0; j < size; ++j)
            frame[j] = in[(cursor + j) % size] * window[j];
        fft.perform(frame.data(), bins.data(), false);
    }
    void synthesise(int ch, const Parameters &p)
    {
        if (holding)
        {
            const auto *held = captured.data() + (size_t)ch * size;
            const double increment = p.values[Phase] < .5 ? 2 * pi * captureOffset / size : 0;
            const Complex rotation((float)std::cos(increment), (float)std::sin(increment));
            Complex phase{1, 0};
            for (int j = 0; j <= size / 2; ++j)
            {
                bins[j] = held[j] * phase;
                phase *= rotation;
            }
        }
        else
            analyse(ch);
        const int radius = (int)std::lround(p.values[Smoothing] * 8);
        auto *env = envelope.data() + (size_t)ch * magnitude.size();
        double memory = p.values[Blur] <= .00001 ? 0 : std::exp(-hop / (rate * p.values[Blur]));
        for (int j = 0; j <= size / 2; ++j)
            magnitude[j] = std::abs(bins[j]);
        for (int j = 0; j <= size / 2; ++j)
        {
            double mag = magnitude[j];
            if (radius)
            {
                int lo = std::max(0, j - radius), hi = std::min(size / 2, j + radius);
                mag = 0;
                for (int k = lo; k <= hi; ++k)
                    mag += magnitude[k];
                mag /= (hi - lo + 1);
            }
            env[j] = (float)(memory * env[j] + (1 - memory) * mag);
            if (radius || memory)
                bins[j] = magnitude[j] > 1.e-12f ? bins[j] * (env[j] / magnitude[j]) : Complex(env[j], 0);
            if (j && j < size / 2)
                bins[size - j] = std::conj(bins[j]);
        }
        fft.perform(bins.data(), frame.data(), true);
        float *out = ola.data() + (size_t)ch * size;
        const float norm = 2.f * hop / size;
        for (int j = 0; j < size; ++j)
            out[(cursor + j) % size] += norm * window[j] * frame[j].real();
    }
    void tick(const Frame &dry, Frame &wet, const Parameters &p) override
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            const size_t i = (size_t)ch * size + cursor;
            wet[ch] = ola[i];
            ola[i] = 0;
            input[i] = (float)dry[ch];
        }
        cursor = (cursor + 1) % size;
        const bool capture = freeze && p.values[Hold] >= .5;
        if (capture && !holding)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                analyse(ch);
                std::copy(bins.begin(), bins.end(), captured.begin() + (size_t)ch * size);
            }
            captureOffset = 0;
        }
        holding = capture;
        if (++hopClock == hop)
        {
            hopClock = 0;
            for (int ch = 0; ch < channels; ++ch)
                synthesise(ch, p);
        }
        if (holding)
            captureOffset = (captureOffset + 1) % size;
    }
};

// The first slice in each group passes through and records. Subsequent slices
// replay that audio by project frame, even when the host changes block size.
struct Stutter final : Engine
{
    enum
    {
        Sync,
        Time,
        Division,
        Repeats,
        Gate
    };
    const double rate, bpm;
    const int channels, capacity;
    std::vector<float> captured;
    int64_t clock = 0, groupStart = -1;
    int stored = 0, previousPeriod = 0;
    Stutter(const ChannelLayout &bus, double sr, MidiTiming timing)
        : rate(sr), bpm(timing.bpm), channels(bus.size()), capacity((int)std::ceil(sr * 4)),
          captured((size_t)channels * capacity)
    {
        validateAdvancedClock(timing);
    }
    void setClock(int64_t frame) override
    {
        if (frame != clock)
        {
            groupStart = -1;
            stored = 0;
        }
        clock = frame;
    }
    void tick(const Frame &dry, Frame &wet, const Parameters &p) override
    {
        auto &v = p.values;
        const auto duration = v[Sync] >= .5 ? 60 / bpm * delayBeats((int)v[Division]) : v[Time] / 1000.;
        const int period = std::clamp((int)std::llround(rate * duration), 1, capacity);
        const int repeats = 2 + (int)v[Repeats];
        auto now = clock++;
        if (now < 0)
        {
            wet = dry;
            return;
        }
        auto group = now / (period * repeats) * (period * repeats);
        int phase = (int)((now - group) % period);
        if (previousPeriod != period || groupStart != group)
        {
            stored = 0;
            groupStart = group;
            previousPeriod = period;
        }
        if (now - group < period)
        {
            if (phase == stored)
            {
                for (int ch = 0; ch < channels; ++ch)
                    captured[(size_t)ch * capacity + phase] = (float)dry[ch];
                ++stored;
            }
            wet = dry;
        }
        else if (stored == period)
        {
            const int active = std::max(1, (int)std::round(v[Gate] * period));
            const int ramp = std::max(1, std::min(active / 2, (int)std::round(rate * .001)));
            double envelope = std::clamp(std::min(phase, active - 1 - phase) / (double)ramp, 0., 1.);
            for (int ch = 0; ch < channels; ++ch)
                wet[ch] = envelope * captured[(size_t)ch * capacity + phase];
        }
        else
            wet = dry; // A live seek without history resumes captures on the next complete group.
    }
};
