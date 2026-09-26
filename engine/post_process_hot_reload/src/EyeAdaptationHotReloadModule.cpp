#include <orbit/hot_reload/ModuleApi.hpp>
#include <orbit/post_process/HumanEyeAdaptationHotReload.hpp>

#include <string_view>

namespace
{
const orbit::hot_reload::HostApi* gHost = nullptr;

bool OnLoad(
    const orbit::hot_reload::HostApi* host,
    const orbit::hot_reload::StateView) noexcept
{
    if (host == nullptr ||
        host->abiVersion != orbit::hot_reload::kHostAbiVersion)
    {
        return false;
    }

    gHost = host;
    if (gHost->log != nullptr)
    {
        gHost->log(
            orbit::hot_reload::HostLogLevel::Info,
            "Human-eye adaptation hot-reload generation activated.");
    }
    return true;
}

std::size_t SaveState(
    void*,
    const std::size_t) noexcept
{
    // Runtime eye state belongs to each viewport, not the DLL. A generation
    // swap therefore requires no module-owned state migration.
    return 0U;
}

const orbit::post_process::HumanEyeAdaptationHotReloadInterface
    kEyeInterface{
        .abiVersion =
            orbit::post_process::
                kHumanEyeAdaptationHotReloadInterfaceVersion,
        .update =
            &orbit::post_process::
                UpdateHumanEyeAdaptationBuiltin
    };

const void* QueryInterface(
    const char* interfaceName,
    const std::uint32_t interfaceVersion) noexcept
{
    if (interfaceName == nullptr ||
        interfaceVersion !=
            orbit::post_process::
                kHumanEyeAdaptationHotReloadInterfaceVersion)
    {
        return nullptr;
    }

    if (std::string_view(interfaceName) !=
        orbit::post_process::
            kHumanEyeAdaptationHotReloadInterfaceName)
    {
        return nullptr;
    }

    return &kEyeInterface;
}

void OnUnload() noexcept
{
    if (gHost != nullptr && gHost->log != nullptr)
    {
        gHost->log(
            orbit::hot_reload::HostLogLevel::Info,
            "Human-eye adaptation hot-reload generation released.");
    }
    gHost = nullptr;
}

const orbit::hot_reload::ModuleApi kModuleApi{
    .abiVersion = orbit::hot_reload::kModuleAbiVersion,
    .moduleName =
        orbit::post_process::
            kHumanEyeAdaptationHotReloadModuleName,
    .stateSchema = 0U,
    .onLoad = &OnLoad,
    .saveState = &SaveState,
    .queryInterface = &QueryInterface,
    .onUnload = &OnUnload
};
} // namespace

ORBIT_HOT_RELOAD_EXPORT
const orbit::hot_reload::ModuleApi* OrbitHotReloadGetModule() noexcept
{
    return &kModuleApi;
}
