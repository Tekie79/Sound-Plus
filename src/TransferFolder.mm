#include "WorkspaceTransfer.h"
#import <Foundation/Foundation.h>

namespace sp
{
juce::String makeTransferFolderBookmark(const juce::File &folder)
{
    @autoreleasepool
    {
        if (!folder.isDirectory())
            throw std::runtime_error("Choose an available copy folder");
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:folder.getFullPathName().toRawUTF8()]
                               isDirectory:YES];
        // The shipped app is not sandboxed. Prefer a scoped bookmark when the host grants one.
        NSData *data = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                     includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        const bool scoped = data != nil;
        if (!data)
            data = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (!data)
            throw std::runtime_error("Cannot retain the selected copy folder");
        return juce::String(scoped ? "scoped:" : "local:") +
               juce::String::fromUTF8([[data base64EncodedStringWithOptions:0] UTF8String]);
    }
}
TransferArtifact withTransferFolder(const juce::String &bookmark, bool copy,
                                    const std::function<TransferArtifact(const juce::File &)> &work)
{
    if (!copy)
        return work({});
    @autoreleasepool
    {
        const bool scoped = bookmark.startsWith("scoped:");
        if (!scoped && !bookmark.startsWith("local:"))
            throw std::runtime_error("Choose the copy folder before transferring");
        auto encoded = bookmark.fromFirstOccurrenceOf(":", false, false);
        NSData *data = [[[NSData alloc] initWithBase64EncodedString:[NSString stringWithUTF8String:encoded.toRawUTF8()]
                                                          options:0] autorelease];
        BOOL stale = NO;
        const auto flags = NSURLBookmarkResolutionWithoutUI | NSURLBookmarkResolutionWithoutMounting |
                           (scoped ? NSURLBookmarkResolutionWithSecurityScope : 0);
        NSURL *url = data ? [NSURL URLByResolvingBookmarkData:data options:flags relativeToURL:nil
                                        bookmarkDataIsStale:&stale error:nil] : nil;
        if (!url || stale || !url.isFileURL)
            throw std::runtime_error("Copy folder is unavailable. Choose the folder again.");
        struct Access
        {
            NSURL *url;
            bool held;
            ~Access() { if (held) [url stopAccessingSecurityScopedResource]; }
        } access{url, scoped && [url startAccessingSecurityScopedResource]};
        if (scoped && !access.held)
            throw std::runtime_error("Access to the copy folder was denied");
        juce::File folder(juce::String::fromUTF8(url.path.UTF8String));
        if (!folder.isDirectory())
            throw std::runtime_error("Copy folder is offline or missing");
        return work(folder);
    }
}
} // namespace sp
