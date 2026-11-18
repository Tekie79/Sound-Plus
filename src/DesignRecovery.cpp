#include "DesignRecovery.h"
#include <fcntl.h>
#include <unistd.h>

namespace sp
{
namespace
{
void synchronize(const juce::File &file)
{
    auto fd = ::open(file.getFullPathName().toRawUTF8(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("Cannot open recovery state for synchronization");
    auto result = ::fsync(fd);
    ::close(fd);
    if (result != 0)
        throw std::runtime_error("Cannot synchronize recovery state");
}
void publishJournal(const juce::File &file, const juce::var &state)
{
    juce::TemporaryFile stage(file, juce::TemporaryFile::useHiddenFile);
    if (!stage.getFile().replaceWithText(juce::JSON::toString(state, true)))
        throw std::runtime_error("Cannot write recovery association; the design snapshot is retained");
    synchronize(stage.getFile());
    if (!stage.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot publish recovery association; the design snapshot is retained");
    synchronize(file.getParentDirectory());
}
} // namespace
DesignRecoveryStore::DesignRecoveryStore(juce::File folder) : directory(std::move(folder)) {}

DesignRecovery DesignRecoveryStore::readUnlocked(const juce::File &snapshot)
{
    DesignRecovery result;
    result.snapshot = snapshot;
    result.document = loadDesign(snapshot);
    auto state = juce::JSON::parse(snapshot.getSiblingFile(snapshot.getFileNameWithoutExtension() + ".json"));
    if (state["id"].toString() == result.document.id &&
        (int64_t)state["revision"] == result.document.revision)
    {
        result.savedFile = juce::File(state["savedFile"].toString());
        result.dirty = state["state"].toString() == "recoverable";
        result.updated = (int64_t)state["updated"];
    }
    else
        result.updated = snapshot.getLastModificationTime().toMilliseconds();
    if (result.savedFile.existsAsFile())
    {
        try
        {
            auto saved = loadDesign(result.savedFile);
            if (saved.id == result.document.id && saved.revision >= result.document.revision)
            {
                result.document = std::move(saved);
                result.dirty = false;
            }
        }
        catch (...)
        {
        } // The recoverable snapshot remains usable if the named save is damaged.
    }
    return result;
}

DesignRecovery DesignRecoveryStore::read(const juce::File &file)
{
    std::lock_guard lock(mutex);
    return readUnlocked(file);
}

void DesignRecoveryStore::retain(const DesignDocument &document, const juce::File &saved, bool dirty)
{
    std::lock_guard lock(mutex);
    auto snapshot = directory.getChildFile(document.id + ".soundplus");
    if (snapshot.existsAsFile())
    {
        auto previous = readUnlocked(snapshot);
        if (previous.document.revision > document.revision ||
            (previous.document.revision == document.revision && !previous.dirty && dirty))
            return;
    }
    saveDesignAtomic(snapshot, document);
    auto *state = new juce::DynamicObject();
    state->setProperty("version", 1);
    state->setProperty("id", document.id);
    state->setProperty("revision", (juce::int64)document.revision);
    state->setProperty("savedFile", saved.getFullPathName());
    state->setProperty("state", dirty ? "recoverable" : saved == juce::File() ? "clean" : "saved");
    lastUpdate = std::max(lastUpdate + 1, juce::Time::currentTimeMillis());
    state->setProperty("updated", (juce::int64)lastUpdate);
    publishJournal(snapshot.withFileExtension(".json"), juce::var(state));
}

std::vector<DesignRecovery> DesignRecoveryStore::entries()
{
    std::lock_guard lock(mutex);
    std::vector<DesignRecovery> result;
    for (auto &file : directory.findChildFiles(juce::File::findFiles, false, "*.soundplus"))
    {
        if (file.getFileName() == "recovery.soundplus")
            continue;
        try
        {
            result.push_back(readUnlocked(file));
        }
        catch (...)
        {
        }
    }
    // Import the old singleton only when no per-document journal has been published.
    auto legacy = directory.getChildFile("recovery.soundplus");
    if (result.empty() && legacy.existsAsFile())
        try
        {
            result.push_back(readUnlocked(legacy));
        }
        catch (...)
        {
        }
    auto active = juce::JSON::parse(directory.getChildFile("active.json"))["id"].toString();
    std::sort(result.begin(), result.end(),
              [active](auto &a, auto &b)
              {
                  if ((a.document.id == active) != (b.document.id == active))
                      return a.document.id == active;
                  return a.updated > b.updated;
              });
    return result;
}
void DesignRecoveryStore::activate(const juce::String &id)
{
    std::lock_guard lock(mutex);
    directory.createDirectory();
    auto *state = new juce::DynamicObject();
    state->setProperty("version", 1);
    state->setProperty("id", id);
    publishJournal(directory.getChildFile("active.json"), juce::var(state));
}
} // namespace sp
