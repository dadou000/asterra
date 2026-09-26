#include <orbit/hot_reload/HotIterationService.hpp>

#include <orbit/core/Log.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orbit::hot_reload
{
namespace
{
std::mutex gEventMutex;
std::vector<HotIterationEvent> gEvents;

void PublishEvent(
    HotIterationEvent event)
{
    std::scoped_lock lock(gEventMutex);
    gEvents.push_back(std::move(event));
}

[[nodiscard]] std::string LowerAscii(
    std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(
                std::tolower(value));
        });
    return value;
}

[[nodiscard]] bool IsGeneratedRelativePath(
    const std::filesystem::path& relative)
{
    if (relative.empty())
    {
        return true;
    }

    const auto first =
        relative.begin();
    if (first == relative.end())
    {
        return true;
    }

    const std::string component =
        LowerAscii(first->string());

    return
        component == ".git" ||
        component == ".vs" ||
        component == "dist" ||
        component == "out" ||
        component == "hot_reload_runtime" ||
        component == "build" ||
        component.starts_with("build-") ||
        component.starts_with("build_") ||
        component.starts_with("cmake-build-");
}

[[nodiscard]] std::filesystem::path NormalizePath(
    const std::filesystem::path& path)
{
    std::error_code error;
    auto normalized =
        std::filesystem::weakly_canonical(
            path,
            error);

    if (!error)
    {
        return normalized;
    }

    error.clear();
    normalized =
        std::filesystem::absolute(
            path,
            error);

    return error
        ? path.lexically_normal()
        : normalized.lexically_normal();
}

[[nodiscard]] bool IsInsideOrEqual(
    const std::filesystem::path& path,
    const std::filesystem::path& root)
{
    const auto normalizedPath =
        NormalizePath(path);
    const auto normalizedRoot =
        NormalizePath(root);

    if (normalizedPath == normalizedRoot)
    {
        return true;
    }

    const auto relative =
        normalizedPath.lexically_relative(
            normalizedRoot);

    if (relative.empty() ||
        relative.is_absolute())
    {
        return false;
    }

    const auto first = relative.begin();
    return
        first != relative.end() &&
        *first != std::filesystem::path("..");
}

[[nodiscard]] std::wstring QuoteCommandArgument(
    const std::wstring_view argument)
{
    // CommandLineToArgvW-compatible quoting. This handles project paths with
    // whitespace and preserves backslashes immediately before quotes.
    std::wstring result;
    result.push_back(L'\"');

    std::size_t backslashes = 0U;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (character == L'\"')
        {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'\"');
            backslashes = 0U;
            continue;
        }

        result.append(backslashes, L'\\');
        backslashes = 0U;
        result.push_back(character);
    }

    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

[[nodiscard]] std::wstring Widen(
    const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int required =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);

    if (required <= 0)
    {
        return std::wstring(
            value.begin(),
            value.end());
    }

    std::wstring result(
        static_cast<std::size_t>(required),
        L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required);
    return result;
}

class DirectoryChangeWatcher
{
public:
    explicit DirectoryChangeWatcher(
        std::filesystem::path root)
        : root_(NormalizePath(root))
    {
        directory_ =
            CreateFileW(
                root_.c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS |
                    FILE_FLAG_OVERLAPPED,
                nullptr);

        if (directory_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error(
                std::format(
                    "Could not watch '{}' (Win32 error {}).",
                    root_.string(),
                    GetLastError()));
        }

        event_ =
            CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                nullptr);

