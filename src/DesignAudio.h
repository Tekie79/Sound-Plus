#pragma once
#include "DesignModel.h"
#include "TakeCapture.h"
#include <juce_audio_utils/juce_audio_utils.h>
namespace sp
{
// Owned by a decoder/render worker, never by the real-time callback.
class DesignGraph
{
  public:
    explicit DesignGraph(
        const DesignDocument &, const std::function<bool()> &cancelled = [] { return false; });
    ~DesignGraph();
    void render(juce::AudioBuffer<float> &, int offset, int frames);
    void
    seek(int64_t frame, const std::function<bool()> &cancelled = [] { return false; }, int64_t warmup = -1);
    void updateParameters(const juce::var &);
    bool squareTouch(const juce::var &);
    juce::var squareTouchReceipt(bool close = false);
    int64_t injectMidi(const juce::String &, MidiPacket);
    void midiPanic(const juce::String &);
    juce::var nativeMeters() const;
    juce::var mixerMeters() const;
    int layers() const;
    int64_t position() const;
    double rate() const;

  private:
    friend class DesignReader;
    DesignGraph(const DesignDocument &, const std::function<bool()> &, const DesignDocument *effectiveInput);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
class DesignReader
{
  public:
    DesignReader(
        const DesignDocument &, double outputRate, int64_t first = 0, int64_t last = -1,
        double tailSeconds = 0, const std::function<bool()> &cancelled = [] { return false; },
        int64_t warmup = -1);
    ~DesignReader();
    int read(
        juce::AudioBuffer<float> &, int frames,
        const std::function<bool()> &cancelled = [] { return false; });
    void updateParameters(const juce::var &);
    int64_t length() const;
    void updatePerformance(const PerformControls &, TakeCapture &);
    int64_t consumed() const;
    int64_t producedFrame() const;
    bool squareTouch(const juce::var &);
    juce::var squareTouchReceipt(bool close = false);
    int64_t endFrame() const;
    int64_t injectMidi(const juce::String &, MidiPacket);
    void midiPanic(const juce::String &);
    juce::var nativeMeters() const;
    juce::var mixerMeters() const;
    int layers() const;

  private:
    friend DesignOutput renderDesign(const DesignDocument &, const juce::File &, const DesignRenderSettings &,
                                     const std::function<bool()> &, const std::function<void(double)> &);
    // Only rendering can supply an already verified immutable input snapshot.
    DesignReader(const DesignDocument &, double, int64_t, int64_t, double, const std::function<bool()> &,
                 int64_t, const DesignDocument *effectiveInput);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
DesignOutput renderDesign(const DesignDocument &, const juce::File &folder, const DesignRenderSettings &,
                          const std::function<bool()> &cancelled,
                          const std::function<void(double)> &progress);
DesignOutput copyDesignOriginal(const DesignLayer &, const juce::File &folder, int64_t revision,
                                const std::function<bool()> &cancelled);
} // namespace sp
