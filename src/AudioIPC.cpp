#include "AudioIPC.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
extern char **environ;
namespace sp
{
juce::var message(const juce::String &command)
{
    auto *o = new juce::DynamicObject();
    o->setProperty("version", protocolVersion);
    o->setProperty("command", command);
    return juce::var(o);
}
bool sendMessage(int fd, const juce::var &msg)
{
    auto line = juce::JSON::toString(msg, true) + "\n";
    auto bytes = line.getNumBytesAsUTF8();
    // Commands are deliberately bounded to fit the socket send buffer in one write.
    if (bytes > 16384)
        return false;
    auto result = send(fd, line.toRawUTF8(), bytes, 0);
    return result == (ssize_t)bytes;
}
AudioClient::AudioClient(juce::File exe) : executable(std::move(exe))
{
    executable = executable.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusAudio");
    worker = std::thread([this] { run(); });
}
AudioClient::~AudioClient()
{
    quitting = true;
    if (worker.joinable())
        worker.join();
}
void AudioClient::enqueue(juce::var v)
{
    std::lock_guard lock(mutex);
    const auto command = v["command"].toString();
    if (command == "play" || command=="play_design") { desiredPlaying = true; desiredId = v["id"].toString(); }
    if (command == "stop") { desiredPlaying = false; desiredId.clear(); }
    if (command == "play" || command=="play_design" || command == "stop")
        std::erase_if(outgoing,[](const auto &old) { auto name = old["command"].toString(); return name == "play" || name=="play_design" || name == "stop"; });
    else if (command == "gain")
        std::erase_if(outgoing,[](const auto &old) { return old["command"].toString() == "gain"; });
    if (outgoing.size() >= 16)
        outgoing.pop_front();
    outgoing.push_back(std::move(v));
}
void AudioClient::play(const juce::String &id, const juce::File &f)
{
    playRegion(id,f,{});
}
uint64_t AudioClient::playRegion(const juce::String &id, const juce::File &f, const PlaybackRegion &region)
{
    auto m = message("play");
    m.getDynamicObject()->setProperty("id", id);
    m.getDynamicObject()->setProperty("path", f.getFullPathName());
    auto generation = ++playbackGeneration;
    auto *o = m.getDynamicObject();
    o->setProperty("playGeneration",(juce::int64)generation);
    o->setProperty("startSample",(juce::int64)region.startSample);
    o->setProperty("endSample",(juce::int64)region.endSample);
    o->setProperty("positionSample",(juce::int64)region.positionSample);
    o->setProperty("auditionPair",region.auditionPair);
    o->setProperty("size",(juce::int64)region.size); o->setProperty("modified",(juce::int64)region.modified);
    o->setProperty("inode",(juce::int64)region.inode); o->setProperty("revision",(juce::int64)region.revision);
    enqueue(m);
    return generation;
}
void AudioClient::stop()
{
    auto m = message("stop"); m.getDynamicObject()->setProperty("playGeneration",(juce::int64)++playbackGeneration);
    enqueue(m);
}
uint64_t AudioClient::playDesign(const juce::String &id,const juce::File &snapshot,int64_t position,const juce::File &capture)
{
    auto m=message("play_design");auto *o=m.getDynamicObject();auto generation=++playbackGeneration;o->setProperty("id",id);o->setProperty("path",snapshot.getFullPathName());o->setProperty("positionSample",(juce::int64)position);o->setProperty("capture",capture==juce::File() ? juce::String() : capture.getFullPathName());o->setProperty("playGeneration",(juce::int64)generation);enqueue(m);return generation;
}
void AudioClient::deviceRate(double rate){auto m=message("device_rate");m.getDynamicObject()->setProperty("rate",rate);enqueue(m);}
void AudioClient::gain(float g)
{
    targetGain = g;
    auto m = message("gain");
    m.getDynamicObject()->setProperty("value", g);
    enqueue(m);
}
void AudioClient::restart()
{
    { std::lock_guard lock(mutex); desiredPlaying = false; desiredId.clear(); }
    restartRequested = true;
}
void AudioClient::device(const juce::String &name)
{
    auto m = message("device");
    m.getDynamicObject()->setProperty("name", name);
    enqueue(m);
}
juce::var AudioClient::status()
{
    std::lock_guard lock(mutex);
    auto copy = latest.clone();
    if (auto *object = copy.getDynamicObject())
    {
        object->setProperty("intentId",desiredId);
        bool awaitingCommand=(uint64_t)(juce::int64)latest["playGeneration"] < playbackGeneration;
        bool pending = desiredPlaying && awaitingCommand;
        object->setProperty("pendingPlayback",pending);
        if (pending) { object->setProperty("id",desiredId); object->setProperty("playing",false); object->setProperty("error",juce::String()); }
        // Stop intent is immediate, even before the audio process acknowledges it.
        // A waveform click in this gap must not restart the previous audition.
        if(awaitingCommand && !desiredPlaying)object->setProperty("playing",false);
    }
    return copy;
}
void AudioClient::run()
{
    int retries = 0;
    int64_t generation = 0;
    while (!quitting)
    {
        int sockets[2];
        pid_t pid = -1;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
            break;
        for (int socket : sockets) fcntl(socket,F_SETFD,FD_CLOEXEC);
        int noSig = 1;
        setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &noSig, sizeof(noSig));
        // stdin is the inherited private channel; the engine has no network listener.
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, sockets[0]);
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
        auto path = executable.getFullPathName().toStdString();
        char *args[] = {path.data(), nullptr};
        int rc = posix_spawn(&pid, path.c_str(), &actions, nullptr, args, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (rc != 0)
        {
            close(sockets[0]);
            std::lock_guard lock(mutex);
            latest = message("status");
            latest.getDynamicObject()->setProperty("error", "Cannot launch audio process");
            break;
        }
        auto fd = sockets[0];
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        auto hello = message("hello");
        sendMessage(fd, hello);
        auto monitor = message("gain");
        monitor.getDynamicObject()->setProperty("value", targetGain.load());
        sendMessage(fd, monitor);
        ++generation;
        juce::String pending;
        bool healthy = true;
        auto lastMessage = juce::Time::getMillisecondCounter();
        while (!quitting && !restartRequested && healthy)
        {
            std::deque<juce::var> commands;
            {
                std::lock_guard lock(mutex);
                commands.swap(outgoing);
            }
            for (auto &cmd : commands)
                if (!sendMessage(fd, cmd))
                {
                    healthy = false;
                    break;
                }
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 30);
            if (p.revents & POLLIN)
            {
                char buffer[8192];
                auto n = recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0)
                {
                    healthy = false;
                    break;
                }
                pending += juce::String::fromUTF8(buffer, (int)n);
                if (pending.length() > 65536)
                {
                    healthy = false;
                    break;
                }
                while (pending.containsChar('\n'))
                {
                    auto line = pending.upToFirstOccurrenceOf("\n", false, false);
                    pending = pending.fromFirstOccurrenceOf("\n", false, false);
                    auto state = juce::JSON::parse(line);
                    if ((int)state["version"] != protocolVersion)
                    {
                        healthy = false;
                        break;
                    }
                    if (auto *object = state.getDynamicObject())
                    {
                        object->setProperty("processGeneration", (juce::int64)generation);
                        if ((uint64_t)(juce::int64)state["playGeneration"] < playbackGeneration)
                        { object->setProperty("playing",false); object->setProperty("pendingPlayback",true); }
                    }
                    {
                        std::lock_guard lock(mutex);
                        latest = state;
                    }
                    lastMessage = juce::Time::getMillisecondCounter();
                }
            }
            if (p.revents & (POLLHUP | POLLERR | POLLNVAL))
                healthy = false;
            if (juce::Time::getMillisecondCounter() - lastMessage > 5000)
                healthy = false;
        }
        sendMessage(fd, message("quit"));
        close(fd);
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 40; ++i)
        {
            if (waitpid(pid, &status, WNOHANG) == pid)
            {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!reaped)
        {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
        bool explicitRestart = restartRequested.exchange(false);
        if (quitting)
            break;
        {
            std::lock_guard lock(mutex);
            outgoing.clear();
            desiredPlaying = false; desiredId.clear();
            latest = message("status");
            latest.getDynamicObject()->setProperty(
                "error", "Audio process restarted. Select a sound to audition again.");
        }
        if (explicitRestart)
            retries = 0;
        else if (++retries >= 3)
        {
            while (!quitting && !restartRequested)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            restartRequested = false;
            retries = 0;
        }
    }
}
} // namespace sp