        if (event_ == nullptr)
        {
            const DWORD error =
                GetLastError();
            CloseHandle(directory_);
            directory_ =
                INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                std::format(
                    "Could not create hot-iteration watcher event (Win32 error {}).",
                    error));
        }

        if (!Arm())
        {
            const DWORD error =
                GetLastError();
            CloseHandle(event_);
            CloseHandle(directory_);
            event_ = nullptr;
            directory_ =
                INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                std::format(
                    "Could not arm hot-iteration watcher for '{}' (Win32 error {}).",
                    root_.string(),
                    error));
        }
    }

    ~DirectoryChangeWatcher()
    {
        if (directory_ != INVALID_HANDLE_VALUE)
        {
            (void)CancelIoEx(
                directory_,
                &overlapped_);
        }

        if (event_ != nullptr)
        {
            CloseHandle(event_);
        }

        if (directory_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(directory_);
        }
    }

    DirectoryChangeWatcher(
        const DirectoryChangeWatcher&) = delete;
    DirectoryChangeWatcher& operator=(
        const DirectoryChangeWatcher&) = delete;

    [[nodiscard]] const std::filesystem::path& Root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] std::vector<std::filesystem::path> Poll()
    {
        std::vector<std::filesystem::path> result;

        if (!armed_ ||
            WaitForSingleObject(event_, 0U) !=
                WAIT_OBJECT_0)
        {
            return result;
        }

        DWORD transferred = 0U;
        const BOOL completed =
            GetOverlappedResult(
                directory_,
                &overlapped_,
                &transferred,
                FALSE);

        armed_ = false;

        if (!completed)
        {
            const DWORD error =
                GetLastError();

            if (error != ERROR_OPERATION_ABORTED)
            {
                orbit::log::Warning(
                    std::format(
                        "Hot iteration watcher temporarily lost '{}': Win32 error {}.",
                        root_.string(),
                        error));
            }

            (void)Arm();
            return result;
        }

        if (transferred == 0U)
        {
            orbit::log::Warning(
                std::format(
                    "Hot iteration watcher overflowed for '{}'; subsequent saves remain watched.",
                    root_.string()));
            (void)Arm();
            return result;
        }

        std::size_t offset = 0U;
        while (offset < transferred)
        {
            const auto* notification =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    buffer_.data() + offset);

            const std::wstring relativeText(
                notification->FileName,
                notification->FileNameLength /
                    sizeof(wchar_t));
            const std::filesystem::path relative(
                relativeText);

            if (!IsGeneratedRelativePath(relative))
            {
                result.push_back(
                    (root_ / relative).
                        lexically_normal());
            }

            if (notification->NextEntryOffset == 0U)
            {
                break;
            }

            offset +=
                notification->NextEntryOffset;
        }

        (void)Arm();
        return result;
    }

private:
    [[nodiscard]] bool Arm()
    {
        ResetEvent(event_);
        overlapped_ = {};
        overlapped_.hEvent = event_;

        const BOOL started =
            ReadDirectoryChangesW(
                directory_,
                buffer_.data(),
                static_cast<DWORD>(buffer_.size()),
                TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_SIZE,
                nullptr,
                &overlapped_,
                nullptr);

        armed_ =
            started != FALSE;
        return armed_;
    }

    std::filesystem::path root_;
    HANDLE directory_{INVALID_HANDLE_VALUE};
    HANDLE event_{nullptr};
    OVERLAPPED overlapped_{};
    std::array<std::byte, 64U * 1024U> buffer_{};
    bool armed_{false};
};
} // namespace

std::vector<HotIterationEvent>
DrainHotIterationEvents()
{
    std::scoped_lock lock(gEventMutex);
    std::vector<HotIterationEvent> result;
    result.swap(gEvents);
    return result;
}

class HotIterationService::Impl
{
public:
    explicit Impl(HotIterationConfig config)
        : config_(std::move(config))
    {
        config_.sourceRoot =
            NormalizePath(config_.sourceRoot);
        config_.binaryRoot =
            NormalizePath(config_.binaryRoot);
        config_.developmentBinary =
            NormalizePath(config_.developmentBinary);

        if (config_.runtimeRoot.empty())
        {
            config_.runtimeRoot =
                config_.binaryRoot /
                "hot_reload_runtime";
        }
        config_.runtimeRoot =
            NormalizePath(config_.runtimeRoot);

        AddWatchRoot(config_.sourceRoot);
        CleanupStaleStudioGenerations();
    }

    ~Impl()
    {
        Stop();
    }

