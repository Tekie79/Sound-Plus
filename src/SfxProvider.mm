#include "SfxGeneration.h"
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <curl/curl.h>
#include <cmath>

namespace sp::sfx
{
namespace
{
class Keychain final : public Credentials
{
    juce::String account;
    bool stabilityKey;

  public:
    Keychain(juce::String value, bool stability) : account(std::move(value)), stabilityKey(stability) {}

  private:
    NSDictionary *query()
    {
        return @{
            (id)kSecClass : (id)kSecClassGenericPassword,
            (id)kSecAttrService :
                (stabilityKey ? @"org.soundplus.audio.stability" : @"org.soundplus.sfx.elevenlabs"),
            (id)kSecAttrAccount : [NSString stringWithUTF8String:account.toRawUTF8()]
        };
    }

  public:
    bool contains() override
    {
        @autoreleasepool
        {
            // An existence-only query lets local history open before the user needs the secret.
            auto status = SecItemCopyMatching((CFDictionaryRef)query(), nullptr);
            if (status == errSecItemNotFound)
                return false;
            if (status != errSecSuccess)
                throw std::runtime_error("Keychain access unavailable");
            return true;
        }
    }
    juce::String read() override
    {
        @autoreleasepool
        {
            NSMutableDictionary *q = [NSMutableDictionary dictionaryWithDictionary:query()];
            q[(id)kSecReturnData] = @YES;
            CFTypeRef result = nullptr;
            auto status = SecItemCopyMatching((CFDictionaryRef)q, &result);
            if (status == errSecItemNotFound)
                return {};
            if (status != errSecSuccess)
                throw std::runtime_error("Keychain access unavailable");
            auto data = (CFDataRef)result;
            auto key =
                juce::String::fromUTF8((const char *)CFDataGetBytePtr(data), (int)CFDataGetLength(data));
            CFRelease(result);
            return key;
        }
    }
    void save(const juce::String &key) override
    {
        @autoreleasepool
        {
            if (key.isEmpty() || key.length() > 512 ||
                !key.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-."))
                throw std::runtime_error("Invalid API key format");
            NSData *data = [NSData dataWithBytes:key.toRawUTF8() length:key.getNumBytesAsUTF8()];
            auto status =
                SecItemUpdate((CFDictionaryRef)query(), (CFDictionaryRef) @{(id)kSecValueData : data});
            if (status == errSecItemNotFound)
            {
                NSMutableDictionary *q = [NSMutableDictionary dictionaryWithDictionary:query()];
                q[(id)kSecValueData] = data;
                q[(id)kSecAttrAccessible] = (id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
                status = SecItemAdd((CFDictionaryRef)q, nullptr);
            }
            if (status != errSecSuccess)
                throw std::runtime_error("Could not save the key in Keychain");
        }
    }
    void erase() override
    {
        @autoreleasepool
        {
            auto s = SecItemDelete((CFDictionaryRef)query());
            if (s != errSecItemNotFound && s != errSecSuccess)
                throw std::runtime_error("Could not delete key");
        }
    }
};
class Eleven final : public Provider
{
    Http http;

  public:
    explicit Eleven(Http h) : http(std::move(h)) {}
    Capabilities capabilities() const override
    {
        return {"ElevenLabs", "eleven_text_to_sound_v3", .5, 30., true, false, false};
    }
    Response generate(const Request &r, const juce::String &key, const std::atomic<bool> &stop) override
    {
        auto problem = validate(r, capabilities());
        if (problem.isNotEmpty())
            throw std::runtime_error(problem.toStdString());
        juce::var body(new juce::DynamicObject);
        auto *o = body.getDynamicObject();
        o->setProperty("model_id", capabilities().model);
        o->setProperty("text", r.prompt);
        o->setProperty("loop", r.loop);
        o->setProperty("prompt_influence", r.influence);
        if (r.seconds)
            o->setProperty("duration_seconds", *r.seconds);
        return http("/v1/sound-generation?output_format=" +
                        juce::String(r.wav ? "wav_44100" : "mp3_44100_128"),
                    key, juce::JSON::toString(body, true), stop);
    }
    bool testKey(const juce::String &key, const std::atomic<bool> &stop) override
    {
        auto result = http("/v1/user", key, {}, stop);
        return result.status == 200 &&
               juce::JSON::parse(
                   juce::String::fromUTF8((const char *)result.bytes.getData(), (int)result.bytes.getSize()))
                   .isObject();
    }
};
struct Download
{
    Response response;
    const std::atomic<bool> &stop;
    static size_t append(char *p, size_t s, size_t n, void *ctx)
    {
        auto &d = *static_cast<Download *>(ctx);
        auto bytes = s * n;
        if (d.stop.load() || bytes > 32 * 1024 * 1024 - d.response.bytes.getSize())
            return 0;
        try
        {
            d.response.bytes.append(p, bytes);
        }
        catch (...)
        {
            return 0;
        }
        return bytes;
    }
    static size_t header(char *p, size_t s, size_t n, void *ctx)
    {
        auto text = juce::String::fromUTF8(p, (int)(s * n));
        if (text.startsWithIgnoreCase("request-id:"))
        {
            auto id = text.fromFirstOccurrenceOf(":", false, false).trim();
            if (id.length() <= 128 && id.containsOnly("abcdefABCDEF0123456789-"))
                static_cast<Download *>(ctx)->response.requestId = id;
        }
        return s * n;
    }
    static int progress(void *ctx, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return static_cast<Download *>(ctx)->stop.load() ? 1 : 0;
    }
};
} // namespace
Response providerHttp(const juce::String &base, const juce::String &path, const juce::String &key,
                      const juce::String &body, const std::atomic<bool> &stop)
{
    if (key.isEmpty() || key.containsAnyOf("\r\n"))
        throw std::runtime_error("Invalid credential");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
        throw std::runtime_error("Network unavailable");
    Download d{{}, stop};
    auto *curl = handle.get();
    auto url = base + path;
    auto *list = curl_slist_append(nullptr, ("xi-api-key: " + key).toRawUTF8());
    if (!list)
        throw std::bad_alloc();
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, curl_slist_free_all);
    if (!curl_slist_append(list, "Content-Type: application/json"))
        throw std::bad_alloc();
    curl_easy_setopt(curl, CURLOPT_URL, url.toRawUTF8());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 75L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Download::append);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Download::header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &d);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Download::progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &d);
    if (body.isNotEmpty())
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.toRawUTF8());
    }
    if (stop.load() || curl_easy_perform(curl) != CURLE_OK)
        throw std::runtime_error("Connection interrupted");
    char *mime = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &d.response.status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &mime);
    if (mime)
        d.response.mime = juce::String(mime).upToFirstOccurrenceOf(";", false, false).trim().toLowerCase();
    return std::move(d.response);
}
juce::String validate(const Request &r, const Capabilities &c)
{
    if (!r.approved)
        return "Approve this billable batch first";
    if (c.audioInput != r.edit.has_value())
        return "This mode needs its own provider and input";
    if (r.edit)
    {
        if (!r.wav)
            return "Audio edits require WAV output";
        if (auto issue = validateEdit(*r.edit); issue.isNotEmpty())
            return issue;
    }
    if (r.prompt.trim().isEmpty() || r.prompt.length() > 1000)
        return "Enter a prompt of 1–1000 characters";
    if (r.candidates < 1 || r.candidates > 4)
        return "Choose one to four candidates";
    if (r.seconds && (!std::isfinite(*r.seconds) || *r.seconds < c.minSeconds || *r.seconds > c.maxSeconds))
        return "Duration must be 0.5–30 seconds";
    if (!std::isfinite(r.influence) || r.influence < 0 || r.influence > 1)
        return "Invalid prompt influence";
    if (r.loop && !c.loop)
        return "Looping unavailable";
    return {};
}
std::unique_ptr<Provider> elevenLabs(Http h)
{
    if (!h)
        h = [](auto &path, auto &key, auto &body, auto &stop)
        { return providerHttp("https://api.elevenlabs.io", path, key, body, stop); };
    return std::make_unique<Eleven>(std::move(h));
}
std::unique_ptr<Credentials> keychain(juce::String account, bool stabilityKey)
{
    return std::make_unique<Keychain>(std::move(account), stabilityKey);
}
} // namespace sp::sfx
