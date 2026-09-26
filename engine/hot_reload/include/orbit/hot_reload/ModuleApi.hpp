#pragma once

#include <cstddef>
#include <cstdint>

namespace orbit::hot_reload
{
inline constexpr std::uint32_t kHostAbiVersion = 1U;
inline constexpr std::uint32_t kModuleAbiVersion = 1U;
inline constexpr char kModuleEntryPoint[] = "OrbitHotReloadGetModule";

enum class HostLogLevel : std::uint32_t
{
    Info = 0U,
    Warning = 1U,
    Error = 2U
};

struct HostApi
{
    std::uint32_t abiVersion{kHostAbiVersion};
    void (*log)(HostLogLevel level, const char* message) noexcept{nullptr};
};

struct StateView
{
    std::uint64_t schema{0U};
    const void* data{nullptr};
    std::size_t size{0U};
};

struct ModuleApi
{
    std::uint32_t abiVersion{kModuleAbiVersion};
    const char* moduleName{nullptr};
    std::uint64_t stateSchema{0U};

    // Called on a newly loaded generation before it replaces the active one.
    // Returning false rejects the generation and leaves the previous module live.
    bool (*onLoad)(const HostApi* host, StateView previousState) noexcept{nullptr};

    // Called first with destination == nullptr to query the required byte count.
    // The second call writes at most capacity bytes and returns bytes written.
    std::size_t (*saveState)(void* destination, std::size_t capacity) noexcept{nullptr};

    // Called only after a replacement generation has been accepted, or at host shutdown.
    void (*onUnload)() noexcept{nullptr};
};

using GetModuleApiFn = const ModuleApi* (*)() noexcept;
} // namespace orbit::hot_reload

#if defined(_WIN32)
#define ORBIT_HOT_RELOAD_EXPORT extern "C" __declspec(dllexport)
#else
#define ORBIT_HOT_RELOAD_EXPORT extern "C"
#endif
