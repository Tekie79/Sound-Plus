#include "MetadataFields.h"
#include <NativePresetsData.h>
namespace sp
{
void addEmbeddedTag(juce::StringPairArray &tags, const juce::String &name, const juce::String &value,
                    const juce::String &qualified)
{
    if (tags.size() >= 512 || name.isEmpty() || name.length() > 160)
        return;
    if (qualified != name && qualified.length() <= 160 && value.isNotEmpty())
        tags.set(qualified, value);
    if (value.isEmpty())
        return;
    auto existing = tags.getValue(name, {});
    if (existing.isEmpty())
        tags.set(name, value);
    else if (existing != value)
    {
        auto other = tags.getValue(qualified, {});
        if (other != value)
            tags.set(qualified, other.isEmpty() ? value : other + "\n" + value);
    }
}
void addXmlTags(const juce::XmlElement &xml, const juce::String &path, juce::StringPairArray &tags, int depth)
{
    if (depth == 32)
        return;
    auto qualified = path + "." + xml.getTagName();
    bool hasChildren = false;
    for (auto *child : xml.getChildIterator())
        if (!child->isTextElement())
        {
            hasChildren = true;
            addXmlTags(*child, qualified, tags, depth + 1);
        }
    if (!hasChildren)
        addEmbeddedTag(tags, xml.getTagName(), xml.getAllSubText().trim(), qualified);
    for (int i = 0; i < xml.getNumAttributes(); ++i)
    {
        auto name = qualified + "@" + xml.getAttributeName(i);
        addEmbeddedTag(tags, name, xml.getAttributeValue(i), name);
    }
}
// The JUCE WAV reader recognizes standard tags and ASWG fields. Read other textual
// RIFF INFO and iXML fields as well, skipping sample data and bounding text reads.
void addWaveTags(const juce::File &file, juce::StringPairArray &tags)
{
    juce::FileInputStream in(file);
    if (!in.openedOk() || in.getTotalLength() < 12)
        return;
    auto fourcc = [](const char *name) { return (uint32_t)juce::ByteOrder::littleEndianInt(name); };
    auto container = (uint32_t)in.readInt();
    in.skipNextBytes(4);
    if ((container != fourcc("RIFF") && container != fourcc("RF64")) ||
        (uint32_t)in.readInt() != fourcc("WAVE"))
        return;
    int64_t rf64DataSize = -1;
    int textBudget = 1024 * 1024;
    const auto readText = [&](int64_t size)
    {
        if (size <= 0 || size > textBudget)
            return juce::String();
        juce::MemoryBlock bytes;
        const auto read = (int)in.readIntoMemoryBlock(bytes, (ssize_t)size);
        textBudget -= read;
        return read > 0 ? juce::String::fromUTF8(static_cast<const char *>(bytes.getData()), read).trim()
                        : juce::String();
    };
    const auto fileEnd = in.getTotalLength();
    while (in.getPosition() <= fileEnd - 8 && textBudget > 0)
    {
        auto type = (uint32_t)in.readInt();
        int64_t size = (uint32_t)in.readInt();
        const auto start = in.getPosition();
        if (container == fourcc("RF64") && type == fourcc("data") && size == UINT32_MAX)
            size = rf64DataSize;
        if (size < 0 || size > fileEnd - start)
            break;
        const auto end = start + size;
        if (type == fourcc("ds64") && size >= 16)
        {
            in.skipNextBytes(8);
            rf64DataSize = in.readInt64();
        }
        else if (type == fourcc("iXML") || type == fourcc("axml") || type == fourcc("_PMX"))
        {
            auto xmlText = readText(size);
            if (xmlText.isNotEmpty() && !xmlText.containsIgnoreCase("<!DOCTYPE"))
                if (auto xml = juce::parseXML(xmlText))
                    addXmlTags(*xml, "XML", tags, 0);
        }
        else if (type == fourcc("LIST") && size >= 4)
        {
            const auto kind = (uint32_t)in.readInt();
            if (kind == fourcc("INFO") || kind == fourcc("info"))
                while (in.getPosition() <= end - 8 && textBudget > 0)
                {
                    char name[5]{};
                    if (in.read(name, 4) != 4)
                        break;
                    const int64_t length = (uint32_t)in.readInt();
                    const auto next = in.getPosition() + length + (length & 1);
                    if (next > end)
                        break;
                    auto key = juce::String::fromUTF8(name, 4).trim();
                    addEmbeddedTag(tags, key, readText(length), "INFO." + key);
                    if (!in.setPosition(next))
                        break;
                }
        }
        if (!in.setPosition(end + (size & 1)))
            break;
    }
}
juce::StringArray editableMetadataKeys()
{
    return {"description", "keywords", "category", "subcategory", "catid",
            "creator",     "location", "mic",      "date",        "tags"};
}
juce::var embeddedFields(const juce::StringPairArray &tags)
{
    auto *o = new juce::DynamicObject;
    juce::var result(o);
    int total = 0;
    auto field = [&](const char *k, std::initializer_list<const char *> keys)
    {
        for (auto key : keys)
            if (auto v = tags.getValue(key, {}).trim().substring(0, 4096); v.isNotEmpty())
            {
                o->setProperty(k, v);
                break;
            }
    };
    field("description", {"description", "bwav description", "ICMT", "comment"});
    field("category", {"category", "ICAT", "genre", "IGNR"});
    field("subcategory", {"subcategory", "sub_category"});
    field("catid", {"catid", "cat_id"});
    field("creator", {"creator", "artist", "IART", "bwav originator", "originator"});
    field("keywords", {"keywords", "IKEY"});
    field("location", {"location", "locationname"});
    field("mic", {"microphone", "micmodel", "microphoneModel", "mic"});
    field("date", {"date", "ICRD", "bwav origination date", "originationdate"});
    auto *raw = new juce::DynamicObject;
    juce::var data(raw);
    for (auto &key : tags.getAllKeys())
    {
        if (raw->getProperties().size() >= 512)
            break;
        auto value = tags.getValue(key, {}).substring(0, 4096);
        if (value.isEmpty() || key.length() > 160 || key.isEmpty())
            continue;
        total += value.getNumBytesAsUTF8();
        if (total > 128 * 1024)
            break;
        raw->setProperty(key, value);
    }
    o->setProperty("raw", data);
    return result;
}
static const juce::var &categoryTable()
{
    static const auto taxonomy = []
    {
        int bytes = 0;
        auto *resource = NativePresetData::getNamedResource("ucs8_2_1_json", bytes);
        return resource ? juce::JSON::parse(juce::String::fromUTF8(resource, bytes)) : juce::var{};
    }();
    return taxonomy;
}
juce::var namedFields(const juce::String &filename)
{
    auto &taxonomy = categoryTable();
    auto *o = new juce::DynamicObject;
    juce::var v(o);
    if (!filename.containsChar('_'))
        return v;
    auto id = filename.upToFirstOccurrenceOf("_", false, false).upToFirstOccurrenceOf("-", false, false);
    auto row = taxonomy[juce::Identifier(id)];
    if (row.isArray() && row.size() == 3)
    {
        o->setProperty("catid", id);
        o->setProperty("category", row[0]);
        o->setProperty("subcategory", row[1]);
        o->setProperty("aliases", row[2]);
    }
    return v;
}
bool isMetadataQuery(const juce::String &q)
{
    if (!q.containsChar(':'))
        return false;
    static const auto fields = []
    {
        auto f = editableMetadataKeys();
        f.addArray(
            {"meta", "channels", "min", "max", "type", "generated", "missing", "favorite", "review", "tag"});
        return f;
    }();
    for (auto &token : juce::StringArray::fromTokens(q, " \t", "\""))
        if (token.containsChar(':') &&
            fields.contains(token.upToFirstOccurrenceOf(":", false, false).toLowerCase()))
            return true;
    return false;
}
} // namespace sp

