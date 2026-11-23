// Original mode geometries around an eight-line normalized Hadamard FDN.
// All channels own independent state; width alone explicitly mixes front L/R.
struct Reverb final : Engine
{
    enum
    {
        Mode,
        Size,
        Predelay,
        Decay,
        Damping,
        Diffusion,
        EarlyLate,
        Width,
        Duck,
        Threshold,
        Attack,
        Release,
        Lfe,
        Mix,
        Output
    };
    static constexpr double inverseRoot8 = .3535533905932737622;
    double rate, size, lpA = 0, attackA = 0, releaseA = 0;
    int mode, channels, left, right, controlPhase = 0, stages;
    std::array<std::array<Ring, 8>, 16> lines;
    std::array<std::array<int, 8>, 16> lengths{};
    std::array<std::array<double, 8>, 16> damping{}, gains{};
    std::array<std::array<Ring, 4>, 16> diffusers;
    std::array<int, 4> diffuserLengths{}, earlyLengths{};
    std::array<Ring, 16> pre, early;
    Frame envelope{};
    static double fractional(Ring &ring, double current, double delay)
    {
        return delay < 1 ? current * (1 - delay) + ring.read(1) * delay : ring.read(delay);
    }
    Reverb(const StockEffect &e, const ChannelLayout &bus, double sr)
        : rate(sr), size(nativeValue(e, "size")), mode((int)nativeValue(e, "mode")), channels(bus.size()),
          left(bus.roles.indexOf("L")), right(bus.roles.indexOf("R")), stages(mode == 0 ? 2 : 4)
    {
        constexpr double geometry[3][8] = {{23.9, 29.7, 31.3, 37.1, 41.1, 43.7, 47.3, 53.1},
                                           {11.3, 13.7, 17.9, 19.3, 23.3, 29.9, 31.7, 37.7},
                                           {43.7, 53.9, 61.1, 67.7, 73.3, 79.7, 89.3, 97.1}};
        constexpr double reflections[3][4] = {
            {3.1, 7.3, 11.7, 19.1}, {1.7, 3.7, 5.3, 9.7}, {11, 19.7, 31.1, 47.3}};
        constexpr double diffuser[4] = {6.3, 8.9, 12.7, 19.1};
        for (int i = 0; i < 4; ++i)
        {
            diffuserLengths[(size_t)i] = std::max(1, (int)std::llround(diffuser[i] * size * sr / 1000.));
            earlyLengths[(size_t)i] =
                std::max(1, (int)std::llround(reflections[mode][i] * size * sr / 1000.));
        }
        for (int c = 0; c < channels; ++c)
        {
            unsigned seed = 0;
            for (auto ch : bus.roles[c])
                seed += (unsigned)ch;
            for (int i = 0; i < 8; ++i)
            {
                int length = std::max(1, (int)std::llround(geometry[mode][i] * size * sr / 1000.)) +
                             17 * (int)(seed % 7);
                if (length % 2 == 0)
                    ++length;
                lengths[(size_t)c][(size_t)i] = length;
                lines[(size_t)c][(size_t)i].prepare(length + 2);
            }
            pre[(size_t)c].prepare((int)std::ceil(sr * .5) + 2);
            early[(size_t)c].prepare((int)std::ceil(sr * .3) + 2);
            for (int i = 0; i < stages; ++i)
                diffusers[(size_t)c][(size_t)i].prepare(diffuserLengths[(size_t)i] + 2);
        }
    }
    void tick(const Frame &input, Frame &wet, const Parameters &p) override
    {
        auto &v = p.values;
        if (controlPhase++ % 32 == 0)
        {
            lpA = v[Damping] >= 40000 ? 0 : std::exp(-2 * pi * std::min(v[Damping], rate * .49) / rate);
            attackA = std::exp(-1 / (rate * v[Attack] / 1000.));
            releaseA = std::exp(-1 / (rate * v[Release] / 1000.));
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < 8; ++i)
                    gains[(size_t)c][(size_t)i] =
                        std::pow(10., -3 * lengths[(size_t)c][(size_t)i] / (rate * v[Decay]));
        }
        for (int c = 0; c < channels; ++c)
        {
            auto channel = (size_t)c;
            double x = fractional(pre[channel], input[channel], v[Predelay] * rate / 1000.);
            pre[channel].push(input[channel]);
            constexpr double earlyGains[4] = {.6, -.35, .25, .18};
            double earlyOut = 0;
            for (int i = 0; i < 4; ++i)
                earlyOut += early[channel].read(earlyLengths[(size_t)i]) * earlyGains[i];
            early[channel].push(x);
            double diffuse = x;
            for (int i = 0; i < stages; ++i)
            {
                auto &line = diffusers[channel][(size_t)i];
                auto delayed = line.read(diffuserLengths[(size_t)i]);
                auto stored = diffuse + v[Diffusion] * delayed;
                line.push(stored);
                diffuse = delayed - v[Diffusion] * stored;
            }
            diffuse = x + (diffuse - x) * (v[Diffusion] / .75);
            std::array<double, 8> scatter{};
            double late = 0;
            for (int i = 0; i < 8; ++i)
            {
                auto at = (size_t)i;
                auto y = lines[channel][at].read(lengths[channel][at]);
                late += y * (i % 2 ? -inverseRoot8 : inverseRoot8);
                damping[channel][at] = lpA * damping[channel][at] + (1 - lpA) * y;
                scatter[at] = damping[channel][at] * gains[channel][at];
            }
            for (int stride = 1; stride < 8; stride *= 2)
                for (int start = 0; start < 8; start += stride * 2)
                    for (int i = 0; i < stride; ++i)
                    {
                        auto a = scatter[(size_t)(start + i)], b = scatter[(size_t)(start + i + stride)];
                        scatter[(size_t)(start + i)] = a + b;
                        scatter[(size_t)(start + i + stride)] = a - b;
                    }
            for (int i = 0; i < 8; ++i)
                lines[channel][(size_t)i].push((diffuse + scatter[(size_t)i]) * inverseRoot8);
            double level = std::abs(input[channel]);
            auto coefficient = level > envelope[channel] ? attackA : releaseA;
            envelope[channel] = coefficient * envelope[channel] + (1 - coefficient) * level;
            auto reduction =
                v[Duck] *
                std::clamp((20 * std::log10(std::max(1.e-12, envelope[channel])) - v[Threshold]) / 24., 0.,
                           1.);
            wet[channel] =
                ((1 - v[EarlyLate]) * earlyOut + v[EarlyLate] * late) * std::pow(10., -reduction / 20.);
        }
        if (left >= 0 && right >= 0 && v[Width] != 1)
        {
            auto mid = (wet[(size_t)left] + wet[(size_t)right]) * .5;
            wet[(size_t)left] = mid + (wet[(size_t)left] - mid) * v[Width];
            wet[(size_t)right] = mid + (wet[(size_t)right] - mid) * v[Width];
        }
    }
};
