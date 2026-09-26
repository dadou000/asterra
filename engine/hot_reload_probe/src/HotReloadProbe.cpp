#include <orbit/hot_reload/ModuleApi.hpp>

#include <cstdio>
#include <cstring>

namespace
{
const orbit::hot_reload::HostApi* gHost = nullptr;

struct ProbeState
{
    std::uint64_t reloadCount{0U};
};

ProbeState gState{};

void Log(const char* message) noexcept
{
    if (gHost != nullptr && gHost->log != nullptr)
    {
        gHost->log(orbit::hot_reload::HostLogLevel::Info, message);
    }
}

bool OnLoad(
    const orbit::hot_reload::HostApi* host,
    const orbit::hot_reload::StateView previousState) noexcept
{
    if (host == nullptr || host->abiVersion != orbit::hot_reload::kHostAbiVersion)
    {
        return false;
    }

    gHost = host;

    if (previousState.schema == 1U &&
        previousState.data != nullptr &&
        previousState.size == sizeof(ProbeState))
    {
        std::memcpy(&gState, previousState.data, sizeof(gState));
    }

    ++gState.reloadCount;

    char message[160]{};
    std::snprintf(
        message,
        sizeof(message),
        "Orbit hot-reload probe is live; activation count = %llu.",
        static_cast<unsigned long long>(gState.reloadCount));
    Log(message);
    return true;
}

std::size_t SaveState(void* destination, const std::size_t capacity) noexcept
{
    if (destination == nullptr)
    {
        return sizeof(ProbeState);
    }

    if (capacity < sizeof(ProbeState))
    {
        return sizeof(ProbeState);
    }

    std::memcpy(destination, &gState, sizeof(gState));
    return sizeof(ProbeState);
}

void OnUnload() noexcept
{
    Log("Orbit hot-reload probe generation released.");
    gHost = nullptr;
}

const orbit::hot_reload::ModuleApi kModuleApi{
    .abiVersion = orbit::hot_reload::kModuleAbiVersion,
    .moduleName = "OrbitHotReloadProbe",
    .stateSchema = 1U,
    .onLoad = &OnLoad,
    .saveState = &SaveState,
    .queryInterface = nullptr,
    .onUnload = &OnUnload
};
} // namespace

ORBIT_HOT_RELOAD_EXPORT
const orbit::hot_reload::ModuleApi* OrbitHotReloadGetModule() noexcept
{
    return &kModuleApi;
}
