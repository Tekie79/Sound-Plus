#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sp
{
class SerialWorker
{
  public:
    SerialWorker() : thread([this] { run(); }) {}
    ~SerialWorker()
    {
        finish({});
    }
    bool submit(std::function<void()> job)
    {
        std::lock_guard lock(mutex);
        if (closing || jobs.size() >= 64)
            return false;
        jobs.push_back(std::move(job));
        wake.notify_one();
        return true;
    }
    void finish(std::function<void()> finalJob)
    {
        {
            std::lock_guard lock(mutex);
            if (!closing)
            {
                closing = true;
                final = std::move(finalJob);
            }
        }
        wake.notify_one();
        if (thread.joinable())
            thread.join();
    }

  private:
    void run()
    {
        for (;;)
        {
            std::function<void()> job;
            bool last = false;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return closing || !jobs.empty(); });
                if (!jobs.empty())
                {
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                else
                {
                    job = std::move(final);
                    last = true;
                }
            }
            if (job)
                job();
            if (last)
                return;
        }
    }
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::function<void()> final;
    bool closing = false;
    std::thread thread;
};
} // namespace sp
