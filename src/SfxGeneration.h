#pragma once
#include "AudioTransform.h"
#include "SerialWorker.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace sp::sfx
{
struct Request
{
    juce::String prompt;
    std::optional<double> seconds;
    double influence = 0.3;
    int candidates = 1;
    bool loop = false, wav = false, approved = false;
    std::optional<AudioEdit> edit;
};
struct Capabilities
{
    juce::String provider, model;
    double minSeconds, maxSeconds;
    bool loop, remoteCancel, audioInput;
};
struct Response
{
    long status = 0;
    juce::String mime, requestId;
    juce::MemoryBlock bytes;
};
class Provider
{
  public:
    virtual ~Provider() = default;
    virtual Capabilities capabilities() const = 0;
    virtual Response generate(const Request &, const juce::String &key, const std::atomic<bool> &) = 0;
    virtual Response poll(const juce::String &, const juce::String &, const std::atomic<bool> &)
    {
        throw std::runtime_error("This provider has no remote job lookup");
    }
    virtual bool testKey(const juce::String &, const std::atomic<bool> &) = 0;
};
using Http = std::function<Response(const juce::String &path, const juce::String &key,
                                    const juce::String &body, const std::atomic<bool> &)>;
std::unique_ptr<Provider> elevenLabs(Http = {});
std::unique_ptr<Provider> stability(juce::String base = "https://api.stability.ai");
Response providerHttp(const juce::String &base, const juce::String &path, const juce::String &key,
                      const juce::String &body, const std::atomic<bool> &);
class Credentials
{
  public:
    virtual ~Credentials() = default;
    virtual bool contains() { return read().isNotEmpty(); }
    virtual juce::String read() = 0;
    virtual void save(const juce::String &) = 0;
    virtual void erase() = 0;
};
std::unique_ptr<Credentials> keychain(juce::String account = "default", bool stabilityKey = false);
juce::String validate(const Request &, const Capabilities &);
enum class State
{
    queued,
    submitting,
    ready,
    cancelled,
    failed,
    review,
    polling
};
struct Candidate
{
    juce::String id, batch, provider, model, prompt, format, approvedAt, startedAt, finishedAt, requestId,
        sha256, published, assetId, detail, fingerprint;
    juce::String accountBinding, rawHash;
    Request options;
    State state = State::queued;
    int ordinal = 0, channels = 0, rate = 0;
    int64_t frames = 0;
    juce::String outputProvenance() const;
};
struct Snapshot
{
    std::vector<Candidate> history;
    juce::String status;
    bool busy = true, hasKey = false;
    uint64_t version = 0;
};
class Generation
{
  public:
    Generation(juce::File privateFolder, std::unique_ptr<Provider> = elevenLabs(),
               std::unique_ptr<Credentials> = keychain());
    ~Generation();
    Snapshot snapshot() const;
    uint64_t version() const
    {
        std::lock_guard lock(mutex);
        return view.version;
    }
    bool submit(Request);
    void cancel();
    bool inspectInput(TransformInput, std::function<void(Asset)>);
    bool resume(const juce::String &id);
    Capabilities capabilities() const
    {
        return provider->capabilities();
    }
    bool credential(juce::String action, juce::String key = {});
    using FileResult = std::function<void(juce::File, Candidate)>; // Runs on worker.
    using Index = std::function<Asset(const juce::File &, const std::atomic<bool> &)>;
    bool use(const juce::String &id, juce::File destinationFolder, FileResult, Index = {});

  private:
    struct Journal;
    bool schedule(std::function<void()>);
    void refresh();
    void pollResult(Candidate &, const juce::String &);
    void acceptResponse(const Response &, Candidate &);
    void report(juce::String);
    juce::File candidateFile(const juce::String &) const;
    juce::File folder;
    std::unique_ptr<Provider> provider;
    std::unique_ptr<Credentials> secrets;
    std::unique_ptr<Journal> journal;
    mutable std::mutex mutex;
    Snapshot view;
    bool closing = false;
    std::atomic<bool> stopped{false};
    SerialWorker worker;
};
juce::String stateName(State);
} // namespace sp::sfx
