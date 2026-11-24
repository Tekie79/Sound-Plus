#include "SfxGeneration.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <fcntl.h>
#include <unistd.h>

namespace sp::sfx
{
namespace
{
void require(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}
juce::String now()
{
    return juce::Time::getCurrentTime().toISO8601(true);
}
juce::var fields(std::initializer_list<std::pair<juce::Identifier, juce::var>> list)
{
    juce::var v(new juce::DynamicObject);
    for (auto &[k, value] : list)
        v.getDynamicObject()->setProperty(k, value);
    return v;
}
juce::var optionsJson(const Request &r)
{
    return fields({{"prompt", r.prompt},
                   {"duration", r.seconds ? juce::var(*r.seconds) : juce::var()},
                   {"influence", r.influence},
                   {"candidates", r.candidates},
                   {"loop", r.loop},
                   {"wav", r.wav}});
}
juce::String encode(const Candidate &c)
{
    return juce::JSON::toString(fields({{"version", 1},
                                        {"id", c.id},
                                        {"batch", c.batch},
                                        {"provider", c.provider},
                                        {"model", c.model},
                                        {"options", optionsJson(c.options)},
                                        {"format", c.format},
                                        {"approvedAt", c.approvedAt},
                                        {"consent", "prompt and independent billable requests v1"},
                                        {"estimate", "unavailable"},
                                        {"startedAt", c.startedAt},
                                        {"finishedAt", c.finishedAt},
                                        {"requestId", c.requestId},
                                        {"sha256", c.sha256},
                                        {"path", c.published},
                                        {"assetId", c.assetId},
                                        {"detail", c.detail},
                                        {"fingerprint", c.fingerprint},
                                        {"state", (int)c.state},
                                        {"ordinal", c.ordinal},
                                        {"channels", c.channels},
                                        {"rate", c.rate},
                                        {"layout", c.channels == 2   ? "stereo"
                                                   : c.channels == 1 ? "mono"
                                                                     : ""},
                                        {"frames", (juce::int64)c.frames}}),
                                true);
}
Candidate decode(const juce::String &json)
{
    auto v = juce::JSON::parse(json);
    require((int)v["version"] == 1, "Unsupported generation journal");
    Candidate c;
    c.id = v["id"];
    c.batch = v["batch"];
    c.provider = v["provider"];
    c.model = v["model"];
    auto r = v["options"];
    c.prompt = c.options.prompt = r["prompt"].toString();
    c.options.candidates = r["candidates"];
    c.options.approved = true;
    c.options.loop = r["loop"];
    c.options.wav = r["wav"];
    c.options.influence = r["influence"];
    if (!r["duration"].isVoid())
        c.options.seconds = (double)r["duration"];
    c.format = v["format"];
    c.approvedAt = v["approvedAt"];
    c.startedAt = v["startedAt"];
    c.finishedAt = v["finishedAt"];
    c.requestId = v["requestId"];
    c.sha256 = v["sha256"];
    c.published = v["path"];
    c.assetId = v["assetId"];
    c.detail = v["detail"];
    c.fingerprint = v["fingerprint"];
    c.state = (State)(int)v["state"];
    c.ordinal = v["ordinal"];
    c.channels = v["channels"];
    c.rate = v["rate"];
    c.frames = (juce::int64)v["frames"];
    return c;
}
void promote(const juce::File &stage, const juce::File &target)
{
    int fd = ::open(stage.getFullPathName().toRawUTF8(), O_RDONLY | O_NOFOLLOW);
    bool synced = fd >= 0 && ::fsync(fd) == 0;
    if (fd >= 0)
        ::close(fd);
    require(synced && renamex_np(stage.getFullPathName().toRawUTF8(), target.getFullPathName().toRawUTF8(),
                                 RENAME_EXCL) == 0,
            "Cannot publish WAV. Check destination permissions, name and free space");
}
juce::String digest(const juce::File &f)
{
    auto in = f.createInputStream();
    require(in && in->getTotalLength() > 0, "Candidate audio is unavailable");
    return juce::SHA256(*in).toHexString();
}
void validateMp3(const uint8_t *p, size_t size)
{
    // Only the negotiated MPEG-1 Layer III, 44.1 kHz stream is supported.
    size_t i = 0;
    if (size >= 10 && !memcmp(p, "ID3", 3))
    {
        for (int k = 6; k <= 9; ++k)
            require(p[k] < 128, "Invalid ID3 length");
        i = 10 + ((size_t)p[6] << 21) + (p[7] << 14) + (p[8] << 7) + p[9] + ((p[5] & 16) ? 10 : 0);
    }
    static constexpr int kbps[]{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    int frames = 0;
    while (i + 4 <= size)
    {
        if (size - i == 128 && !memcmp(p + i, "TAG", 3))
        {
            i = size;
            break;
        }
        require(p[i] == 255 && (p[i + 1] & 254) == 250 && (p[i + 2] & 12) == 0, "Invalid MP3 frame");
        int rate = kbps[p[i + 2] >> 4];
        size_t bytes = 144000 * rate / 44100 + ((p[i + 2] >> 1) & 1);
        require(rate && bytes <= size - i, "Truncated MP3");
        i += bytes;
        ++frames;
    }
    require(i == size && frames > 0, "Incomplete MP3 response");
}
void canonicalise(const Response &r, Candidate &c, const juce::File &target, const std::atomic<bool> &cancel)
{
    auto n = r.bytes.getSize();
    auto p = static_cast<const uint8_t *>(r.bytes.getData());
    require(n >= 12 && n <= 32 * 1024 * 1024, "Empty or oversized provider audio");
    if (c.options.wav)
    {
        require(r.mime == "audio/wav" || r.mime == "audio/x-wav" || r.mime == "audio/wave" ||
                    r.mime == "application/octet-stream",
                "Unexpected WAV content type");
        require(!memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4) &&
                    (uint64_t)juce::ByteOrder::littleEndianInt(p + 4) + 8 == n,
                "Incomplete WAV response");
        size_t i = 12;
        bool data = false;
        while (i + 8 <= n)
        {
            size_t len = juce::ByteOrder::littleEndianInt(p + i + 4);
            require(len <= n - i - 8, "Truncated WAV chunk");
            if (!memcmp(p + i, "data", 4))
                data = len > 0;
            i += 8 + len + (len & 1);
        }
        require(i == n && data, "Invalid WAV data");
    }
    else
    {
        require(r.mime == "audio/mpeg" || r.mime == "audio/mp3" || r.mime == "application/octet-stream",
                "Unexpected MP3 content type");
        validateMp3(p, n);
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(std::make_unique<juce::MemoryInputStream>(p, n, false)));
    require(reader && reader->sampleRate == 44100 && reader->numChannels >= 1 && reader->numChannels <= 2 &&
                reader->lengthInSamples > 0 && reader->lengthInSamples <= 44100 * 31,
            "Unexpected audio layout, rate or duration");
    c.rate = (int)reader->sampleRate;
    c.channels = (int)reader->numChannels;
    c.frames = reader->lengthInSamples;
    juce::TemporaryFile stage(target.getSiblingFile("." + c.id + ".stage"),
                              juce::TemporaryFile::useHiddenFile);
    juce::WavAudioFormat wav;
    auto stream = stage.getFile().createOutputStream();
    require(stream != nullptr, "Could not open candidate file");
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), c.rate, (unsigned)c.channels, 24, {}, 0));
    require(writer != nullptr, "Could not create PCM WAV");
    stream.release();
    juce::AudioBuffer<float> block(c.channels, 4096);
    for (int64_t at = 0; at < c.frames; at += 4096)
    {
        require(!cancel.load(), "Cancelled");
        int count = (int)std::min<int64_t>(4096, c.frames - at);
        require(reader->read(&block, 0, count, at, true, true), "Audio decode failed");
        for (int ch = 0; ch < c.channels; ++ch)
            for (int j = 0; j < count; ++j)
                require(std::isfinite(block.getSample(ch, j)), "Nonfinite audio");
        require(writer->writeFromAudioSampleBuffer(block, 0, count), "WAV write failed");
    }
    require(writer->flush(), "Cannot flush WAV");
    writer.reset();
    std::unique_ptr<juce::AudioFormatReader> verify(
        formats.createReaderFor(stage.getFile().createInputStream()));
    require(verify && verify->lengthInSamples == c.frames && (int)verify->numChannels == c.channels &&
                verify->sampleRate == c.rate && verify->bitsPerSample == 24 &&
                stage.getFile().getSize() >= c.frames * c.channels * 3,
            "WAV verification failed");
    c.sha256 = digest(stage.getFile());
    require(!cancel.load(), "Cancelled");
    promote(stage.getFile(), target);
}
juce::String httpIssue(long status)
{
    switch (status)
    {
    case 401:
        return "API key was rejected";
    case 402:
        return "Provider credits or plan required";
    case 403:
        return "Permission or format entitlement denied";
    case 429:
        return "Rate limited. Review before starting another batch";
    default:
        return "Provider returned HTTP " + juce::String((int)status);
    }
}
} // namespace
juce::String stateName(State s)
{
    switch (s)
    {
    case State::queued:
        return "Queued";
    case State::submitting:
        return "Submitting";
    case State::ready:
        return "Ready";
    case State::cancelled:
        return "Cancelled";
    case State::review:
        return "Needs review";
    default:
        return "Failed";
    }
}
struct Generation::Journal
{
    sqlite3 *db = nullptr;
    explicit Journal(const juce::File &dir)
    {
        require(dir.createDirectory().wasOk(), "Cannot open generation folder");
        int rc = sqlite3_open(dir.getChildFile("history.sqlite").getFullPathName().toRawUTF8(), &db);
        if (rc != SQLITE_OK ||
            sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY,record TEXT NOT NULL)",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
        {
            sqlite3_close(db);
            throw std::runtime_error("Cannot open generation journal");
        }
    }
    ~Journal()
    {
        sqlite3_close(db);
    }
    using Stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    Stmt statement(const char *sql)
    {
        sqlite3_stmt *s = nullptr;
        require(sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK, "Generation history unavailable");
        return Stmt(s, sqlite3_finalize);
    }
    void put(const Candidate &c)
    {
        auto s =
            statement("INSERT INTO jobs VALUES(?,?) ON CONFLICT(id) DO UPDATE SET record=excluded.record");
        auto json = encode(c);
        sqlite3_bind_text(s.get(), 1, c.id.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.get(), 2, json.toRawUTF8(), -1, SQLITE_TRANSIENT);
        require(sqlite3_step(s.get()) == SQLITE_DONE, "Cannot retain generation record");
    }
    std::vector<Candidate> list()
    {
        auto s = statement("SELECT record FROM jobs ORDER BY rowid DESC LIMIT 500");
        std::vector<Candidate> out;
        int rc;
        while ((rc = sqlite3_step(s.get())) == SQLITE_ROW)
            out.push_back(decode(juce::String::fromUTF8((const char *)sqlite3_column_text(s.get(), 0))));
        require(rc == SQLITE_DONE, "Cannot read history");
        return out;
    }
    Candidate get(const juce::String &id)
    {
        auto s = statement("SELECT record FROM jobs WHERE id=?");
        sqlite3_bind_text(s.get(), 1, id.toRawUTF8(), -1, SQLITE_TRANSIENT);
        require(sqlite3_step(s.get()) == SQLITE_ROW, "Candidate unavailable");
        return decode(juce::String::fromUTF8((const char *)sqlite3_column_text(s.get(), 0)));
    }
};
Generation::Generation(juce::File dir, std::unique_ptr<Provider> p, std::unique_ptr<Credentials> k)
    : folder(std::move(dir)), provider(std::move(p)), secrets(std::move(k))
{
    view.busy = false;
    schedule(
        [this]
        {
            journal = std::make_unique<Journal>(folder);
            for (auto c : journal->list())
                if (c.state == State::submitting || c.state == State::queued)
                {
                    c.state = c.state == State::queued ? State::cancelled : State::review;
                    c.detail = c.state == State::review
                                   ? "Interrupted after submission. Check provider billing"
                                   : "Interrupted before submission";
                    c.finishedAt = now();
                    journal->put(c);
                    candidateFile(c.id).deleteFile();
                }
            auto key = secrets->read();
            std::lock_guard l(mutex);
            view.hasKey = key.isNotEmpty();
            view.status = view.hasKey ? "Ready" : "Add an ElevenLabs API key to generate";
        });
}
Generation::~Generation()
{
    {
        std::lock_guard l(mutex);
        closing = true;
        stopped = true;
    }
    worker.finish({});
}
Snapshot Generation::snapshot() const
{
    std::lock_guard l(mutex);
    return view;
}
void Generation::report(juce::String s)
{
    std::lock_guard l(mutex);
    view.status = std::move(s);
    ++view.version;
}
void Generation::refresh()
{
    if (!journal)
        return;
    auto rows = journal->list();
    std::lock_guard l(mutex);
    view.history = std::move(rows);
    ++view.version;
}
bool Generation::schedule(std::function<void()> job)
{
    std::lock_guard lock(mutex);
    if (closing || view.busy)
        return false;
    view.busy = true;
    ++view.version;
    stopped = false;
    return worker.submit(
        [this, job = std::move(job)]
        {
            try
            {
                job();
            }
            catch (const std::exception &error)
            {
                report(error.what());
            }
            try
            {
                refresh();
            }
            catch (...)
            {
                report("History could not be read");
            }
            std::lock_guard l(mutex);
            view.busy = false;
            ++view.version;
        });
}
void Generation::cancel()
{
    stopped = true;
}
juce::File Generation::candidateFile(const juce::String &id) const
{
    require(id.length() == 32 && id.containsOnly("abcdef0123456789"), "Invalid candidate identity");
    return folder.getChildFile(id + ".wav");
}
bool Generation::credential(juce::String action, juce::String key)
{
    return schedule(
        [this, action, key]
        {
            if (action == "save")
                secrets->save(key);
            else if (action == "delete")
                secrets->erase();
            else
                require(action == "test", "Unknown credential action");
            auto saved = secrets->read();
            {
                std::lock_guard l(mutex);
                view.hasKey = saved.isNotEmpty();
            }
            if (action == "test")
            {
                require(saved.isNotEmpty(), "No API key");
                report(provider->testKey(saved, stopped)
                           ? "Credential verified"
                           : "Account check denied. Check the key and user-read permission");
            }
            else
                report(action == "save" ? "Key saved in Keychain" : "Key deleted");
        });
}
bool Generation::submit(Request request)
{
    auto cap = provider->capabilities();
    auto problem = validate(request, cap);
    if (problem.isNotEmpty())
    {
        report(problem);
        return false;
    }
    return schedule(
        [this, request, cap]
        {
            require(journal != nullptr, "Generation history unavailable");
            auto key = secrets->read();
            require(key.isNotEmpty(), "No API key");
            auto batch = juce::Uuid().toString();
            std::vector<Candidate> rows;
            auto options = juce::JSON::toString(optionsJson(request), true);
            auto input = cap.provider + cap.model + options;
            auto fingerprint = juce::SHA256(input.toRawUTF8(), input.getNumBytesAsUTF8()).toHexString();
            auto approved = now();
            for (int i = 0; i < request.candidates; ++i)
            {
                Candidate c;
                c.id = juce::Uuid().toString();
                c.batch = batch;
                c.provider = cap.provider;
                c.model = cap.model;
                c.options = request;
                c.prompt = request.prompt;
                c.format = request.wav ? "wav_44100" : "mp3_44100_128";
                c.approvedAt = approved;
                c.ordinal = i + 1;
                c.fingerprint = fingerprint;
                journal->put(c);
                rows.push_back(c);
            }
            refresh();
            bool halted = false;
            for (auto &c : rows)
            {
                if (stopped || halted)
                {
                    c.state = State::cancelled;
                    c.detail = "Skipped before submission";
                }
                else
                {
                    c.state = State::submitting;
                    c.startedAt = now();
                    journal->put(c);
                    refresh();
                    report("Generating candidate " + juce::String(c.ordinal) + " of " +
                           juce::String(request.candidates));
                    try
                    {
                        auto r = provider->generate(request, key, stopped);
                        c.requestId = r.requestId;
                        if (r.status != 200)
                        {
                            c.state = r.status >= 500 ? State::review : State::failed;
                            c.detail = httpIssue(r.status);
                            halted = true;
                        }
                        else
                        {
                            canonicalise(r, c, candidateFile(c.id), stopped);
                            c.state = State::ready;
                        }
                    }
                    catch (const std::exception &e)
                    {
                        c.state = stopped ? State::cancelled : State::review;
                        halted = true;
                        c.detail = stopped ? "Cancelled locally. The provider may still bill this request"
                                           : juce::String(e.what()) + ". Check billing before retrying";
                    }
                }
                c.finishedAt = now();
                journal->put(c);
                refresh();
                if (c.detail.isNotEmpty() && c.startedAt.isNotEmpty())
                    report(c.detail);
            }
            if (!halted)
                report(stopped ? "Batch cancelled" : "Candidates ready");
        });
}
bool Generation::use(const juce::String &id, juce::File destination, FileResult done, Index index)
{
    return schedule(
        [this, id, destination, done, index]
        {
            require(journal != nullptr, "Generation history unavailable");
            auto c = journal->get(id);
            require(c.state == State::ready, "Candidate is unavailable");
            auto file = candidateFile(c.id);
            if (c.published.isNotEmpty())
                file = juce::File(c.published);
            require(digest(file) == c.sha256, "Candidate has changed");
            if (destination != juce::File() && c.published.isEmpty())
            {
                require(destination.isDirectory(), "Select an existing generated-sounds folder");
                auto name = juce::File::createLegalFileName(c.prompt).substring(0, 55).trim();
                auto out = destination.getChildFile(name + " - " + c.id + ".wav");
                if (!out.exists())
                {
                    juce::TemporaryFile temp(out.withFileExtension(".part"),
                                             juce::TemporaryFile::useHiddenFile);
                    require(file.copyFileTo(temp.getFile()) && digest(temp.getFile()) == c.sha256,
                            "Could not verify generated copy");
                    promote(temp.getFile(), out);
                }
                require(digest(out) == c.sha256, "A different file already exists at this destination");
                c.published = out.getFullPathName();
                journal->put(c);
                file = out;
            }
            if (index)
            {
                auto a = index(file, stopped);
                require(c.published == a.path && a.online && a.samples == c.frames &&
                            a.sampleRate == c.rate && a.channels == c.channels && digest(file) == c.sha256,
                        "Indexed file differs from candidate");
                require(c.assetId.isEmpty() || c.assetId == a.id, "Indexed source identity has changed");
                c.assetId = a.id;
                journal->put(c);
            }
            if (done)
                done(file, c);
            report(destination == juce::File() ? "Candidate verified" : "Saved to Library");
        });
}
} // namespace sp::sfx
