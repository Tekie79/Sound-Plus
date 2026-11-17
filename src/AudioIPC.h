#pragma once
#include <juce_core/juce_core.h>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
namespace sp
{
constexpr int protocolVersion = 1;
juce::var message(const juce::String &command);
bool sendMessage(int fd, const juce::var &);
// Private inherited socketpair: no listening port or discoverable endpoint.
class AudioClient
{
  public:
    explicit AudioClient(juce::File executable);
    ~AudioClient();
    void play(const juce::String &id, const juce::File &);
    void stop();
    void gain(float);
    void restart();
    void device(const juce::String &);
    juce::var status();

  private:
    void enqueue(juce::var);
    void run();
    juce::File executable;
    std::atomic<bool> quitting{false}, restartRequested{false};
    std::atomic<float> targetGain{0.7f};
    std::thread worker;
    std::mutex mutex;
    std::deque<juce::var> outgoing;
    juce::var latest;
};
} // namespace sp
