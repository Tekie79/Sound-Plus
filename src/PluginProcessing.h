#pragma once
#include "DesignModel.h"
#include "SerialWorker.h"
namespace sp
{
// Versioned independently of stock recipes/takes. No plugin code is loaded here.
juce::String pluginFingerprint(const juce::File &);
juce::String processingHash(const juce::File &);
std::vector<juce::File> defaultVst3Locations();
juce::String pluginEffectGroup(const juce::var &);
juce::String pluginFormatLabel(const juce::var &);
inline constexpr int pluginProbePolicy = 3;
juce::String pluginParameterSchemaSignature(const juce::var &);
juce::StringArray pluginDisplayNames(const juce::Array<juce::var> &);
juce::String pluginCandidateSelectionKey(const juce::var &);
juce::Array<juce::var> pluginCandidateRows(const juce::var &, const ChannelLayout &, const juce::String &,
                                           bool attentionOnly);
void saveProcessingJson(const juce::File &, const juce::var &);
juce::var loadProcessingJson(const juce::File &);
struct PluginInput
{
    Asset source;
    int64_t first = 0, last = -1;
    ChannelLayout layout;
    std::vector<int> inputs;
    bool mean = false;
    juce::String scope = "Original source/range";
};
juce::var pluginInputJson(const PluginInput &);
PluginInput pluginInputFromJson(const juce::var &);
// Produces a verified finite 32-bit float WAV. Explicit interpretations required.
juce::var stagePluginInput(const PluginInput &, const juce::File &, const std::function<bool()> &);
class PluginWorker
{
  public:
    explicit PluginWorker(juce::File helper) : executable(std::move(helper)) {}
    juce::var call(
        juce::var, const juce::File &workspace, int timeoutMs,
        const std::function<bool()> &cancelled = [] { return false; });
    juce::File executable, lastRequest;
    juce::var lastDiagnostics;
};
class PluginCatalog
{
  public:
    explicit PluginCatalog(juce::File storage, std::function<void()> captured = {})
        : file(std::move(storage)), afterSnapshot(std::move(captured))
    {
    }
    // Worker-thread only. One child per changed candidate. Failed unchanged candidates stay quarantined.
    juce::var scan(PluginWorker &, const std::vector<juce::File> &, bool retry, const std::function<bool()> &,
                   const std::function<void(juce::var)> & = {}, bool registeredAudioUnits = false,
                   const juce::String &targetPath = {});
    juce::var snapshot() const;

  private:
    juce::File file;
    std::function<void()> afterSnapshot;
};
// Derive delivery from authoritative PCM; never calls a plugin. Atomic directory publication.
juce::var publishPluginCopy(const juce::var &result, const juce::File &folder, bool normalize, bool split,
                            const std::function<bool()> &, const std::function<void()> &afterStaging = {});
} // namespace sp
