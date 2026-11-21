// A3: pause real discovery after its observed candidates, before publication.
struct PausedDiscovery
{
    std::promise<void> ready, release;
    std::future<var> result;
    PausedDiscovery(const File &file, const File &helper, std::vector<File> roots)
    {
        auto gate = release.get_future();
        result =
            std::async(std::launch::async,
                       [this, file, helper, roots, gate = std::move(gate)]() mutable
                       {
                           PluginWorker child(helper);
                           PluginCatalog catalog(
                               file, {},
                               [&]
                               {
                                   ready.set_value();
                                   if (gate.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
                                       throw std::runtime_error("QA publication gate timed out");
                               });
                           return catalog.scan(child, roots, false, [] { return false; });
                       });
        require(ready.get_future().wait_for(std::chrono::seconds(15)) == std::future_status::ready,
                "Discovery reaches controlled publication barrier");
    }
    var finish()
    {
        release.set_value();
        return result.get();
    }
};
auto containsPath = [](const var &registry, const File &path)
{
    for (auto &entry : *registry["entries"].getArray())
        if (entry["path"].toString() == path.getFullPathName())
            return true;
    return false;
};
for (bool secondCustom : {false, true})
    for (bool olderFirst : {false, true})
    {
        auto work = root.getChildFile("coverage-" + String((int)secondCustom) + "-" + String((int)olderFirst));
        work.createDirectory();
        auto defaults = work.getChildFile("Default"), custom = work.getChildFile("Custom"),
             other = work.getChildFile("Other");
        defaults.createDirectory();
        custom.createDirectory();
        other.createDirectory();
        auto base = defaults.getChildFile("Base.vst3"), a = custom.getChildFile("A.vst3"),
             b = other.getChildFile("B.vst3");
        require(fixture.copyDirectoryTo(base) && fixture.copyDirectoryTo(a) && stereo.copyDirectoryTo(b),
                "Distinct outside-default custom fixtures");
        require(!a.isAChildOf(defaults) && !b.isAChildOf(defaults),
                "Custom discovery roots are not covered by Default");
        auto file = work.getChildFile("registry.json");
        PluginCatalog catalog(file);
        catalog.scan(child, {defaults}, false, cancel);
        PausedDiscovery older(file, helper, {custom});
        PausedDiscovery newer(file, helper, secondCustom ? std::vector<File>{other} : std::vector<File>{});
        if (olderFirst)
        {
            older.finish();
            newer.finish();
        }
        else
        {
            newer.finish();
            older.finish();
        }
        auto final = catalog.snapshot();
        require(containsPath(final, a) && containsPath(final, base) &&
                    (!secondCustom || containsPath(final, b)),
                "Both publication orders preserve additions outside the other scan's discovery coverage");
        require(final["roots"].size() == (secondCustom ? 3 : 2),
                "Concurrent additions retain all configured roots");
        evidence.add(final);
    }
{
    auto work = root.getChildFile("presence-monotonic");
    work.createDirectory();
    auto directory = work.getChildFile("Default");
    directory.createDirectory();
    auto a = directory.getChildFile("A.vst3"), parked = work.getChildFile("Parked.vst3");
    require(fixture.copyDirectoryTo(a), "Presence fixture copied");
    auto file = work.getChildFile("registry.json");
    PluginCatalog catalog(file);
    auto initial = catalog.scan(child, {directory}, false, cancel);
    PausedDiscovery cachedA2(file, helper, {});
    require(a.moveFileTo(parked), "Remove generated fixture without changing its fingerprint");
    PausedDiscovery deletionD3(file, helper, {});
    require(parked.moveFileTo(a), "Restore exactly the same fixture before newer presence scan");
    auto newer = catalog.scan(child, {}, false, cancel);
    require((int)newer["cacheHits"] == 1, "N4 reuses equal-version cached A with newer presence");
    auto n = (int64)newer["entries"][0]["registryPresenceOrdinal"];
    auto old = cachedA2.finish();
    require((int64)old["entries"][0]["registryPresenceOrdinal"] >= n,
            "A2 cannot lower N4 presence while replacing equal-version cached data");
    auto final = deletionD3.finish();
    require(containsPath(final, a) && (int64) final["entries"][0]["registryPresenceOrdinal"] >= n,
            "Stale D3 deletion cannot remove N4 restored-file presence");
    require(a.moveFileTo(parked), "Prepare legitimate newer deletion");
    auto removed = catalog.scan(child, {}, false, cancel);
    require(!containsPath(removed, a), "An actually covering newer scan still removes a missing plugin");
    evidence.add(final);
}
