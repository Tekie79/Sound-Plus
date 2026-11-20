#include "AudioIPC.h"
#include "DesignLive.h"
#include "Performance.h"
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
    // ASCII JSON preserves Unicode via escapes even when socket reads split a UTF-8 codepoint.
    auto line = juce::JSON::toString(msg, juce::JSON::FormatOptions()
                                              .withSpacing(juce::JSON::Spacing::singleLine)
                                              .withEncoding(juce::JSON::Encoding::ascii)) +
                "\n";
    auto bytes = line.getNumBytesAsUTF8();
    // Non-real-time workers only. A bounded frame is not guaranteed to fit a
    // nonblocking stream socket in one write, even when the peer is healthy.
    if (bytes > 65536)
    {
        errno = EMSGSIZE;
        return false;
    }
    auto deadline = monotonicMs() + 250;
    size_t sent = 0;
    while (sent < bytes)
    {
        auto n = send(fd, line.toRawUTF8() + sent, bytes - sent, MSG_DONTWAIT);
        if (n > 0)
        {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            int remaining = (int)std::ceil(deadline - monotonicMs());
            if (remaining <= 0)
            {
                errno = ETIMEDOUT;
                return false;
            }
            pollfd writable{fd, POLLOUT, 0};
            auto ready = poll(&writable, 1, std::min(remaining, 10));
            if (ready < 0 && errno != EINTR)
                return false;
            if (ready > 0 && (writable.revents & (POLLERR | POLLHUP | POLLNVAL)))
            {
                errno = EPIPE;
                return false;
            }
            continue;
        }
        if (n == 0)
            errno = EPIPE;
        return false;
    }
    return true;
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
    enqueueLocked(std::move(v));
}
void AudioClient::enqueueLocked(juce::var v)
{
    const auto command = v["command"].toString();
    if (command == "play" || command == "play_design")
    {
        desiredPlaying = true;
        restartNotice.clear();
        desiredId = v["id"].toString();
    }
    if (command == "stop")
    {
        desiredPlaying = false;
        desiredId.clear();
    }
    if (command == "play" || command == "play_design" || command == "stop")
        std::erase_if(outgoing,
                      [](const auto &old)
                      {
                          auto name = old["command"].toString();
                          return name == "play" || name == "play_design" || name == "stop";
                      });
    else if (command == "design_parameters")
        std::erase_if(outgoing,
                      [](const auto &old) { return old["command"].toString() == "design_parameters"; });
    else if (command == "gain")
        std::erase_if(outgoing, [](const auto &old) { return old["command"].toString() == "gain"; });
    if (outgoing.size() >= 16)
        outgoing.pop_front();
    outgoing.push_back(std::move(v));
}
void AudioClient::play(const juce::String &id, const juce::File &f)
{
    playRegion(id, f, {});
}
uint64_t AudioClient::playRegion(const juce::String &id, const juce::File &f, const PlaybackRegion &region)
{
    if (captureBusy())
        return playbackGeneration.load();
    auto m = message("play");
    m.getDynamicObject()->setProperty("id", id);
    m.getDynamicObject()->setProperty("path", f.getFullPathName());
    auto generation = ++playbackGeneration;
    auto *o = m.getDynamicObject();
    o->setProperty("playGeneration", (juce::int64)generation);
    o->setProperty("startSample", (juce::int64)region.startSample);
    o->setProperty("endSample", (juce::int64)region.endSample);
    o->setProperty("positionSample", (juce::int64)region.positionSample);
    o->setProperty("auditionPair", region.auditionPair);
    o->setProperty("sourceLayoutOverride", region.sourceLayoutOverride);
    o->setProperty("loop", region.loop);
    o->setProperty("size", (juce::int64)region.size);
    o->setProperty("modified", (juce::int64)region.modified);
    o->setProperty("inode", (juce::int64)region.inode);
    o->setProperty("revision", (juce::int64)region.revision);
    enqueue(m);
    return generation;
}
void AudioClient::stop()
{
    if (captureBusy())
        return; // Only the explicit Designer take Stop/Cancel owns capture.
    auto m = message("stop");
    m.getDynamicObject()->setProperty("playGeneration", (juce::int64)++playbackGeneration);
    enqueue(m);
}
bool AudioClient::stopIfOwned(const juce::String &id, uint64_t generation)
{
    std::lock_guard lock(mutex);
    if (id.isEmpty() || !generation || captureBusy() || desiredId != id || playbackGeneration != generation)
        return false;
    auto m = message("stop");
    m.getDynamicObject()->setProperty("playGeneration", (juce::int64)++playbackGeneration);
    enqueueLocked(m);
    return true;
}
uint64_t AudioClient::playDesign(const juce::String &id, const juce::File &snapshot, int64_t position,
                                 const juce::File &capture, const juce::String &mode)
{
    if (captureOwned || (captureReserved && mode != "record"))
        return 0;
    if (mode == "record")
        captureOwned = true;
    auto m = message("play_design");
    auto *o = m.getDynamicObject();
    auto generation = ++playbackGeneration;
    o->setProperty("id", id);
    o->setProperty("path", snapshot.getFullPathName());
    o->setProperty("positionSample", (juce::int64)position);
    o->setProperty("capture", capture == juce::File() ? juce::String() : capture.getFullPathName());
    o->setProperty("playGeneration", (juce::int64)generation);
    o->setProperty("mode", mode);
    o->setProperty("deviceFaultEpoch", status()["deviceFaultEpoch"]);
    enqueue(m);
    return generation;
}
uint64_t AudioClient::updateDesign(const DesignDocument &document, uint64_t generation, double eventMs)
{
    auto command = message("design_parameters");
    auto *object = command.getDynamicObject();
    object->setProperty("id", "design:" + document.id);
    object->setProperty("playbackContext", designPlaybackContext(document));
    object->setProperty("playGeneration", (juce::int64)generation);
    auto sequence = ++parameterSequence;
    object->setProperty("sequence", (juce::int64)sequence);
    object->setProperty("eventMs", eventMs);
    object->setProperty("parameters", designParameters(document));
    object->setProperty("performControls", controlsToJson(mixControls(document.activeMix())));
    object->setProperty("structure", designStructure(document));
    enqueue(command);
    return sequence;
}
void AudioClient::deviceRate(double rate)
{
    if (captureBusy())
        return;
    auto m = message("device_rate");
    m.getDynamicObject()->setProperty("rate", rate);
    enqueue(m);
}
void AudioClient::gain(float g)
{
    targetGain = g;
    auto m = message("gain");
    m.getDynamicObject()->setProperty("value", g);
    enqueue(m);
}
void AudioClient::restart()
{
    if (captureBusy())
        return;
    {
        std::lock_guard lock(mutex);
        desiredPlaying = false;
        desiredId.clear();
    }
    restartRequested = true;
}
void AudioClient::device(const juce::String &name)
{
    if (captureBusy())
        return;
    auto m = message("device");
    m.getDynamicObject()->setProperty("name", name);
    enqueue(m);
}
void AudioClient::bufferSize(int frames)
{
    if (captureBusy())
        return;
    auto command = message("device_buffer");
    command.getDynamicObject()->setProperty("frames", frames);
    enqueue(command);
}
void AudioClient::finishTake(bool cancel)
{
    if (cancel)
        captureOwned = false;
    auto cmd = message("finish_take");
    cmd.getDynamicObject()->setProperty("cancel", cancel);
    cmd.getDynamicObject()->setProperty("playGeneration", (juce::int64)playbackGeneration.load());
    enqueue(cmd);
}
void AudioClient::deviceChannels(int channels)
{
    if (captureBusy())
        return;
    auto m = message("device_channels");
    m.getDynamicObject()->setProperty("channels", channels);
    enqueue(m);
}
void AudioClient::monitorChannel(int deviceChannel, int sourceChannel)
{
    auto m = message("monitor_map");
    m.getDynamicObject()->setProperty("deviceChannel", deviceChannel);
    m.getDynamicObject()->setProperty("sourceChannel", sourceChannel);
    enqueue(m);
}
void AudioClient::monitorSolo(int sourceChannel)
{
    auto m = message("monitor_solo");
    m.getDynamicObject()->setProperty("sourceChannel", sourceChannel);
    enqueue(m);
}
void AudioClient::reserveTake(bool reserve)
{
    captureReserved = reserve;
    if (reserve)
        ++captureReservationId;
    auto cmd = message("reserve_capture");
    cmd.getDynamicObject()->setProperty("reserve", reserve);
    cmd.getDynamicObject()->setProperty("reservationId", (juce::int64)captureReservationId.load());
    cmd.getDynamicObject()->setProperty("deviceFaultEpoch", status()["deviceFaultEpoch"]);
    enqueue(cmd);
}
juce::var AudioClient::status()
{
    std::lock_guard lock(mutex);
    auto copy = latest.isObject() ? latest.clone() : message("status");
    if (auto *object = copy.getDynamicObject())
    {
        object->setProperty("intentId", desiredId);
        object->setProperty("captureReservationId", (juce::int64)captureReservationId.load());
        object->setProperty("captureReserved", captureReserved.load());
        object->setProperty("intentGeneration", (juce::int64)playbackGeneration.load());
        if (restartNotice.isNotEmpty() && copy["error"].toString().isEmpty())
            object->setProperty("error", restartNotice);
        object->setProperty("helperRestarts", healthEvents.size());
        bool awaitingCommand = (uint64_t)(juce::int64)latest["playGeneration"] < playbackGeneration;
        bool pending = desiredPlaying && awaitingCommand;
        object->setProperty("pendingPlayback", pending);
        if (pending)
        {
            object->setProperty("id", desiredId);
            object->setProperty("playing", false);
            object->setProperty("completed", false);
            object->setProperty("error", juce::String());
        }
        // Stop intent is immediate, even before the audio process acknowledges it.
        // A waveform click in this gap must not restart the previous audition.
        if (awaitingCommand && !desiredPlaying)
            object->setProperty("playing", false);
    }
    return copy;
}
juce::var AudioClient::diagnostics()
{
    std::lock_guard lock(mutex);
    auto copy = health.clone();
    copy.getDynamicObject()->setProperty("events", healthEvents);
    copy.getDynamicObject()->setProperty("observedMs", monotonicMs());
    return copy;
}
void AudioClient::run()
{
    int retries = 0;
    int64_t generation = 0, commandSequence = 0;
    while (!quitting)
    {
        int sockets[2], errors[2];
        pid_t pid = -1;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
            break;
        if (pipe(errors) != 0)
        {
            close(sockets[0]);
            close(sockets[1]);
            break;
        }
        for (int fd : {sockets[0], sockets[1], errors[0], errors[1]})
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        int noSig = 1;
        setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &noSig, sizeof(noSig));
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
        for (int fd : {sockets[0], sockets[1], errors[0], errors[1]})
            posix_spawn_file_actions_addclose(&actions, fd);
        auto path = executable.getFullPathName().toStdString();
        char *args[] = {path.data(), nullptr};
        int rc = posix_spawn(&pid, path.c_str(), &actions, nullptr, args, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        close(errors[1]);
        if (rc != 0)
        {
            close(sockets[0]);
            close(errors[0]);
            std::lock_guard lock(mutex);
            latest = message("status");
            latest.getDynamicObject()->setProperty("error", "Cannot launch audio process");
            break;
        }
        int fd = sockets[0];
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(errors[0], F_SETFL, fcntl(errors[0], F_GETFL) | O_NONBLOCK);
        ++generation;
        juce::String pending, stderrTail, lastCommand;
        bool healthy = true;
        const char *reason = "none";
        int failureErrno = 0;
        double lastStatusMs = monotonicMs(), lastCommandMs = 0;
        uint64_t received = 0;
        juce::var observed;
        auto drainErrors = [&]
        {
            char b[2048];
            for (int i = 0; i < 8; ++i)
            {
                auto n = read(errors[0], b, sizeof(b));
                if (n <= 0)
                    break;
                stderrTail += juce::String::fromUTF8(b, (int)n);
                if (stderrTail.length() > 16384)
                    stderrTail = stderrTail.getLastCharacters(16384);
            }
        };
        auto send = [&](juce::var cmd)
        {
            lastCommand = cmd["command"].toString();
            cmd.getDynamicObject()->setProperty("ipcCommandSequence", (juce::int64)++commandSequence);
            lastCommandMs = monotonicMs();
            if (!sendMessage(fd, cmd))
            {
                failureErrno = errno;
                reason = "command write failed";
                healthy = false;
                return false;
            }
            return true;
        };
        send(message("hello"));
        auto monitor = message("gain");
        monitor.getDynamicObject()->setProperty("value", targetGain.load());
        send(monitor);
        while (!quitting && !restartRequested && healthy)
        {
            std::deque<juce::var> commands;
            {
                std::lock_guard lock(mutex);
                commands.swap(outgoing);
            }
            for (auto &cmd : commands)
                if (!send(cmd))
                    break;
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 10);
            drainErrors();
            if (healthy && (p.revents & POLLIN))
            {
                char b[8192];
                auto n = recv(fd, b, sizeof(b), 0);
                if (n <= 0)
                {
                    failureErrno = n < 0 ? errno : 0;
                    reason = "socket closed";
                    healthy = false;
                }
                else
                {
                    pending += juce::String::fromUTF8(b, (int)n);
                    if (pending.length() > 131072)
                    {
                        reason = "oversized response";
                        healthy = false;
                    }
                    while (healthy && pending.containsChar('\n'))
                    {
                        auto line = pending.upToFirstOccurrenceOf("\n", false, false);
                        pending = pending.fromFirstOccurrenceOf("\n", false, false);
                        auto state = juce::JSON::parse(line);
                        if ((int)state["version"] != protocolVersion)
                        {
                            reason = "invalid status response";
                            healthy = false;
                            break;
                        }
                        lastStatusMs = monotonicMs();
                        ++received;
                        observed = state.clone();
                        if (auto *o = state.getDynamicObject())
                        {
                            o->setProperty("processGeneration", (juce::int64)generation);
                            if ((uint64_t)(int64_t)state["playGeneration"] < playbackGeneration)
                            {
                                o->setProperty("playing", false);
                                o->setProperty("pendingPlayback", true);
                            }
                        }
                        std::lock_guard lock(mutex);
                        latest = state;
                        if ((bool)state["captureLifecycleFailed"] && captureReserved &&
                            (uint64_t)(int64_t)state["deviceReservationId"] == captureReservationId.load())
                            captureOwned = false;
                        if ((uint64_t)(int64_t)state["playGeneration"] == playbackGeneration.load() &&
                            ((state.hasProperty("takePhase") && (int)state["takePhase"] >= 3) ||
                             state["error"].toString().isNotEmpty()))
                            captureOwned = false;
                        auto *h = health.getDynamicObject();
                        h->setProperty("helperPid", (int)pid);
                        h->setProperty("processGeneration", generation);
                        h->setProperty("lastHeartbeatMs", lastStatusMs);
                        h->setProperty("lastCommandMs", lastCommandMs);
                        h->setProperty("sentSequence", commandSequence);
                        h->setProperty("statusesReceived", (juce::int64)received);
                        h->setProperty("lastStatusSequence", observed["statusSequence"]);
                        h->setProperty("lastEchoedCommand", observed["lastCommandSequence"]);
                    }
                }
            }
            if (healthy && (p.revents & (POLLHUP | POLLERR | POLLNVAL)))
            {
                reason = "socket hangup";
                healthy = false;
            }
            if (healthy && monotonicMs() - lastStatusMs > 5000)
            {
                reason = "status heartbeat timed out";
                healthy = false;
            }
        }
        const bool explicitRestart = restartRequested.exchange(false);
        auto failedMs = monotonicMs();
        sendMessage(fd, message("quit"));
        close(fd);
        int status = 0, supervisorSignal = 0;
        bool reaped = false;
        auto reap = [&](int attempts)
        {
            for (int i = 0; i < attempts; ++i)
            {
                if (waitpid(pid, &status, WNOHANG) == pid)
                {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        };
        reap(40);
        if (!reaped)
        {
            supervisorSignal = SIGTERM;
            kill(pid, SIGCONT);
            kill(pid, SIGTERM);
            reap(20);
        }
        if (!reaped)
        {
            supervisorSignal = SIGKILL;
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            reaped = true;
        }
        drainErrors();
        close(errors[0]);
        if (quitting)
            break;
        auto *event = new juce::DynamicObject();
        event->setProperty("helperPid", (int)pid);
        event->setProperty("processGeneration", generation);
        event->setProperty("reason", explicitRestart ? "explicit reconnect" : reason);
        event->setProperty("failureMs", failedMs);
        event->setProperty("lastHeartbeatMs", lastStatusMs);
        event->setProperty("heartbeatAgeMs", failedMs - lastStatusMs);
        event->setProperty("lastCommandMs", lastCommandMs);
        event->setProperty("lastCommand", lastCommand);
        event->setProperty("sentSequence", commandSequence);
        event->setProperty("statusesReceived", (juce::int64)received);
        event->setProperty("pendingResponseBytes", (juce::int64)pending.getNumBytesAsUTF8());
        event->setProperty("errno", failureErrno);
        event->setProperty("exitCode", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        event->setProperty("exitSignal", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        event->setProperty("supervisorSignal", supervisorSignal);
        event->setProperty("stderrTail", stderrTail);
        event->setProperty("lastStatus", observed);
        event->setProperty("classification",
                           explicitRestart                                        ? "explicitReconnect"
                           : juce::String(reason) == "status heartbeat timed out" ? "watchdogTimeout"
                           : WIFSIGNALED(status) && supervisorSignal == 0         ? "helperSignalExit"
                                                                                  : "ipcOrHelperExit");
        juce::var eventValue(event);
        std::fprintf(stderr, "SoundPlus audio health: %s\n",
                     juce::JSON::toString(eventValue, true).toRawUTF8());
        {
            std::lock_guard lock(mutex);
            healthEvents.add(eventValue);
            while (healthEvents.size() > 8)
                healthEvents.remove(0);
            outgoing.clear();
            captureOwned = false;
            desiredPlaying = false;
            desiredId.clear();
            latest = message("status");
            restartNotice = explicitRestart ? "Audio device reconnected. Select a sound to audition again."
                                            : "Audio process restarted. Select a sound to audition again.";
            latest.getDynamicObject()->setProperty("error", restartNotice);
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
