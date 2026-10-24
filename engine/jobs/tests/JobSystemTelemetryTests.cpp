#include <orbit/jobs/JobSystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>
#include <vector>

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

    // Background futures come from packaged tasks in the shared bounded pool,
    // rather than std::async, and errors are delivered to the consumer.
    auto answer = orbit::jobs::SubmitBackgroundCpu([] { return 42; });
    Check(answer.get() == 42, "background CPU task delivers its value");
    auto broken = orbit::jobs::SubmitBackgroundCpu([]() -> int {
        throw std::runtime_error("worker failure");
    });
    bool propagated = false;
    try { static_cast<void>(broken.get()); }
    catch (const std::runtime_error&) { propagated = true; }
    Check(propagated, "background CPU task propagates exceptions");
    Check(orbit::jobs::BackgroundCpuPool().Telemetry().workers >= 2U,
          "background CPU pool has bounded named workers");
    orbit::jobs::BackgroundCpuPool().WaitIdle();

    // Saturate the low-priority admission window without blocking the
    // submission thread; reserve headroom for one higher-priority request.
    std::promise<void> releaseBatches;
    auto gate = releaseBatches.get_future().share();
    std::vector<std::future<int>> admitted;
    const auto limit = orbit::jobs::BackgroundCpuAdmission().capacity;
    for (orbit::u64 index = 0U; index < limit + 2U; ++index)
    {
        auto task = orbit::jobs::TrySubmitBackgroundCpu(
            [gate] { gate.wait(); return 7; },
            orbit::jobs::JobPriority::Low);
        if (!task.has_value()) break;
        admitted.push_back(std::move(*task));
    }
    Check(!admitted.empty(), "bounded work admits pending jobs");
    Check(admitted.size() < limit, "low-priority work stops before full capacity");
    Check(orbit::jobs::BackgroundCpuAdmission().outstanding == admitted.size(),
          "admission counts running and queued work");
    const auto rejected = orbit::jobs::TrySubmitBackgroundCpu(
        [] { return 0; }, orbit::jobs::JobPriority::Low);
    Check(!rejected.has_value(), "overload defers rather than waiting");
    auto highPriority = orbit::jobs::TrySubmitBackgroundCpu(
        [] { return 19; }, orbit::jobs::JobPriority::High);
    Check(highPriority.has_value(), "urgent work receives reserved admission");
    releaseBatches.set_value();
    for (auto& task : admitted)
        Check(task.get() == 7, "all admitted jobs finish");
    Check(highPriority->get() == 19, "high priority job finishes");
    orbit::jobs::BackgroundCpuPool().WaitIdle();
    Check(orbit::jobs::BackgroundCpuAdmission().outstanding == 0U,
          "admission capacity is released after completion");
    auto failedAdmitted = orbit::jobs::TrySubmitBackgroundCpu([]() -> int {
        throw std::runtime_error("intentional admitted task failure");
    });
    Check(failedAdmitted.has_value(), "failed job reserves a slot");
    bool caught = false;
    try { static_cast<void>(failedAdmitted->get()); }
    catch (const std::runtime_error&) { caught = true; }
    Check(caught, "admitted worker exceptions reach the future");
    orbit::jobs::BackgroundCpuPool().WaitIdle();
    Check(orbit::jobs::BackgroundCpuAdmission().outstanding == 0U,
          "failed tasks release admission capacity");
    Check(orbit::jobs::BackgroundCpuAdmission().rejected > 0U,
          "admission rejections are counted");

    return EXIT_SUCCESS;
}
