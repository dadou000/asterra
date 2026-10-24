#include "StudioGpuSubmitWorker.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#define REQUIRE(test) do { if (!(test)) { \
    std::cerr << "StudioGpuSubmitWorker: " #test " at line " \
              << __LINE__ << '\\n'; return 1; } } while (false)

int main()
{
    using namespace orbit::editor_app;
    using namespace std::chrono_literals;

    // Synchronous compatibility path must not schedule an additional thread.
    {
        StudioGpuSubmitWorker submitter(false);
        bool called = false;
        submitter.Dispatch({.execute = [&] { called = true; },
                            .fenceValue = 3U, .frameSlot = 1U});
        REQUIRE(called);
        REQUIRE(submitter.Poll());
        submitter.Drain();
    }

    // The producer owns the packet until the worker acknowledges completion.
    {
        StudioGpuSubmitWorker submitter(true);
        std::promise<void> release;
        auto gate = release.get_future().share();
        std::atomic<bool> running{false};
        const auto producerThread = std::this_thread::get_id();
        std::atomic<bool> ranOnProducer{false};
        submitter.Dispatch({.execute = [&] {
            ranOnProducer.store(
                std::this_thread::get_id() == producerThread);
            running.store(true);
            gate.wait();
        }, .fenceValue = 17U, .frameSlot = 2U});
        const auto began = std::chrono::steady_clock::now();
        while (!running.load() &&
               std::chrono::steady_clock::now() - began < 3s)
            std::this_thread::sleep_for(1ms);
        REQUIRE(running.load());
        REQUIRE(!submitter.Poll());

        bool rejectedOverwrite = false;
        try
        {
            submitter.Dispatch({.execute = [] {}, .fenceValue = 18U});
        }
        catch (const std::logic_error&)
        {
            rejectedOverwrite = true;
        }
        REQUIRE(rejectedOverwrite);

        release.set_value();
        std::optional<SubmissionResult> result;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!submitter.Poll(&result) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        REQUIRE(result.has_value());
        REQUIRE(result->fenceValue == 17U);
        REQUIRE(result->frameSlot == 2U);
        REQUIRE(result->workerMilliseconds >= 0.0);
        REQUIRE(!ranOnProducer.load());
        REQUIRE(submitter.Poll());
    }

    // Worker exceptions belong on the owner thread at Poll/Drain time.
    {
        StudioGpuSubmitWorker submitter(true);
        submitter.Dispatch({
            .execute = [] { throw std::runtime_error("intentional submit error"); },
            .fenceValue = 31U
        });
        bool propagated = false;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            try
            {
                if (submitter.Poll()) break;
            }
            catch (const std::runtime_error& error)
            {
                propagated = std::string(error.what()) ==
                    "intentional submit error";
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(propagated);
        REQUIRE(submitter.Poll());
    }

    // Drain is an explicit lifecycle barrier; no captures outlive it.
    {
        std::atomic<bool> executed{false};
        StudioGpuSubmitWorker submitter(true);
        submitter.Dispatch({
            .execute = [&] { executed.store(true); },
            .fenceValue = 100U
        });
        submitter.Drain();
        REQUIRE(executed.load());
        REQUIRE(submitter.Poll());
    }
    return 0;
}
