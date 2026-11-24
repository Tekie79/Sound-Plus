#include "AutomationMidi.h"
namespace sp
{
bool AutomationMidiInput::open(const juce::String &identifier, bool notes)
{
    includeNotes = notes;
    input = juce::MidiInput::openDevice(identifier, this);
    if (!input)
        return false;
    input->start();
    return true;
}
AutomationMidiInput::~AutomationMidiInput()
{
    if (input)
        input->stop();
}
void AutomationMidiInput::handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m)
{
    const bool note = includeNotes && m.isNoteOnOrOff();
    if (!note && (!m.isController() || m.getControllerNumber() >= 120))
        return;
    auto w = write.load(std::memory_order_relaxed);
    if (w - read.load(std::memory_order_acquire) >= capacity)
    {
        overflow = true;
        return;
    }
    queue[w % capacity] = {m.getChannel(), note ? m.getNoteNumber() : m.getControllerNumber(),
                           note ? (m.isNoteOff() ? 0 : (int)m.getVelocity()) : m.getControllerValue(), note};
    write.store(w + 1, std::memory_order_release);
}
void AutomationMidiInput::drain(const std::function<void(CC)> &fn)
{
    auto r = read.load(std::memory_order_relaxed), w = write.load(std::memory_order_acquire);
    while (r != w)
    {
        auto e = queue[r++ % capacity];
        read.store(r, std::memory_order_release);
        fn(e);
    }
}
std::optional<double> ControllerPickup::move(const AutomationMidiBinding &b, const AutomationParameter &p,
                                             int raw, double current)
{
    if (raw < 0 || raw > 127 || b.high <= b.low)
        return {};
    int v = b.invert ? 127 - raw : raw;
    const double n = p.logarithmic ? std::log(std::clamp(current, p.minimum, p.maximum) / p.minimum) /
                                         std::log(p.maximum / p.minimum)
                                   : (current - p.minimum) / (p.maximum - p.minimum);
    double wanted = (n - b.low) * 127. / (b.high - b.low);
    if (!acquired)
        acquired = !b.pickup || std::abs(v - wanted) <= b.hysteresis ||
                   (previous >= 0 && (previous - wanted) * (v - wanted) <= 0);
    previous = v;
    if (!acquired || last == v || (last >= 0 && std::abs(v - last) < b.hysteresis && v != 0 && v != 127))
        return {};
    last = v;
    double u = std::lerp(b.low, b.high, v / 127.);
    double value =
        p.logarithmic ? p.minimum * std::pow(p.maximum / p.minimum, u) : std::lerp(p.minimum, p.maximum, u);
    if (p.discrete)
    {
        auto step = p.normalizedStep > 0 ? p.normalizedStep : 1.;
        value = std::round(value / step) * step;
    }
    return std::clamp(value, p.minimum, p.maximum);
}
} // namespace sp
