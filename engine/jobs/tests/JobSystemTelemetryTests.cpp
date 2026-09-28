#include <orbit/jobs/JobSystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "JobSystem telemetry test failed: "
                  << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main()
{
    orbit::jobs::JobSystem jobs(1);
    std::atomic<bool> firstRunning{false};
    std::atomic<bool> releaseFirst{false};

    jobs.Submit([&]
    {
        firstRunning.store(true, std::memory_order_release);
        while (!releaseFirst.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!firstRunning.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    Check(firstRunning.load(std::memory_order_acquire), "first job reached running state");

    jobs.Submit([] {});

    orbit::jobs::JobSystemTelemetry active{};
    while (std::chrono::steady_clock::now() < deadline)
    {
        active = jobs.Telemetry();
        if (active.running >= 1U &&
            active.queued >= 1U &&
            active.outstanding >= 2U)
        {
            break;
        }
        std::this_thread::yield();
    }

    Check(active.workers == 1U, "worker count is reported");
    Check(active.running >= 1U, "running work is reported");
    Check(active.queued >= 1U, "queued work is reported");
    Check(active.outstanding >= 2U, "outstanding includes queued and running work");

    releaseFirst.store(true, std::memory_order_release);
    jobs.WaitIdle();

    const auto idle = jobs.Telemetry();
    Check(idle.outstanding == 0U, "idle outstanding count returns to zero");
    Check(idle.queued == 0U, "idle queued count returns to zero");
    Check(idle.running == 0U, "idle running count returns to zero");
    return EXIT_SUCCESS;
}
