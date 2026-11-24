#include "AcousticMetadata.h"
#include "ModelPack.h"
#include <NativePresetsData.h>
#include <Accelerate/Accelerate.h>
#include <juce_cryptography/juce_cryptography.h>

namespace sp
{
namespace
{
using Vec = std::array<float, 512>;
float dot(const Vec &a, const Vec &b)
{
    float x;
    vDSP_dotpr(a.data(), 1, b.data(), 1, &x, a.size());
    return x;
}
Vec unpack(const juce::var &value)
{
    juce::MemoryOutputStream out;
    Vec v;
    if (!juce::Base64::convertFromBase64(out, value.toString()) || out.getDataSize() != sizeof(v))
        throw std::runtime_error("Invalid category prototypes");
    std::memcpy(v.data(), out.getData(), sizeof(v));
    auto n = dot(v, v);
    if (!std::isfinite(n) || std::abs(n - 1) > 0.001f)
        throw std::runtime_error("Invalid category prototype norm");
    return v;
}
struct Bank
{
    std::vector<AcousticPrototype> categories;
    Vec silence;
    Bank()
    {
        int size = 0;
        auto bytes = NativePresetData::getNamedResource("acousticucsv1_json", size);
        auto tree = bytes ? juce::JSON::parse(juce::String::fromUTF8(bytes, size)) : juce::var{};
        if ((int)tree["version"] != 1 || tree["model"].toString() != modelId() ||
            tree["preprocessing"].toString() != preprocessingId() || !tree["categories"].isArray())
            throw std::runtime_error("Category embedding space unavailable");
        silence = unpack(tree["silence"]);
        for (auto &r : *tree["categories"].getArray())
        {
            auto name = r["category"].toString();
            if (name == "ARCHIVED" || name == "UNCATEGORIZED")
                continue;
            categories.push_back({name, unpack(r["vector"])});
        }
    }
};
} // namespace
const juce::String &acousticPolicy()
{
    static const auto key = []
    {
        int bytes = 0;
        auto raw = NativePresetData::getNamedResource("acousticucsv1_json", bytes);
        return "ucs-acoustic/v1/" + juce::SHA256(raw, (size_t)bytes).toHexString();
    }();
    return key;
}
bool currentAcousticSource(const Asset &expected, const Asset &a)
{
    FileStamp s;
    return a.online && a.state == "ready" && a.deliverable() && a.id == expected.id &&
           a.revision == expected.revision && a.path == expected.path && a.samples == expected.samples &&
           a.sampleRate == expected.sampleRate && fileStamp(juce::File(a.path), s) &&
           s.size == expected.size && s.modified == expected.modified && s.inode == expected.inode;
}
AcousticResult rankAcoustic(const std::vector<StoredRegion> &windows,
                            const std::vector<AcousticPrototype> &bank, const Vec &silence,
                            const std::function<bool()> &cancel)
{
    std::vector<const StoredRegion *> usable;
    for (auto &w : windows)
    {
        auto n = dot(w.vector, w.vector);
        if (std::isfinite(n) && std::abs(n - 1) < 0.01f && dot(w.vector, silence) < 0.97f)
            usable.push_back(&w);
    }
    if (usable.empty())
        return {"No suggestion (quiet or unclear audio)", {}};
    AcousticResult r;
    for (auto &p : bank)
    {
        if (cancel())
            return {"Cancelled", {}};
        std::array<float, 3> top{-2, -2, -2};
        MatchRegion moment;
        for (auto *w : usable)
        {
            float s = dot(p.vector, w->vector);
            if (s > top[0])
                moment = {w->first, w->last, s, 1};
            for (auto &v : top)
                if (s > v)
                    std::swap(s, v);
        }
        const int k = std::min<int>(usable.size(), 3);
        float mean = 0;
        for (int i = 0; i < k; ++i)
            mean += top[i] / k;
        r.choices.push_back({p.category, 0.65f * top[0] + 0.35f * mean, moment});
    }
    std::sort(r.choices.begin(), r.choices.end(),
              [](auto &a, auto &b) {
                  return a.similarity != b.similarity ? a.similarity > b.similarity : a.category < b.category;
              });
    if (r.choices.empty() || r.choices.front().similarity < 0.32f ||
        (r.choices.size() > 1 && r.choices[0].similarity - r.choices[1].similarity < 0.03f))
        return {"No suggestion (ambiguous audio)", {}};
    const float floor = std::max(0.32f, r.choices.front().similarity - 0.10f);
    std::erase_if(r.choices, [floor](auto &c) { return c.similarity < floor; });
    if (r.choices.size() > 5)
        r.choices.resize(5);
    r.status = "Suggested from audio (unverified)";
    return r;
}
AcousticResult acousticSuggestions(const juce::File &database, const Asset &a, bool ready,
                                   const std::function<bool()> &cancel)
{
    if (cancel())
        return {"Cancelled", {}};
    if (!ready)
        return {"Local model unavailable", {}};
    try
    {
        Catalog db(database, true);
        auto valid = [&] { return currentAcousticSource(a, db.asset(a.id)); };
        if (!a.online || !valid())
            return {"Recording offline or changed", {}};
        if (db.describeMetadata(a.id)["fields"].hasProperty("category"))
            return {"Category already assigned", {}};
        auto windows = db.acousticWindows(a, modelId(), preprocessingId(), cancel);
        if (cancel())
            return {"Cancelled", {}};
        if (windows.empty())
            return {"Audio analysis incomplete", {}};
        static const Bank bank;
        auto result = rankAcoustic(windows, bank.categories, bank.silence, cancel);
        auto dismissed = db.dismissedAcoustic(a, acousticPolicy());
        std::erase_if(result.choices, [&](auto &c) { return dismissed.contains(c.category); });
        if (cancel())
            return {"Cancelled", {}};
        if (!valid())
            return {"Recording offline or changed", {}};
        if (result.choices.empty() && result.status.startsWith("Suggested"))
            result.status = "No remaining suggestions";
        return result;
    }
    catch (const std::exception &e)
    {
        return {juce::String::fromUTF8(e.what()), {}};
    }
}
} // namespace sp
