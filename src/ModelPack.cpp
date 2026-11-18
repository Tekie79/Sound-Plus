#include "ModelPack.h"
#include "ModelManifestData.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
juce::var modelManifest()
{
    return juce::JSON::parse(juce::String::fromUTF8(BinaryData::clapmodel_json,
                                                 BinaryData::clapmodel_jsonSize));
}
juce::String modelId() { return modelManifest()["id"].toString(); }
juce::String preprocessingId() { return modelManifest()["preprocessing"].toString(); }
juce::File defaultModelDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Application Support/SoundPlus/Models").getChildFile(modelId());
}
bool verifyModelPack(const juce::File &folder, juce::String &error, const std::atomic<bool> *cancel)
{
    auto manifest = modelManifest();
    auto *files = manifest["files"].getArray();
    if (!files || files->isEmpty()) { error = "Model manifest is unavailable"; return false; }
    for (auto &v : *files)
    {
        if (cancel && cancel->load()) { error = "Model installation cancelled"; return false; }
        auto file = folder.getChildFile(v["name"].toString());
        if (!file.existsAsFile() || file.getSize() != (int64_t)v["bytes"])
        { error = "Missing or incomplete model file: " + file.getFileName(); return false; }
        auto stream = file.createInputStream();
        if (!stream || juce::SHA256(*stream).toHexString() != v["sha256"].toString())
        { error = "Model checksum failed: " + file.getFileName(); return false; }
    }
    error.clear();
    return true;
}
bool installModelPack(const juce::File &destination, const juce::File &offlineSource,
                      const std::atomic<bool> &cancel,
                      const std::function<void(int64_t, int64_t)> &progress, juce::String &error)
{
    auto manifest = modelManifest();
    auto stage = destination.getSiblingFile(destination.getFileName() + ".install-" + juce::Uuid().toString());
    struct Cleanup { juce::File path; ~Cleanup() { path.deleteRecursively(); } } cleanup{stage};
    if (!stage.createDirectory()) { error = "Cannot create model installation directory"; return false; }
    int64_t total = 0, done = 0;
    for (auto &v : *manifest["files"].getArray()) total += (int64_t)v["bytes"];
    for (auto &v : *manifest["files"].getArray())
    {
        std::unique_ptr<juce::InputStream> in;
        if (offlineSource != juce::File())
            in = offlineSource.getChildFile(v["name"].toString()).createInputStream();
        else
            in = juce::URL(v["url"].toString()).createInputStream(
                juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs(5000).withNumRedirectsToFollow(8));
        auto target = stage.getChildFile(v["name"].toString());
        auto out = target.createOutputStream();
        if (!in || !out) { error = "Cannot obtain model file: " + target.getFileName(); return false; }
        std::array<char, 65536> buffer{};
        int64_t bytes = 0;
        while (!in->isExhausted())
        {
            if (cancel) { error = "Model installation cancelled"; return false; }
            auto n = in->read(buffer.data(), (int)buffer.size());
            if (n <= 0) break;
            bytes += n;
            if (bytes > (int64_t)v["bytes"] || !out->write(buffer.data(), (size_t)n))
            { error = "Invalid or unwritable model download"; return false; }
            done += n;
            progress(done, total);
        }
        out->flush(); out.reset();
        if (bytes != (int64_t)v["bytes"]) { error = "Incomplete model download; retry installation"; return false; }
    }
    if (!verifyModelPack(stage, error, &cancel)) return false;
    stage.getChildFile("manifest.json").replaceWithText(juce::JSON::toString(manifest));
    if (destination.exists())
    {
        if (verifyModelPack(destination, error)) return true;
        auto quarantine=destination.getSiblingFile(destination.getFileName()+".invalid-"+juce::Uuid().toString());
        if (!destination.moveFileTo(quarantine)) { error="Cannot move the damaged model pack aside"; return false; }
        if (!stage.moveFileTo(destination))
        { quarantine.moveFileTo(destination); error="Cannot publish repaired model pack"; return false; }
        error.clear(); return true;
    }
    if (!stage.moveFileTo(destination)) { error = "Cannot publish verified model pack"; return false; }
    error.clear(); return true;
}
} // namespace sp
