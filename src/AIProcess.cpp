#include "AIProcess.h"
#include "AudioIPC.h"
#include "Performance.h"
#include "ModelPack.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <fcntl.h>
extern char **environ;
namespace sp
{
AIProcess::AIProcess(juce::File executable, juce::File directory) : model(std::move(directory))
{
    helper = executable.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusAI");
}
AIProcess::~AIProcess() { quitting = true; std::lock_guard lock(mutex); terminate(); }
void AIProcess::terminate()
{
    if (fd >= 0) { close(fd); fd = -1; }
    if (pid > 0)
    {
        int status = 0;
        if (waitpid(pid,&status,WNOHANG) != pid)
        {
            // Retain ownership and reap; a genuinely uninterruptible kernel I/O call may delay reaping.
            kill(pid,SIGKILL);
            while (waitpid(pid,&status,0) < 0 && errno == EINTR) {}
        }
        pid = -1;
    }
}
void AIProcess::retry() { std::lock_guard lock(mutex); failures = 0; terminate(); }
juce::var AIProcess::exchange(const juce::var &command, int timeoutMs)
{
    auto request = ++sequence;
    command.getDynamicObject()->setProperty("sequence",request);
    if (!sendMessage(fd,command)) throw std::runtime_error("Cannot send local AI request");
    std::string input;
    auto until = monotonicMs()+timeoutMs;
    while (!quitting && monotonicMs() < until)
    {
        pollfd p{fd,POLLIN,0};
        poll(&p,1,25);
        if (p.revents & POLLIN)
        {
            char buffer[16384]; auto n = recv(fd,buffer,sizeof(buffer),0);
            if (n <= 0) break;
            input.append(buffer,(size_t)n);
            if (input.size() > 65536) throw std::runtime_error("Invalid local AI response size");
            if (input.find('\n') != std::string::npos)
            {
                auto result = juce::JSON::parse(juce::String::fromUTF8(input.data(),(int)input.find('\n')));
                if ((int)result["version"] != protocolVersion || (int64_t)result["sequence"] != request)
                    throw std::runtime_error("Obsolete or incompatible local AI response");
                result.getDynamicObject()->setProperty("processGeneration",generation.load());
                return result;
            }
        }
        if (p.revents & (POLLHUP|POLLERR|POLLNVAL)) break;
    }
    throw std::runtime_error(quitting ? "Local AI stopped" : "Local AI worker timed out or exited; retry analysis");
}
void AIProcess::start()
{
    if (failures >= 3) throw std::runtime_error("Local AI stopped after three worker failures. Choose Retry Analysis.");
    int sockets[2];
    if (socketpair(AF_UNIX,SOCK_STREAM,0,sockets) != 0) throw std::runtime_error("Cannot create private AI channel");
    for (int socket : sockets) fcntl(socket,F_SETFD,FD_CLOEXEC);
    int noSig = 1; setsockopt(sockets[0],SOL_SOCKET,SO_NOSIGPIPE,&noSig,sizeof(noSig));
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,sockets[1],STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions,sockets[0]);
    posix_spawn_file_actions_addclose(&actions,sockets[1]);
    auto path = helper.getFullPathName().toStdString(); char *args[]{path.data(),nullptr};
    auto result = posix_spawn(&pid,path.c_str(),&actions,nullptr,args,environ);
    posix_spawn_file_actions_destroy(&actions); close(sockets[1]);
    if (result != 0) { close(sockets[0]); pid = -1; throw std::runtime_error("Cannot launch bundled AI worker (requires macOS 13.3+)"); }
    fd = sockets[0]; ++generation;
    auto load = message("load"); load.getDynamicObject()->setProperty("modelDirectory",model.getFullPathName());
    auto reply = exchange(load,30000);
    if (reply["error"].toString().isNotEmpty()) throw std::runtime_error(reply["error"].toString().toStdString());
    if (reply["model"].toString()!=modelId() || reply["preprocessing"].toString()!=preprocessingId())
        throw std::runtime_error("Bundled AI worker uses a different model space; rebuild the application bundle");
    lastLoadMs = (double)reply["loadMs"];
}
juce::var AIProcess::call(juce::var command, bool queryPriority)
{
    if (queryPriority) ++waitingQueries;
    else while (waitingQueries > 0 && !quitting) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::unique_lock lock(mutex);
    if (queryPriority) --waitingQueries;
    if (quitting) throw std::runtime_error("Local AI stopped");
    try
    {
        if (fd < 0) start();
        auto result = exchange(command,command["command"].toString() == "audio" ? 15000 : 5000);
        failures = 0;
        return result;
    }
    catch (...) { ++failures; terminate(); throw; }
}
} // namespace sp
