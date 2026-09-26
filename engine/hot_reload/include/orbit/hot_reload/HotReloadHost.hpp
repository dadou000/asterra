#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::hot_reload
{
struct ModuleRegistration
{
    std::string name;
    std::string buildTarget;
    std::filesystem::path binaryPath;
    std::vector<std::filesystem::path> sourceRoots;
    std::chrono::milliseconds debounce{300};
};

struct HotReloadHostConfig
{
    std::filesystem::path sourceRoot;
    std::filesystem::path binaryRoot;
    std::filesystem::path runtimeCopyRoot;
    std::string buildConfiguration{"Debug"};
    std::chrono::milliseconds pollInterval{250};
    bool automaticBuilds{true};
};

using InterfaceVisitor =
    void (*)(const void* interfacePointer, void* userData) noexcept;

class HotReloadHost
{
public:
    explicit HotReloadHost(HotReloadHostConfig config);
    ~HotReloadHost();

    HotReloadHost(const HotReloadHost&) = delete;
    HotReloadHost& operator=(const HotReloadHost&) = delete;

    void RegisterModule(ModuleRegistration registration);

    void Start();
    void Stop() noexcept;

    // Queues a rebuild/reload even when no file-system change was observed.
    [[nodiscard]] bool RequestReload(std::string_view moduleName);

    // Visits an interface while its owning DLL is pinned by the host lock.
    // The callback must not retain the pointer after it returns.
    [[nodiscard]] bool VisitInterface(
        std::string_view moduleName,
        const char* interfaceName,
        std::uint32_t interfaceVersion,
        InterfaceVisitor visitor,
        void* userData);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::hot_reload
