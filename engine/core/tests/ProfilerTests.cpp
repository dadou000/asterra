#include <orbit/core/ThreadName.hpp>
#include <orbit/profiler/Profiler.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;

bool Fail(const char* message)
{
    std::cerr << "FAILED: " << message << "\n";
    return false;
}

[[nodiscard]] std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
} // namespace

int main()
{
    namespace profiler = orbit::profiler;

    const auto directory =
        std::filesystem::temp_directory_path() / "orbit_profiler_test";
    std::filesystem::remove_all(directory);

    profiler::Config config;
    config.hitchThresholdMs = 150.0;
    config.stallThresholdMs = 100.0;
    config.captureWindowMs = 5000.0;
    config.outputDirectory = directory;
    profiler::Configure(config);
    profiler::StartWatchdog();

    // 1. Scopes recorded on several named threads, plus a synthetic lane.
    {
        std::vector<std::thread> threads;
        for (int index = 0; index < 4; ++index)
        {
            threads.emplace_back(
                [index]
                {
                    orbit::core::SetCurrentThreadName(
                        "Orbit.Test." + std::to_string(index));
                    for (int job = 0; job < 50; ++job)
                    {
                        ORBIT_PROFILE_SCOPE("test.job");
                        ORBIT_PROFILE_SCOPE("test.inner");
                        std::this_thread::sleep_for(200us);
                    }
                });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        const auto now = profiler::NowTicks();
        profiler::RecordLaneSpan("Test lane", "lane.span", now - 1000, now);
    }

    const auto manual = directory / "manual.json";
    const auto info = profiler::Capture(manual, 5000.0);
    const std::string text = ReadAll(manual);
    if (info.events < 400U || info.threads < 5U)
    {
        return Fail("capture missed events or threads") ? 0 : 1;
    }
    for (const char* needle :
         {"\"traceEvents\"", "test.job", "test.inner", "lane.span", "Orbit.Test.2",
          "CPU cores", "\"name\":\"Core ", "\"core\":"})
    {
        if (text.find(needle) == std::string::npos)
        {
            std::cerr << "missing in trace: " << needle << "\n";
            return 1;
        }
    }

    // 2. The ring wraps instead of growing without bound.
    {
        std::thread wrapper(
            []
            {
                orbit::core::SetCurrentThreadName("Orbit.Test.Wrap");
                for (int i = 0; i < 100000; ++i)
                {
                    ORBIT_PROFILE_SCOPE("test.wrap");
                }
            });
        wrapper.join();
        const auto wrapped = profiler::Capture(directory / "wrap.json", 5000.0);
        if (wrapped.events > 4U * 16384U + 2000U)
        {
            std::cerr << "ring did not bound events: " << wrapped.events << "\n";
            return 1;
        }
    }

    // 3. A stalled frame writes a hitch capture with stack samples.
    profiler::BeginFrame();
    {
        ORBIT_PROFILE_SCOPE("test.long_work");
        std::this_thread::sleep_for(600ms);
    }
    profiler::EndFrame();

    std::vector<profiler::HitchInfo> hitches;
    for (int attempt = 0; attempt < 60 && hitches.empty(); ++attempt)
    {
        std::this_thread::sleep_for(100ms);
        hitches = profiler::RecentHitches();
    }
    if (hitches.empty())
    {
        std::cerr << "no hitch capture was written\n";
        return 1;
    }
    const auto& hitch = hitches.front();
    if (hitch.frameMs < 500.0 || !std::filesystem::exists(hitch.path))
    {
        std::cerr << "hitch record is wrong\n";
        return 1;
    }
    if (hitch.stackSamples < 2U)
    {
        std::cerr << "expected stack samples during the stall, got "
                  << hitch.stackSamples << "\n";
        return 1;
    }
    const std::string hitchText = ReadAll(hitch.path);
    for (const char* needle : {"test.long_work", "STALLED in", "\"stack\":["})
    {
        if (hitchText.find(needle) == std::string::npos)
        {
            std::cerr << "missing in hitch trace: " << needle << "\n";
            return 1;
        }
    }

    // 4. A normal frame is not a hitch, and recent frame times are reported.
    const auto before = profiler::Frames().hitches;
    profiler::BeginFrame();
    std::this_thread::sleep_for(5ms);
    profiler::EndFrame();
    const auto summary = profiler::Frames();
    if (summary.hitches != before || summary.frames < 2U ||
        profiler::RecentFrameMilliseconds(10).empty())
    {
        std::cerr << "frame summary is wrong\n";
        return 1;
    }

    profiler::StopWatchdog();
    std::filesystem::remove_all(directory);
    std::cout << "Orbit profiler tests passed.\n";
    return 0;
}
