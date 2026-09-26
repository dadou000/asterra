#include <orbit/hot_reload/HotReloadHost.hpp>
#include <orbit/hot_reload/ModuleApi.hpp>

#include <orbit/core/Log.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orbit::hot_reload
{
namespace
{
[[nodiscard]] std::string SanitizeName(std::string value)
{
    for (char& character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '_' && character != '-')
        {
            character = '_';
        }
    }
    return value;
}

void HostLog(const HostLogLevel level, const char* message) noexcept
{
    if (message == nullptr)
    {
        return;
    }

    switch (level)
    {
    case HostLogLevel::Warning:
        orbit::log::Warning(message);
        break;
    case HostLogLevel::Error:
        orbit::log::Error(message);
        break;
    case HostLogLevel::Info:
    default:
        orbit::log::Info(message);
        break;
    }
}

[[nodiscard]] std::filesystem::file_time_type LatestSourceWrite(
    const std::vector<std::filesystem::path>& roots)
{
    auto latest = std::filesystem::file_time_type::min();

    for (const auto& root : roots)
    {
        std::error_code error;
        if (!std::filesystem::exists(root, error))
        {
            continue;
        }

        if (std::filesystem::is_regular_file(root, error))
        {
            const auto write = std::filesystem::last_write_time(root, error);
            if (!error)
            {
                latest = (std::max)(latest, write);
            }
            continue;
        }

        error.clear();
        std::filesystem::recursive_directory_iterator iterator(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;

        while (!error && iterator != end)
        {
            if (iterator->is_regular_file(error))
            {
                const auto write = iterator->last_write_time(error);
                if (!error)
                {
                    latest = (std::max)(latest, write);
                }
            }

            iterator.increment(error);
        }
    }

    return latest;
}

[[nodiscard]] std::wstring Quote(const std::filesystem::path& path)
{
    return L"\"" + path.wstring() + L"\"";
}
} // namespace

class HotReloadHost::Impl
{
public:
    explicit Impl(HotReloadHostConfig config)
        : config_(std::move(config))
    {
        hostApi_.abiVersion = kHostAbiVersion;
        hostApi_.log = &HostLog;

        if (config_.runtimeCopyRoot.empty())
        {
            config_.runtimeCopyRoot = config_.binaryRoot / "hot_reload_runtime";
        }

        runtimeProcessRoot_ =
            config_.runtimeCopyRoot /
            std::format("process_{}", GetCurrentProcessId());
    }

    ~Impl()
    {
        Stop();
        std::scoped_lock lock(modulesMutex_);
        for (auto& module : modules_)
        {
            Unload(module);
        }

        std::error_code error;
        std::filesystem::remove_all(runtimeProcessRoot_, error);
    }

    void RegisterModule(ModuleRegistration registration)
    {
        if (registration.name.empty() || registration.buildTarget.empty())
        {
            throw std::invalid_argument(
                "Hot-reload modules require a name and build target.");
        }

        if (registration.binaryPath.is_relative())
        {
            registration.binaryPath = config_.binaryRoot / registration.binaryPath;
        }

        for (auto& sourceRoot : registration.sourceRoots)
        {
            if (sourceRoot.is_relative())
            {
                sourceRoot = config_.sourceRoot / sourceRoot;
            }
        }

        std::scoped_lock lock(modulesMutex_);
        modules_.push_back(ModuleRuntime{
            .registration = std::move(registration)
        });
    }

    void Start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true))
        {
            return;
        }

        worker_ = std::jthread(
            [this](const std::stop_token stopToken)
            {
                WorkerMain(stopToken);
            });
    }

    void Stop() noexcept
    {
        if (!running_.exchange(false))
        {
            return;
        }

        if (worker_.joinable())
        {
            worker_.request_stop();
            worker_.join();
        }
    }

    [[nodiscard]] bool RequestReload(const std::string_view name)
    {
        std::scoped_lock lock(modulesMutex_);
        const auto iterator = std::find_if(
            modules_.begin(),
            modules_.end(),
            [name](const ModuleRuntime& module)
            {
                return module.registration.name == name;
            });

        if (iterator == modules_.end())
        {
            return false;
        }

        iterator->forceReload = true;
        return true;
    }