    void AddWatchRoot(
        std::filesystem::path root)
    {
        if (root.empty())
        {
            return;
        }

        root = NormalizePath(root);
        if (!std::filesystem::is_directory(root))
        {
            return;
        }

        std::scoped_lock lock(configurationMutex_);

        if (std::find(
                knownWatchRoots_.begin(),
                knownWatchRoots_.end(),
                root) != knownWatchRoots_.end())
        {
            return;
        }

        knownWatchRoots_.push_back(root);
        pendingWatchRoots_.push_back(
            std::move(root));
    }

    void AddNativeHotRoot(
        std::filesystem::path root)
    {
        if (root.empty())
        {
            return;
        }

        root = NormalizePath(root);
        std::scoped_lock lock(configurationMutex_);

        if (std::find(
                nativeHotRoots_.begin(),
                nativeHotRoots_.end(),
                root) == nativeHotRoots_.end())
        {
            nativeHotRoots_.push_back(
                std::move(root));
        }
    }

    void Start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(
                expected,
                true))
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

private:
    struct PendingFallback
    {
        ChangeKind kind{ChangeKind::Ignored};
        std::filesystem::path path;
        std::chrono::steady_clock::time_point since{};
    };

    [[nodiscard]] bool IsNativeHotPath(
        const std::filesystem::path& path)
    {
        std::scoped_lock lock(configurationMutex_);
        return std::any_of(
            nativeHotRoots_.begin(),
            nativeHotRoots_.end(),
            [&path](const std::filesystem::path& root)
            {
                return IsInsideOrEqual(path, root);
            });
    }

    void WorkerMain(
        const std::stop_token stopToken)
    {
        std::vector<std::unique_ptr<DirectoryChangeWatcher>>
            watchers;
        std::unordered_map<std::wstring,
                           std::chrono::steady_clock::time_point>
            recentEvents;
        std::optional<PendingFallback>
            fallback;

        orbit::log::Info(
            "Orbit engine-wide hot iteration watcher is active.");

        while (!stopToken.stop_requested())
        {
            std::vector<std::filesystem::path>
                newRoots;
            {
                std::scoped_lock lock(
                    configurationMutex_);
                newRoots.swap(
                    pendingWatchRoots_);
            }

            for (const auto& root : newRoots)
            {
                try
                {
                    watchers.push_back(
                        std::make_unique<DirectoryChangeWatcher>(
                            root));
                    orbit::log::Info(
                        std::format(
                            "Hot iteration is watching {}.",
                            root.string()));
                }
                catch (const std::exception& exception)
                {
                    orbit::log::Warning(
                        std::string(
                            "Hot iteration could not watch a root: ") +
                        exception.what());
                }
            }

            const auto now =
                std::chrono::steady_clock::now();

            for (auto& watcher : watchers)
            {
                for (const auto& path :
                     watcher->Poll())
                {
                    const auto key =
                        NormalizePath(path).wstring();
                    const auto previous =
                        recentEvents.find(key);

                    if (previous != recentEvents.end() &&
                        now - previous->second <
                            std::chrono::milliseconds(80))
                    {
                        continue;
                    }
                    recentEvents[key] = now;

                    const ChangeKind kind =
                        ClassifyChange(path);

                    if (kind == ChangeKind::Ignored)
                    {
                        continue;
                    }

                    if (kind == ChangeKind::NativeModule &&
                        IsNativeHotPath(path))
                    {
                        // HotReloadHost owns this path and will replace its DLL
                        // generation in-process. Do not also rebuild Studio.
                        continue;
                    }

                    if (kind == ChangeKind::Shader ||
                        kind == ChangeKind::Script ||
                        kind == ChangeKind::Content)
                    {
                        PublishEvent({
                            .kind = kind,
                            .path = path
                        });

                        orbit::log::Info(
                            std::format(
                                "Hot iteration routed {} directly: {}",
                                ChangeKindName(kind),
                                path.string()));
                        continue;
                    }

                    if (kind == ChangeKind::NativeModule ||
                        kind == ChangeKind::RestartRequired)
                    {
                        fallback = PendingFallback{
                            .kind = kind,
                            .path = path,
                            .since = now
                        };

                        orbit::log::Info(
                            std::format(
                                "Hot iteration queued seamless Studio refresh for {}: {}",
                                ChangeKindName(kind),
                                path.string()));
                    }
                }
            }

            if (recentEvents.size() > 4096U)
            {
                std::erase_if(
                    recentEvents,
                    [now](const auto& pair)
                    {
                        return now - pair.second >
                            std::chrono::seconds(10);
                    });
            }

            if (fallback.has_value() &&
                now - fallback->since >=
                    config_.fallbackDebounce)
            {
                const auto request =
                    *fallback;
                fallback.reset();

                if (config_.automaticFallbackRelaunch &&
                    !relaunchScheduled_)
                {
                    if (BuildFallback(request) &&
                        StageAndLaunchReplacement())
                    {
                        relaunchScheduled_ = true;
                    }
                }
            }

            std::this_thread::sleep_for(
                config_.pollInterval);
        }
    }

    [[nodiscard]] bool BuildFallback(
        const PendingFallback& request)
    {
        orbit::log::Info(
            std::format(
                "Hot iteration rebuilding {} for {} change at {}...",
                config_.fallbackBuildTarget,
                ChangeKindName(request.kind),
                request.path.string()));

        std::error_code error;
        std::filesystem::create_directories(
            config_.runtimeRoot,
            error);

        const auto logPath =
            config_.runtimeRoot /
            "OrbitStudio_hot_iteration_build.log";

        HANDLE logHandle =
            CreateFileW(
                logPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags =
            STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;

        BOOL inheritHandles = FALSE;
        if (logHandle != INVALID_HANDLE_VALUE)
        {
            startup.dwFlags |=
                STARTF_USESTDHANDLES;
            startup.hStdOutput = logHandle;
            startup.hStdError = logHandle;
            startup.hStdInput =
                GetStdHandle(STD_INPUT_HANDLE);
            (void)SetHandleInformation(
                logHandle,
                HANDLE_FLAG_INHERIT,
                HANDLE_FLAG_INHERIT);
            inheritHandles = TRUE;
        }

        const std::wstring buildConfiguration =
            Widen(config_.buildConfiguration);
        const std::wstring buildTarget =
            Widen(config_.fallbackBuildTarget);

        std::wstring command =
            L"cmake --build " +
            QuoteCommandArgument(
                config_.binaryRoot.wstring()) +
            L" --config " +
            QuoteCommandArgument(
                buildConfiguration) +
            L" --target " +
            QuoteCommandArgument(
                buildTarget) +
            L" --parallel";
        command.push_back(L'\0');

        PROCESS_INFORMATION process{};
        const BOOL created =
            CreateProcessW(
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
            orbit::log::Error(
                std::format(
                    "Hot iteration could not start the fallback build (Win32 error {}).",
                    GetLastError()));
            return false;
        }

        WaitForSingleObject(
            process.hProcess,
            INFINITE);

        DWORD exitCode = 1U;
        (void)GetExitCodeProcess(
            process.hProcess,
            &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (exitCode != 0U)
        {
            orbit::log::Error(
                std::format(
                    "Hot iteration fallback build failed; current Orbit stays live. Build log: {}",
                    logPath.string()));
            return false;
        }

        orbit::log::Info(
            "Hot iteration fallback build succeeded; staging the replacement Studio generation.");
        return true;
    }

    [[nodiscard]] bool StageAndLaunchReplacement()
    {
        if (!std::filesystem::is_regular_file(
                config_.developmentBinary))
        {
            orbit::log::Error(
                std::format(
                    "Hot iteration built Studio but could not find {}.",
                    config_.developmentBinary.string()));
            return false;
        }

        std::error_code error;
        const auto generationRoot =
            config_.runtimeRoot /
            "studio_generations";
        std::filesystem::create_directories(
            generationRoot,
            error);

        const auto staged =
            generationRoot /
            std::format(
                "OrbitStudio_hot_{}_{}.exe",
                GetCurrentProcessId(),
                ++studioGeneration_);

        std::filesystem::copy_file(
            config_.developmentBinary,
            staged,
            std::filesystem::copy_options::overwrite_existing,
            error);

        if (error)
        {
            orbit::log::Error(
                std::format(
                    "Hot iteration could not stage the replacement executable: {}",
                    error.message()));
            return false;
        }

        const auto sourceDxCompiler =
            config_.developmentBinary.parent_path() /
            "dxcompiler.dll";
        const auto stagedDxCompiler =
            generationRoot /
            "dxcompiler.dll";

        if (std::filesystem::is_regular_file(
                sourceDxCompiler) &&
            !std::filesystem::exists(
                stagedDxCompiler))
        {
            error.clear();
            std::filesystem::copy_file(
                sourceDxCompiler,
                stagedDxCompiler,
                std::filesystem::copy_options::overwrite_existing,
                error);
        }

        std::wstring command =
            QuoteCommandArgument(
                staged.wstring());

        for (const auto& argument :
             config_.relaunchArguments)
        {
            command.push_back(L' ');
            command +=
                QuoteCommandArgument(
                    Widen(argument));
        }

        command +=
            L" --orbit-hot-wait-pid " +
            std::to_wstring(
                GetCurrentProcessId());
        command.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        const BOOL created =
            CreateProcessW(
                nullptr,
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                0U,
                nullptr,
                config_.sourceRoot.c_str(),
                &startup,
                &process);

        if (!created)
        {
            orbit::log::Error(
                std::format(
                    "Hot iteration could not launch the replacement generation (Win32 error {}).",
                    GetLastError()));
            std::filesystem::remove(
                staged,
                error);
            return false;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        struct CloseContext
        {
            DWORD processId{0U};
            bool posted{false};
        } context{
            .processId =
                GetCurrentProcessId(),
            .posted = false
        };

        (void)EnumWindows(
            [](const HWND window,
               const LPARAM parameter) -> BOOL
            {
                auto* context =
                    reinterpret_cast<CloseContext*>(
                        parameter);
                DWORD owner = 0U;
                (void)GetWindowThreadProcessId(
                    window,
                    &owner);

                if (owner == context->processId)
                {
                    context->posted = true;
                    (void)PostMessageW(
                        window,
                        WM_CLOSE,
                        0U,
                        0U);
                }

                return TRUE;
            },
            reinterpret_cast<LPARAM>(
                &context));

        orbit::log::Info(
            context.posted
                ? "Hot iteration replacement is staged; closing the old generation cleanly."
                : "Hot iteration replacement is staged; no top-level window was found to close.");
        return true;
    }

    void CleanupStaleStudioGenerations()
    {
        const auto generationRoot =
            config_.runtimeRoot /
            "studio_generations";

        std::error_code error;
        if (!std::filesystem::is_directory(
                generationRoot,
                error))
        {
            return;
        }

        for (std::filesystem::directory_iterator
                 iterator(
                     generationRoot,
                     std::filesystem::directory_options::skip_permission_denied,
                     error),
             end;
             !error && iterator != end;
             iterator.increment(error))
        {
            if (!iterator->is_regular_file(error) ||
                iterator->path().extension() != ".exe")
            {
                continue;
            }

            std::error_code removeError;
            std::filesystem::remove(
                iterator->path(),
                removeError);
            // A currently running generation is locked by Windows and simply
            // survives this cleanup. It disappears on a later launch.
        }
    }

    HotIterationConfig config_;
    std::mutex configurationMutex_;
    std::vector<std::filesystem::path>
        knownWatchRoots_;
    std::vector<std::filesystem::path>
        pendingWatchRoots_;
    std::vector<std::filesystem::path>
        nativeHotRoots_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    bool relaunchScheduled_{false};
    std::uint64_t studioGeneration_{0U};
};

HotIterationService::HotIterationService(
    HotIterationConfig config)
    : impl_(
          std::make_unique<Impl>(
              std::move(config)))
{
}

HotIterationService::~HotIterationService() = default;

void HotIterationService::AddWatchRoot(
    std::filesystem::path root)
{
    impl_->AddWatchRoot(
        std::move(root));
}

void HotIterationService::AddNativeHotRoot(
    std::filesystem::path root)
{
    impl_->AddNativeHotRoot(
        std::move(root));
}

void HotIterationService::Start()
{
    impl_->Start();
}

void HotIterationService::Stop() noexcept
{
    impl_->Stop();
}
} // namespace orbit::hot_reload
