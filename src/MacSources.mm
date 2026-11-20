#include "MacSources.h"
#include "Catalog.h"
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <CoreServices/CoreServices.h>
#include <sys/mount.h>
#include <filesystem>
namespace sp
{
juce::var powerAndThermalState()
{
    auto state=juce::var(new juce::DynamicObject());
    const char *names[]{"nominal","fair","serious","critical"};
    NSInteger thermal=NSProcessInfo.processInfo.thermalState;
    state.getDynamicObject()->setProperty("thermalState",juce::String(names[std::clamp<int>((int)thermal,0,3)]));
    if (@available(macOS 12.0,*)) state.getDynamicObject()->setProperty("lowPowerMode",(bool)NSProcessInfo.processInfo.lowPowerModeEnabled);
    return state;
}
bool localAiSupported()
{
    if (@available(macOS 13.3, *)) return true;
    return false;
}
std::vector<juce::String> robertaPieces(const juce::String &text)
{
    @autoreleasepool
    {
        static NSRegularExpression *regex = [[NSRegularExpression alloc]
            initWithPattern:@"'s|'t|'re|'ve|'m|'ll|'d| ?\\p{L}+| ?\\p{N}+| ?[^\\s\\p{L}\\p{N}]+|\\s+(?!\\S)|\\s+"
            options:0 error:nil]; // Owned for the process lifetime in this non-ARC translation unit.
        NSString *input = [NSString stringWithUTF8String:text.toRawUTF8()];
        std::vector<juce::String> out;
        for (NSTextCheckingResult *m in [regex matchesInString:input options:0 range:NSMakeRange(0, input.length)])
            out.emplace_back(juce::String::fromUTF8([[input substringWithRange:m.range] UTF8String]));
        return out;
    }
}
void makeAudioProcessBackground()
{
    // The helper must never compete with the Library window for keyboard focus.
    [[NSApplication sharedApplication] setActivationPolicy:NSApplicationActivationPolicyProhibited];
}
void configurePluginServiceApplication()
{
    // Native editor services have no restorable user windows. Their owner restores
    // exact processors/checkpoints; macOS must not offer stale helper windows.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"NSQuitAlwaysKeepsWindows":@NO,@"NSApplePersistenceIgnoreState":@YES}];
    [[NSApplication sharedApplication] setActivationPolicy:NSApplicationActivationPolicyAccessory];
}
void activateDesktopApplication()
{
    [[NSApplication sharedApplication] setActivationPolicy:NSApplicationActivationPolicyRegular];
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
}
VolumeLocation volumeLocation(const juce::File &file)
{
    @autoreleasepool
    {
        NSURL *url =
            [[NSURL fileURLWithPath:[NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()]]
                URLByResolvingSymlinksInPath];
        NSString *uuid = nil;
        [url getResourceValue:&uuid forKey:NSURLVolumeUUIDStringKey error:nil];
        // Foundation normalizes /private/tmp back to /tmp; use POSIX canonical
        // paths on both sides when computing a persistent volume-relative path.
        std::error_code ec;
        auto source = std::filesystem::canonical(file.getFullPathName().toStdString(), ec);
        if (ec)
            return {};
        // NSURLVolumeURLKey may return the Finder /Volumes alias for a hidden mount.
        // statfs provides the actual mount point used by the source path.
        struct statfs mounted{};
        if (statfs(source.c_str(), &mounted) != 0)
            return {};
        auto base = std::filesystem::canonical(mounted.f_mntonname, ec);
        if (ec)
            return {};
        auto relative = source.lexically_relative(base);
        if (!relative.empty() && *relative.begin() == "..")
        {
            // APFS firmlinks expose /Users from the Data volume at /System/Volumes/Data.
            auto physical = base / source.relative_path();
            if (std::filesystem::equivalent(physical, source, ec) && !ec)
                relative = source.relative_path();
        }
        return {uuid ? juce::String([uuid UTF8String]) : juce::String(),
                juce::String::fromUTF8(relative.c_str())};
    }
}
juce::String resolveRoot(const juce::String &path, const juce::String &id, const juce::String &relative)
{
    juce::File f(path);
    if (f.isDirectory() && (id.isEmpty() || volumeLocation(f).identity == id))
        return path;
    if (id.isEmpty() || relative == ".." || relative.startsWith("../") || relative.startsWith("/"))
        return {};
    // Include hidden/nobrowse mounts and disk images as well as Finder-visible volumes.
    int count = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0)
        return {};
    std::vector<struct statfs> mounts((size_t)count + 8);
    count = getfsstat(mounts.data(), (int)(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
    for (int i = 0; i < count; ++i)
    {
        auto volume = juce::File(juce::String::fromUTF8(mounts[(size_t)i].f_mntonname));
        if (volumeLocation(volume).identity == id)
        {
            auto candidate = volume.getChildFile(relative);
            if (candidate.isDirectory())
                return candidate.getFullPathName();
        }
    }
    return {};
}
struct SourceWatch::Impl
{
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    std::atomic<bool> *dirty = nullptr;
    DirectoryEvents *events = nullptr;
    bool running = false;
    juce::String excluded;
    static void callback(ConstFSEventStreamRef, void *context, size_t count, void *paths,
                         const FSEventStreamEventFlags *flags, const FSEventStreamEventId *)
    {
        auto &self = *static_cast<Impl *>(context);
        auto names = static_cast<char **>(paths);
        for (size_t i = 0; i < count; ++i)
        {
            auto path = juce::String::fromUTF8(names[i]);
            if (self.excluded.isNotEmpty() && (path == self.excluded || path.startsWith(self.excluded + "/")))
                continue;
            const auto mustReconcile = kFSEventStreamEventFlagMustScanSubDirs |
                                       kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagItemIsDir;
            if ((flags[i] & mustReconcile) != 0 || supportedExtension(juce::File(path)))
            {
                if (self.dirty)
                    self.dirty->store(true);
                if (self.events)
                {
                    bool recovery = (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs |
                                                 kFSEventStreamEventFlagRootChanged)) != 0;
                    if (flags[i] & kFSEventStreamEventFlagItemIsDir)
                        self.events->push(path, recovery);
                    self.events->push(path.upToLastOccurrenceOf("/", false, false), recovery);
                }
            }
        }
    }
};
SourceWatch::SourceWatch(const std::vector<juce::String> &paths, std::atomic<bool> &dirty,
                         const juce::String &excludedDirectory)
    : SourceWatch(paths, &dirty, nullptr, excludedDirectory)
{
}
SourceWatch::SourceWatch(const std::vector<juce::String> &paths, DirectoryEvents &events,
                         const juce::String &excluded)
    : SourceWatch(paths, nullptr, &events, excluded)
{
}
SourceWatch::SourceWatch(const std::vector<juce::String> &paths, std::atomic<bool> *dirty,
                         DirectoryEvents *events, const juce::String &excludedDirectory)
    : impl(std::make_unique<Impl>())
{
    impl->dirty = dirty;
    impl->events = events;
    impl->excluded = excludedDirectory;
    if (paths.empty())
        return;
    auto array = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (auto &path : paths)
    {
        auto str = CFStringCreateWithCString(nullptr, path.toRawUTF8(), kCFStringEncodingUTF8);
        CFArrayAppendValue(array, str);
        CFRelease(str);
    }
    FSEventStreamContext context{0, impl.get(), nullptr, nullptr, nullptr};
    impl->stream =
        FSEventStreamCreate(nullptr, Impl::callback, &context, array, kFSEventStreamEventIdSinceNow, 0.5,
                            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(array);
    if (impl->stream)
    {
        impl->queue = dispatch_queue_create("org.soundplus.file-events", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(impl->stream, impl->queue);
        impl->running = FSEventStreamStart(impl->stream);
    }
}
bool SourceWatch::started() const
{
    return impl->running;
}
SourceWatch::~SourceWatch()
{
    if (impl->stream)
    {
        FSEventStreamStop(impl->stream);
        FSEventStreamInvalidate(impl->stream);
        dispatch_sync(impl->queue, ^{
                      });
        FSEventStreamRelease(impl->stream);
        dispatch_release(impl->queue);
    }
}
} // namespace sp