private:
    struct ModuleRuntime
    {
        ModuleRegistration registration;
        std::filesystem::file_time_type lastObservedWrite{
            std::filesystem::file_time_type::min()};
        std::optional<std::chrono::steady_clock::time_point> pendingSince;
        bool forceReload{false};
        std::uint64_t generation{0U};
        HMODULE library{nullptr};
        const ModuleApi* api{nullptr};
        std::filesystem::path loadedCopy;
    };

    void WorkerMain(const std::stop_token stopToken)
    {
        {
            std::scoped_lock lock(modulesMutex_);
            std::error_code directoryError;
            std::filesystem::create_directories(runtimeProcessRoot_, directoryError);

            for (auto& module : modules_)
            {
                module.lastObservedWrite = LatestSourceWrite(module.registration.sourceRoots);

                if (!std::filesystem::exists(module.registration.binaryPath) &&
                    config_.automaticBuilds)
                {
                    if (!Build(module))
                    {
                        continue;
                    }
                }

                (void)LoadGeneration(module);
            }
        }

        orbit::log::Info("Orbit native hot-reload host is active.");

        while (!stopToken.stop_requested())
        {
            {
                std::scoped_lock lock(modulesMutex_);
                const auto now = std::chrono::steady_clock::now();

                for (auto& module : modules_)
                {
                    const auto latest = LatestSourceWrite(module.registration.sourceRoots);
                    if (latest > module.lastObservedWrite)
                    {
                        module.lastObservedWrite = latest;
                        module.pendingSince = now;
                        orbit::log::Info(std::format(
                            "Hot reload detected a change in {}.",
                            module.registration.name));
                    }

                    const bool debounceElapsed =
                        module.pendingSince.has_value() &&
                        now - *module.pendingSince >= module.registration.debounce;

                    if (!module.forceReload && !debounceElapsed)
                    {
                        continue;
                    }

                    module.forceReload = false;
                    module.pendingSince.reset();

                    if (!config_.automaticBuilds || Build(module))
                    {
                        (void)LoadGeneration(module);
                    }
                }
            }

            std::this_thread::sleep_for(config_.pollInterval);
        }
    }

    [[nodiscard]] bool Build(const ModuleRuntime& module) const
    {
        orbit::log::Info(std::format(
            "Hot reload building CMake target {}...",
            module.registration.buildTarget));

        std::error_code error;
        std::filesystem::create_directories(runtimeProcessRoot_, error);
        const auto logPath =
            runtimeProcessRoot_ /
            (SanitizeName(module.registration.name) + "_build.log");

        HANDLE logHandle = CreateFileW(
            logPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;

        BOOL inheritHandles = FALSE;
        if (logHandle != INVALID_HANDLE_VALUE)
        {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdOutput = logHandle;
            startup.hStdError = logHandle;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            (void)SetHandleInformation(
                logHandle,
                HANDLE_FLAG_INHERIT,
                HANDLE_FLAG_INHERIT);
            inheritHandles = TRUE;
        }

        PROCESS_INFORMATION process{};
        std::wstring command =
            L"cmake --build " + Quote(config_.binaryRoot) +
            L" --config \"" +
            std::wstring(config_.buildConfiguration.begin(), config_.buildConfiguration.end()) +
            L"\" --target \"" +
            std::wstring(module.registration.buildTarget.begin(), module.registration.buildTarget.end()) +
            L"\" --parallel";
        command.push_back(L'\0');

        const BOOL created = CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            inheritHandles,
            CREATE_NO_WINDOW,
            nullptr,
            config_.sourceRoot.c_str(),
            &startup,
            &process);

        if (logHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(logHandle);
        }

        if (!created)
        {
            orbit::log::Error(std::format(
                "Hot reload could not start CMake for {} (Win32 error {}).",
                module.registration.name,
                GetLastError()));
            return false;
        }

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1U;
        (void)GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (exitCode != 0U)
        {
            orbit::log::Error(std::format(
                "Hot reload build failed for {}. Previous generation remains live. Build log: {}",
                module.registration.name,
                logPath.string()));
            return false;
        }

        orbit::log::Info(std::format(
            "Hot reload build completed for {}.",
            module.registration.name));
        return true;
    }

    [[nodiscard]] bool LoadGeneration(ModuleRuntime& module)
    {
        if (!std::filesystem::exists(module.registration.binaryPath))
        {
            orbit::log::Warning(std::format(
                "Hot reload binary is missing for {}: {}",
                module.registration.name,
                module.registration.binaryPath.string()));
            return false;
        }

        const auto nextGeneration = module.generation + 1U;
        const auto moduleDirectory =
            runtimeProcessRoot_ / SanitizeName(module.registration.name);
        std::error_code error;
        std::filesystem::create_directories(moduleDirectory, error);

        const auto generationCopy = moduleDirectory / std::format(
            "{}_{:06}.dll",
            SanitizeName(module.registration.name),
            nextGeneration);

        std::filesystem::copy_file(
            module.registration.binaryPath,
            generationCopy,
            std::filesystem::copy_options::overwrite_existing,
            error);

        if (error)
        {
            orbit::log::Error(std::format(
                "Hot reload could not stage {}: {}",
                module.registration.name,
                error.message()));
            return false;
        }

        const HMODULE newLibrary = LoadLibraryW(generationCopy.c_str());
        if (newLibrary == nullptr)
        {
            orbit::log::Error(std::format(
                "Hot reload could not load {} generation {} (Win32 error {}).",
                module.registration.name,
                nextGeneration,
                GetLastError()));
            std::filesystem::remove(generationCopy, error);
            return false;
        }

        const FARPROC symbol = GetProcAddress(newLibrary, kModuleEntryPoint);
        GetModuleApiFn getApi = nullptr;
        static_assert(sizeof(getApi) == sizeof(symbol));
        std::memcpy(&getApi, &symbol, sizeof(getApi));

        if (getApi == nullptr)
        {
            orbit::log::Error(std::format(
                "Hot reload rejected {} because {} is not exported.",
                module.registration.name,
                kModuleEntryPoint));
            FreeLibrary(newLibrary);
            std::filesystem::remove(generationCopy, error);
            return false;
        }

        const ModuleApi* newApi = getApi();
        if (newApi == nullptr || newApi->abiVersion != kModuleAbiVersion ||
            newApi->onLoad == nullptr)
        {
            orbit::log::Error(std::format(
                "Hot reload rejected {} because its module ABI is incompatible.",
                module.registration.name));
            FreeLibrary(newLibrary);
            std::filesystem::remove(generationCopy, error);
            return false;
        }

        std::vector<std::byte> savedState;
        StateView previousState{};
        if (module.api != nullptr && module.api->saveState != nullptr)
        {
            const std::size_t required = module.api->saveState(nullptr, 0U);
            savedState.resize(required);
            if (required != 0U)
            {
                const std::size_t written =
                    module.api->saveState(savedState.data(), savedState.size());
                if (written > savedState.size())
                {
                    orbit::log::Error(std::format(
                        "Hot reload rejected {} because its previous generation returned an invalid state size.",
                        module.registration.name));
                    FreeLibrary(newLibrary);
                    std::filesystem::remove(generationCopy, error);
                    return false;
                }
                savedState.resize(written);
            }

            previousState = {
                .schema = module.api->stateSchema,
                .data = savedState.empty() ? nullptr : savedState.data(),
                .size = savedState.size()
            };
        }

        if (!newApi->onLoad(&hostApi_, previousState))
        {
            orbit::log::Error(std::format(
                "Hot reload generation {} of {} declined activation. Previous generation remains live.",
                nextGeneration,
                module.registration.name));
            if (newApi->onUnload != nullptr)
            {
                newApi->onUnload();
            }
            FreeLibrary(newLibrary);
            std::filesystem::remove(generationCopy, error);
            return false;
        }

        const HMODULE previousLibrary = module.library;
        const ModuleApi* previousApi = module.api;
        const auto previousCopy = module.loadedCopy;

        module.library = newLibrary;
        module.api = newApi;
        module.loadedCopy = generationCopy;
        module.generation = nextGeneration;

        if (previousApi != nullptr && previousApi->onUnload != nullptr)
        {
            previousApi->onUnload();
        }
        if (previousLibrary != nullptr)
        {
            FreeLibrary(previousLibrary);
        }
        if (!previousCopy.empty())
        {
            std::filesystem::remove(previousCopy, error);
        }

        orbit::log::Info(std::format(
            "Hot reload activated {} generation {} without restarting Orbit.",
            module.registration.name,
            nextGeneration));
        return true;
    }

    void Unload(ModuleRuntime& module) noexcept
    {
        if (module.api != nullptr && module.api->onUnload != nullptr)
        {
            module.api->onUnload();
        }
        module.api = nullptr;

        if (module.library != nullptr)
        {
            FreeLibrary(module.library);
            module.library = nullptr;
        }

        if (!module.loadedCopy.empty())
        {
            std::error_code error;
            std::filesystem::remove(module.loadedCopy, error);
            module.loadedCopy.clear();
        }
    }

    HotReloadHostConfig config_;
    HostApi hostApi_{};
    std::filesystem::path runtimeProcessRoot_;
    std::mutex modulesMutex_;
    std::vector<ModuleRuntime> modules_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
};

HotReloadHost::HotReloadHost(HotReloadHostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

HotReloadHost::~HotReloadHost() = default;

void HotReloadHost::RegisterModule(ModuleRegistration registration)
{
    impl_->RegisterModule(std::move(registration));
}

void HotReloadHost::Start()
{
    impl_->Start();
}

void HotReloadHost::Stop() noexcept
{
    impl_->Stop();
}

bool HotReloadHost::RequestReload(const std::string_view moduleName)
{
    return impl_->RequestReload(moduleName);
}
} // namespace orbit::hot_reload
