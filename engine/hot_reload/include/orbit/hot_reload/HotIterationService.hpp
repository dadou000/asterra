#pragma once

#include <orbit/hot_reload/ChangeClassifier.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orbit::hot_reload
{
struct HotIterationEvent
{
    ChangeKind kind{ChangeKind::Ignored};
    std::filesystem::path path;
};

// Drains non-native changes observed by the development watcher. Engine/editor
// services can consume these events without coupling the low-level watcher to
// content, shader, or plugin implementations.
[[nodiscard]] std::vector<HotIterationEvent>
DrainHotIterationEvents();

using HotIterationHandlerId = std::uint64_t;
using HotIterationHandler =
    std::function<void(const HotIterationEvent&)>;

// Handlers are dispatched only when PumpHotIterationEvents() is called. Studio
// pumps on its UI thread so content, shader, and editor services never mutate
// live state from the file-watcher worker thread.
[[nodiscard]] HotIterationHandlerId
AddHotIterationHandler(
    ChangeKind kind,
    HotIterationHandler handler);

void RemoveHotIterationHandler(
    HotIterationHandlerId id) noexcept;

[[nodiscard]] std::uint32_t
PumpHotIterationEvents();

struct HotIterationConfig
{
    std::filesystem::path sourceRoot;
    std::filesystem::path binaryRoot;
    std::filesystem::path developmentBinary;
    std::filesystem::path runtimeRoot;
    std::string buildConfiguration{"Debug"};
    std::string fallbackBuildTarget{"OrbitStudio"};
    std::chrono::milliseconds pollInterval{50};
    std::chrono::milliseconds fallbackDebounce{350};
    bool automaticFallbackRelaunch{true};
    std::vector<std::string> relaunchArguments;
};

// Watches all development sources once, routes files that already belong to a
// native hot module away from the fallback builder, publishes direct resource
// changes, and automatically rebuilds/relaunches Studio when a change crosses
// the tiny immutable host boundary.
class HotIterationService
{
public:
    explicit HotIterationService(HotIterationConfig config);
    ~HotIterationService();

    HotIterationService(const HotIterationService&) = delete;
    HotIterationService& operator=(const HotIterationService&) = delete;

    // Extra roots are useful for the active game project when it lives outside
    // the Orbit repository. Registration is safe before or after Start().
    void AddWatchRoot(std::filesystem::path root);

    // Native files underneath these roots are owned by an in-process DLL hot
    // module and must never trigger the process-level fallback rebuild.
    void AddNativeHotRoot(std::filesystem::path root);

    void Start();
    void Stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The Studio bootstrap publishes its active service here. Other engine systems
// can then register their project roots without depending on the editor app.
void SetActiveHotIterationService(
    HotIterationService* service) noexcept;

void AddHotIterationWatchRoot(
    const std::filesystem::path& root);
} // namespace orbit::hot_reload
