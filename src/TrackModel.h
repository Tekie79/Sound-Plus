#pragma once
#include "DesignModel.h"
#include <set>
namespace sp
{
inline constexpr int maxDesignTracks = 64, maxAutomationPoints = 8192;
struct AutomationParameter
{
    AutomationTarget target;
    juce::String group, name, units;
    double minimum = 0, maximum = 1, base = 0;
    bool logarithmic = false, discrete = false, writable = true;
    std::optional<HostedAutomationBinding> hostedBinding;
    int nativeIndex = -1;
    uint64_t hostedToken = 0;
    double normalizedStep = 0;
    juce::String unavailable;
};
DesignTrack *findTrack(DesignMix &, const juce::String &);
const DesignTrack *findTrack(const DesignMix &, const juce::String &);
DesignLayer *findClip(DesignMix &, const juce::String &);
const DesignLayer *findClip(const DesignMix &, const juce::String &);
InsertChain *ownerInserts(DesignMix &, const juce::String &);
void attachClip(DesignDocument &, DesignLayer &, juce::String owner = {});
void rebaseAutomation(DesignMix &, int64_t first);
void splitClip(DesignDocument &, const juce::String &, int64_t);
void copyClip(DesignDocument &, const juce::String &, const juce::String &, int64_t);
void enableTrackSchema(DesignMix &);
juce::String addTrack(DesignMix &, TrackKind, const juce::String &parent = {});
void reparentTrack(DesignMix &, const juce::String &, const juce::String &, int position = -1);
void routeTrack(DesignMix &, const juce::String &, const juce::String &);
void deleteTrack(DesignMix &, const juce::String &); // Folders unwrap; nonempty routed buses reject.
std::vector<juce::String> visibleTracks(const DesignMix &);
ChannelLayout trackOutputLayout(const DesignMix &, const DesignTrack &);
bool trackPanSupported(const DesignMix &, const DesignTrack &);
ChannelMatrix trackMatrix(const DesignTrack &, const ChannelLayout &, double pan, double azimuth,
                          double width);
bool trackAudible(const DesignMix &, const juce::String &);
void validateTracks(const DesignMix &, double);
juce::var automationLaneJson(const AutomationLane &);
AutomationLane automationLaneFromJson(const juce::var &);
juce::var tracksToJson(const DesignMix &);
void tracksFromJson(DesignMix &, const juce::var &);
bool hasHostedAutomation(const DesignMix &);
bool hasHostedAutomation(const DesignDocument &);
std::vector<AutomationParameter> automationTargets(const DesignMix &, const juce::String &owner);
AutomationLane *automationLane(DesignMix &, const juce::String &);
const AutomationLane *automationLane(const DesignMix &, const juce::String &);
AutomationCurve *automationCurve(AutomationLane &, const AutomationTarget &, bool create = false);
const AutomationCurve *automationCurve(const AutomationLane &, const AutomationTarget &);
double automationValue(const AutomationCurve &, const AutomationParameter &, int64_t frame);
void addAutomationPoint(DesignMix &, const juce::String &, const AutomationTarget &, int64_t, double,
                        double rate);
void writeTouchPass(DesignMix &, const juce::String &, const AutomationTarget &,
                    const std::vector<AutomationPoint> &, int64_t end, double rate,
                    std::optional<double> returnValue = {});
// Typed prepared bindings; snapshots are built on the decoder/graph worker.
struct CompiledAutomation
{
    struct Binding
    {
        AutomationParameter parameter;
        AutomationCurve curve;
    };
    std::vector<Binding> bindings;
    explicit CompiledAutomation(const DesignMix &, const juce::String &);
    double value(const AutomationTarget &, double base, int64_t frame) const;
    int untilBoundary(int64_t frame, int maximum) const;
    int64_t segmentFrame(int64_t) const;
    std::vector<HostedParameterEvent> hostedEvents(const HostedRack &, int64_t inputFrame, int frames,
                                                   int latency, double rate = 48000) const;
};
} // namespace sp
