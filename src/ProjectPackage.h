#pragma once
#include "DesignModel.h"
#include <set>
namespace sp
{
struct PackageFile
{
    juce::File original;
    juce::String relative, sha256;
    FileStamp stamp;
    bool optional = false;
    juce::String derived, originalHash;
};
struct PackagePlan
{
    juce::var project;
    std::vector<PackageFile> files;
    int64_t uniqueBytes = 0;
};
using PackageProgress = std::function<void(int64_t, int64_t)>;
PackagePlan planPackage(const DesignDocument &, bool includeOutputs = false,
                        const std::function<bool()> &cancelled = {});
juce::File collectPackage(const PackagePlan &, const std::set<juce::String> &approved,
                          const juce::File &destination, const std::function<bool()> &cancelled = {},
                          const PackageProgress &progress = {});
DesignDocument openPackage(const juce::File &directory, const std::function<bool()> &cancelled = {});
} // namespace sp
