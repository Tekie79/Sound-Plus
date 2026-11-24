#include "SfxGeneration.h"
#include <juce_cryptography/juce_cryptography.h>
#include <curl/curl.h>
namespace sp::sfx
{
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
struct HttpResult
{
    Response response;
    const std::atomic<bool> &stop;
    static size_t write(char *p, size_t s, size_t n, void *ctx)
    {
        auto &r = *static_cast<HttpResult *>(ctx);
        if (r.stop || s * n > 140 * 1024 * 1024 - r.response.bytes.getSize())
            return 0;
        try
        {
            r.response.bytes.append(p, s * n);
        }
        catch (...)
        {
            return 0;
        }
        return s * n;
    }
    static int progress(void *ctx, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return static_cast<HttpResult *>(ctx)->stop ? 1 : 0;
    }
};
class Stability final : public Provider
{
    juce::String base;
    Response http(const juce::String &path, const juce::String &key, const Request *request,
                  const std::atomic<bool> &stop)
    {
        require(key.isNotEmpty() && !key.containsAnyOf("\r\n") && !stop,
                "Credential missing or operation cancelled");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> h(curl_easy_init(), curl_easy_cleanup);
        require(h != nullptr, "Network unavailable");
        auto *c = h.get();
        auto url = base + path;
        auto *list = curl_slist_append(nullptr, ("Authorization: Bearer " + key).toRawUTF8());
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, curl_slist_free_all);
        require(list && curl_slist_append(list, request || path.startsWith("/v1") ? "Accept: application/json"
                                                                                  : "Accept: audio/*"),
                "Network unavailable");
        HttpResult result{{}, stop};
        curl_easy_setopt(c, CURLOPT_URL, url.toRawUTF8());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, HttpResult::write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &result);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, HttpResult::progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &result);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        std::unique_ptr<curl_mime, decltype(&curl_mime_free)> form(nullptr, curl_mime_free);
        if (request)
        {
            auto &r = *request;
            auto &e = *r.edit;
            verifyTransform(e.input);
            juce::MemoryBlock audio;
            auto in = juce::File(e.input.path).createInputStream();
            require(in && in->getTotalLength() == e.input.bytes && e.input.bytes < 100000000 &&
                        in->readIntoMemoryBlock(audio, (ssize_t)e.input.bytes) == e.input.bytes &&
                        juce::SHA256(audio).toHexString() == e.input.sha256 && !stop,
                    "Upload changed or cancelled");
            form.reset(curl_mime_init(c));
            require(form != nullptr, "Cannot prepare upload");
            auto field = [&](const char *name, juce::String value)
            {
                auto *part = curl_mime_addpart(form.get());
                require(part && curl_mime_name(part, name) == CURLE_OK &&
                            curl_mime_data(part, value.toRawUTF8(), value.getNumBytesAsUTF8()) == CURLE_OK,
                        "Cannot prepare upload");
            };
            field("model", "stable-audio-3");
            field("prompt", r.prompt);
            field("output_format", "wav");
            field("duration", juce::String(e.input.frames / e.input.rate, 9));
            if (e.inpaint)
            {
                field("mask_start", juce::String(e.maskStart / e.input.rate, 9));
                field("mask_end", juce::String(e.maskEnd / e.input.rate, 9));
            }
            else
                field("strength", juce::String(r.influence, 6));
            auto *part = curl_mime_addpart(form.get());
            require(part && curl_mime_name(part, "audio") == CURLE_OK &&
                        curl_mime_filename(part, "input.wav") == CURLE_OK &&
                        curl_mime_type(part, "audio/wav") == CURLE_OK &&
                        curl_mime_data(part, (const char *)audio.getData(), audio.getSize()) == CURLE_OK,
                    "Cannot attach audio");
            curl_easy_setopt(c, CURLOPT_MIMEPOST, form.get());
        }
        require(!stop && curl_easy_perform(c) == CURLE_OK, "Connection interrupted");
        char *mime = nullptr;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &result.response.status);
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &mime);
        if (mime)
            result.response.mime =
                juce::String(mime).upToFirstOccurrenceOf(";", false, false).trim().toLowerCase();
        return std::move(result.response);
    }

  public:
    explicit Stability(juce::String url) : base(std::move(url))
    {
        require(base == "https://api.stability.ai" ||
                    (base.startsWith("http://127.0.0.1:") && base.substring(17).containsOnly("0123456789")),
                "Unsupported provider endpoint");
    }
    Capabilities capabilities() const override
    {
        return {"Stability AI", "stable-audio-3", 6, 380, false, false, true};
    }
    Response generate(const Request &r, const juce::String &key, const std::atomic<bool> &stop) override
    {
        auto issue = validate(r, capabilities());
        if (issue.isNotEmpty())
            throw std::runtime_error(issue.toStdString());
        auto response = http(juce::String("/v2beta/audio/stable-audio/") +
                                 (r.edit->inpaint ? "inpaint" : "audio-to-audio"),
                             key, &r, stop);
        if (response.status == 202)
        {
            require(response.mime == "application/json" && response.bytes.getSize() < 65536,
                    "Invalid submission receipt");
            auto body = juce::JSON::parse(juce::String::fromUTF8((const char *)response.bytes.getData(),
                                                                 (int)response.bytes.getSize()));
            response.requestId = body["id"].toString();
            require(response.requestId.length() >= 1 && response.requestId.length() <= 128 &&
                        response.requestId.containsOnly("0123456789abcdefABCDEF-"),
                    "Missing or invalid provider job ID");
        }
        return response;
    }
    Response poll(const juce::String &id, const juce::String &key, const std::atomic<bool> &stop) override
    {
        require(id.isNotEmpty() && id.length() <= 128 && id.containsOnly("abcdefABCDEF0123456789-"),
                "Invalid provider job ID");
        return http("/v2beta/audio/results/" + id, key, nullptr, stop);
    }
    bool testKey(const juce::String &key, const std::atomic<bool> &stop) override
    {
        return http("/v1/user/balance", key, nullptr, stop).status == 200;
    }
};
} // namespace
std::unique_ptr<Provider> stability(juce::String base)
{
    return std::make_unique<Stability>(std::move(base));
}
} // namespace sp::sfx