namespace sp
{
juce::String metadataTokens(const juce::String &q, bool filters)
{
    juce::StringArray parts;
    for (auto token : juce::StringArray::fromTokens(q, " \t", "\""))
        if (isMetadataQuery(token) == filters)
            parts.add(token);
    return parts.joinIntoString(" ");
}
juce::String suggestedCategory(const juce::String &name)
{
    // A suggestion requires an exact word shared exclusively by one top-level UCS category.
    static const auto lookup = []
    {
        std::map<juce::String, juce::String> matches;
        if (auto *o = categoryTable().getDynamicObject())
            for (auto &p : o->getProperties())
            {
                auto row = p.value;
                if (row.size() != 3)
                    continue;
                auto cat = row[0].toString();
                auto terms = juce::StringArray::fromTokens(row[2].toString(), ",", "");
                terms.add(cat);
                terms.add(row[1].toString());
                for (auto term : terms)
                {
                    term = term.trim().toLowerCase();
                    if (term.length() < 4)
                        continue;
                    auto [it, added] = matches.emplace(term, cat);
                    if (!added && it->second != cat)
                        it->second.clear();
                }
            }
        return matches;
    }();
    juce::String suggestion;
    for (auto word : juce::StringArray::fromTokens(name.toLowerCase(), " _.0123456789-", ""))
    {
        auto at = lookup.find(word);
        if (at != lookup.end() && at->second.isNotEmpty())
        {
            if (suggestion.isNotEmpty() && suggestion != at->second)
                return {};
            suggestion = at->second;
        }
    }
    return suggestion;
}
} // namespace sp
