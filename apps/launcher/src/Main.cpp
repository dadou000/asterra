#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <orbit/platform/AppResources.hpp>
#include <orbit/platform/CrashHandler.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::size_t kPathCapacity = 32768;

void WriteBytes(
    const HANDLE file,
    const std::string_view text) noexcept
{
    if (file == INVALID_HANDLE_VALUE ||
        text.empty())
    {
        return;
    }

    DWORD written = 0;

    static_cast<void>(
        WriteFile(
            file,
            text.data(),
            static_cast<DWORD>(
                (std::min)(
                    text.size(),
                    static_cast<std::size_t>(
                        MAXDWORD))),
            &written,
            nullptr));
}

[[nodiscard]] std::string WideToUtf8(
    const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int required =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

    if (required <= 0)
    {
        return {};
    }

    std::string result(
        static_cast<std::size_t>(
            required),
        '\0');

    static_cast<void>(
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr));

    return result;
}

void WriteWideLine(
    const HANDLE file,
    const std::string_view label,
    const std::filesystem::path& value)
{
    WriteBytes(file, label);
    WriteBytes(
        file,
        WideToUtf8(
            value.wstring()));
    WriteBytes(file, "\r\n");
}

[[nodiscard]] std::filesystem::path
ExecutablePath()
{
    std::array<wchar_t, kPathCapacity>
        buffer{};

    const DWORD length =
        GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(
                buffer.size()));

    if (length == 0 ||
        length >= buffer.size())
    {
        return {};
    }

    return std::filesystem::path(
        std::wstring_view(
            buffer.data(),
            length));
}

[[nodiscard]] std::filesystem::path
FindSandbox(
    const std::filesystem::path& launcherDirectory)
{
    std::error_code error;

    const std::filesystem::path packaged =
        launcherDirectory /
        L"OrbitSandbox.exe";

    if (std::filesystem::exists(
            packaged,
            error) &&
        !error)
    {
        return packaged;
    }

    error.clear();

    const std::filesystem::path configName =
        launcherDirectory.filename();

    const std::filesystem::path development =
        launcherDirectory.
            parent_path().
            parent_path() /
        L"sandbox" /
        configName /
        L"OrbitSandbox.exe";

    if (std::filesystem::exists(
            development,
            error) &&
        !error)
    {
        return development;
    }

    return {};
}

[[nodiscard]] std::wstring
QuoteArgument(
    const std::wstring_view argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }

    const bool needsQuotes =
        argument.find_first_of(
            L" \t\n\v\"") !=
        std::wstring_view::npos;

    if (!needsQuotes)
    {
        return std::wstring(
            argument);
    }

    std::wstring result;
    result.push_back(L'\"');

    std::size_t backslashes = 0;

    for (const wchar_t character :
         argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (character == L'\"')
        {
            result.append(
                backslashes * 2U + 1U,
                L'\\');

            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }

        result.append(
            backslashes,
            L'\\');

        backslashes = 0;
        result.push_back(character);
    }

    result.append(
        backslashes * 2U,
        L'\\');

    result.push_back(L'\"');
    return result;
}

[[nodiscard]] std::wstring
BuildChildCommandLine(
    const std::filesystem::path& sandboxPath)
{
    int argumentCount = 0;

    LPWSTR* arguments =
        CommandLineToArgvW(
            GetCommandLineW(),
            &argumentCount);

    std::wstring commandLine =
        QuoteArgument(
            sandboxPath.wstring());

    if (arguments != nullptr)
    {
        for (int index = 1;
             index < argumentCount;
             ++index)
        {
            commandLine.push_back(L' ');
            commandLine +=
                QuoteArgument(
                    arguments[index]);
        }

        LocalFree(arguments);
    }

    return commandLine;
}

[[nodiscard]] std::filesystem::path
MakeSessionLogPath(
    const std::filesystem::path& logDirectory)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    std::array<wchar_t, 256> name{};

    _snwprintf_s(
        name.data(),
        name.size(),
        _TRUNCATE,
        L"orbit-session-%04u%02u%02u-%02u%02u%02u-p%lu.log",
        static_cast<unsigned>(
            time.wYear),
        static_cast<unsigned>(
            time.wMonth),
        static_cast<unsigned>(
            time.wDay),
        static_cast<unsigned>(
            time.wHour),
        static_cast<unsigned>(
            time.wMinute),
        static_cast<unsigned>(
            time.wSecond),
        static_cast<unsigned long>(
            GetCurrentProcessId()));

    return logDirectory /
        name.data();
}

void ShowFailure(
    const std::wstring_view message)
{
    MessageBoxW(
        nullptr,
        std::wstring(message).c_str(),
        L"Orbit Launcher",
        MB_OK |
        MB_ICONERROR |
        MB_SETFOREGROUND);
}

constexpr const wchar_t* kSplashClassName =
    L"OrbitSplashWindowClass";

// A borderless, layered (per-pixel alpha) window showing the Orbit
// logo while OrbitSandbox starts up. Closed as soon as the sandbox's
// own window appears, or after a bounded timeout if it never does.
class SplashWindow
{
public:
    SplashWindow() = default;

    ~SplashWindow()
    {
        Close();
    }

    SplashWindow(const SplashWindow&) = delete;
    SplashWindow& operator=(const SplashWindow&) = delete;

    // Returns false (leaving nothing shown) rather than throwing --
    // a missing splash is cosmetic, never worth failing the launch.
    bool Show()
    {
        gdiplusToken_ = 0;

        Gdiplus::GdiplusStartupInput startupInput;

        if (Gdiplus::GdiplusStartup(
                &gdiplusToken_,
                &startupInput,
                nullptr) != Gdiplus::Ok)
        {
            gdiplusToken_ = 0;
            return false;
        }

        const HMODULE module =
            GetModuleHandleW(nullptr);

        const HRSRC resourceHandle =
            FindResourceA(
                module,
                MAKEINTRESOURCEA(
                    ORBIT_RESOURCE_SPLASH_PNG),
                ORBIT_RESOURCE_SPLASH_TYPE);

        if (resourceHandle == nullptr)
        {
            return false;
        }

        const HGLOBAL resourceData =
            LoadResource(
                module,
                resourceHandle);

        const void* resourceBytes =
            resourceData != nullptr
                ? LockResource(resourceData)
                : nullptr;

        const DWORD resourceSize =
            SizeofResource(
                module,
                resourceHandle);

        if (resourceBytes == nullptr ||
            resourceSize == 0)
        {
            return false;
        }

        const HGLOBAL streamBuffer =
            GlobalAlloc(
                GMEM_MOVEABLE,
                resourceSize);

        if (streamBuffer == nullptr)
        {
            return false;
        }

        void* const streamMemory =
            GlobalLock(streamBuffer);

        if (streamMemory == nullptr)
        {
            GlobalFree(streamBuffer);
            return false;
        }

        memcpy(
            streamMemory,
            resourceBytes,
            resourceSize);

        GlobalUnlock(streamBuffer);

        IStream* imageStream = nullptr;

        if (CreateStreamOnHGlobal(
                streamBuffer,
                TRUE,
                &imageStream) != S_OK)
        {
            GlobalFree(streamBuffer);
            return false;
        }

        Gdiplus::Bitmap logo(imageStream);
        imageStream->Release();

        if (logo.GetLastStatus() !=
            Gdiplus::Ok)
        {
            return false;
        }

        constexpr int kMaxDimension = 384;

        const UINT sourceWidth =
            logo.GetWidth();

        const UINT sourceHeight =
            logo.GetHeight();

        if (sourceWidth == 0 ||
            sourceHeight == 0)
        {
            return false;
        }

        const float scale =
            (std::min)(
                1.0F,
                static_cast<float>(
                    kMaxDimension) /
                    static_cast<float>(
                        (std::max)(
                            sourceWidth,
                            sourceHeight)));

        const int width =
            (std::max)(
                1,
                static_cast<int>(
                    static_cast<float>(
                        sourceWidth) *
                    scale));

        const int height =
            (std::max)(
                1,
                static_cast<int>(
                    static_cast<float>(
                        sourceHeight) *
                    scale));

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = module;
        windowClass.lpszClassName = kSplashClassName;
        windowClass.hCursor =
            LoadCursorW(
                nullptr,
                MAKEINTRESOURCEW(32512)); // IDC_ARROW

        // Harmless if a previous instance already registered it.
        RegisterClassExW(&windowClass);

        const int screenWidth =
            GetSystemMetrics(SM_CXSCREEN);

        const int screenHeight =
            GetSystemMetrics(SM_CYSCREEN);

        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED |
                WS_EX_TOPMOST |
                WS_EX_TOOLWINDOW,
            kSplashClassName,
            L"Orbit",
            WS_POPUP,
            (screenWidth - width) / 2,
            (screenHeight - height) / 2,
            width,
            height,
            nullptr,
            nullptr,
            module,
            nullptr);

        if (hwnd_ == nullptr)
        {
            return false;
        }

        const HDC screenDc = GetDC(nullptr);
        const HDC memoryDc = CreateCompatibleDC(screenDc);

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;

        const HBITMAP dib = CreateDIBSection(
            memoryDc,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0);

        if (dib != nullptr)
        {
            const HGDIOBJ previousBitmap =
                SelectObject(memoryDc, dib);

            Gdiplus::Graphics graphics(memoryDc);
            graphics.SetInterpolationMode(
                Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetCompositingMode(
                Gdiplus::CompositingModeSourceCopy);

            graphics.DrawImage(
                &logo,
                Gdiplus::Rect(0, 0, width, height));

            POINT sourcePoint{0, 0};
            POINT destinationPoint{
                (screenWidth - width) / 2,
                (screenHeight - height) / 2};

            SIZE windowSize{width, height};

            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;

            UpdateLayeredWindow(
                hwnd_,
                screenDc,
                &destinationPoint,
                &windowSize,
                memoryDc,
                &sourcePoint,
                0,
                &blend,
                ULW_ALPHA);

            SelectObject(memoryDc, previousBitmap);
            DeleteObject(dib);
        }

        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);

        return true;
    }

    void PumpMessages()
    {
        MSG message{};

        while (PeekMessageW(
            &message,
            nullptr,
            0,
            0,
            PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void Close()
    {
        if (hwnd_ != nullptr)
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        if (gdiplusToken_ != 0)
        {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
        }
    }

private:
    HWND hwnd_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
};

struct FindChildWindowContext
{
    DWORD processId = 0;
    HWND result = nullptr;
};

BOOL CALLBACK FindChildWindowProc(
    const HWND hwnd,
    const LPARAM lParam)
{
    auto& context =
        *reinterpret_cast<FindChildWindowContext*>(
            lParam);

    DWORD windowProcessId = 0;

    GetWindowThreadProcessId(
        hwnd,
        &windowProcessId);

    if (windowProcessId ==
            context.processId &&
        IsWindowVisible(hwnd) &&
        GetWindow(hwnd, GW_OWNER) ==
            nullptr)
    {
        context.result = hwnd;
        return FALSE;
    }

    return TRUE;
}

// Waits until the sandbox process has created its own visible
// top-level window (so the splash can hand off to it), the process
// exits early, or a bounded timeout elapses -- whichever comes
// first. Pumps the splash window's own message queue throughout so
// it keeps rendering while we wait.
void WaitForChildWindowOrTimeout(
    const HANDLE processHandle,
    const DWORD processId,
    SplashWindow& splash,
    const DWORD timeoutMs)
{
    const DWORD deadline =
        GetTickCount() + timeoutMs;

    for (;;)
    {
        FindChildWindowContext context{};
        context.processId = processId;

        EnumWindows(
            FindChildWindowProc,
            reinterpret_cast<LPARAM>(
                &context));

        if (context.result != nullptr)
        {
            return;
        }

        const DWORD now = GetTickCount();

        if (now >= deadline)
        {
            return;
        }

        splash.PumpMessages();

        const DWORD waitResult =
            WaitForSingleObject(
                processHandle,
                50);

        if (waitResult ==
            WAIT_OBJECT_0)
        {
            return;
        }
    }
}
} // namespace

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int)
{
    SplashWindow splash;
    splash.Show();
    splash.PumpMessages();

    const std::filesystem::path
        launcherPath =
            ExecutablePath();

    if (launcherPath.empty())
    {
        ShowFailure(
            L"Orbit Launcher could not determine its executable path.");

        return 1;
    }

    const std::filesystem::path
        launcherDirectory =
            launcherPath.parent_path();

    const std::filesystem::path
        logDirectory =
            launcherDirectory /
            L"logs";

    std::error_code error;

    std::filesystem::create_directories(
        logDirectory,
        error);

    if (error)
    {
        ShowFailure(
            L"Orbit Launcher could not create its logs directory.");

        return 1;
    }

    static_cast<void>(
        orbit::platform::InstallCrashHandler({
            .directory =
                logDirectory,
            .applicationName =
                "OrbitLauncher",
            .writeMiniDump = true
        }));

    const std::filesystem::path
        sessionLogPath =
            MakeSessionLogPath(
                logDirectory);

    SECURITY_ATTRIBUTES security{};
    security.nLength =
        sizeof(SECURITY_ATTRIBUTES);

    security.bInheritHandle =
        TRUE;

    const HANDLE sessionLog =
        CreateFileW(
            sessionLogPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE,
            &security,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (sessionLog ==
        INVALID_HANDLE_VALUE)
    {
        ShowFailure(
            L"Orbit Launcher could not create a session log.");

        return 1;
    }

    WriteBytes(
        sessionLog,
        "Orbit launcher session\r\n"
        "======================\r\n");

    WriteWideLine(
        sessionLog,
        "Launcher: ",
        launcherPath);

    WriteWideLine(
        sessionLog,
        "Session log: ",
        sessionLogPath);

    const std::filesystem::path
        sandboxPath =
            FindSandbox(
                launcherDirectory);

    if (sandboxPath.empty())
    {
        WriteBytes(
            sessionLog,
            "ERROR: OrbitSandbox.exe was not found.\r\n");

        FlushFileBuffers(
            sessionLog);

        CloseHandle(
            sessionLog);

        ShowFailure(
            L"OrbitSandbox.exe was not found. Build the OrbitSandbox target first.");

        return 1;
    }

    WriteWideLine(
        sessionLog,
        "Runtime: ",
        sandboxPath);

    static_cast<void>(
        SetEnvironmentVariableW(
            L"ORBIT_LOG_DIR",
            logDirectory.c_str()));

    static_cast<void>(
        SetEnvironmentVariableW(
            L"ORBIT_SESSION_LOG",
            sessionLogPath.c_str()));

    std::wstring commandLine =
        BuildChildCommandLine(
            sandboxPath);

    STARTUPINFOW startup{};
    startup.cb =
        sizeof(STARTUPINFOW);

    startup.dwFlags =
        STARTF_USESTDHANDLES;

    startup.hStdInput =
        GetStdHandle(
            STD_INPUT_HANDLE);

    startup.hStdOutput =
        sessionLog;

    startup.hStdError =
        sessionLog;

    PROCESS_INFORMATION process{};

    WriteBytes(
        sessionLog,
        "Launching OrbitSandbox...\r\n"
        "-------------------------\r\n");

    FlushFileBuffers(
        sessionLog);

    const BOOL launched =
        CreateProcessW(
            sandboxPath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            launcherDirectory.c_str(),
            &startup,
            &process);

    if (launched == FALSE)
    {
        const DWORD launchError =
            GetLastError();

        std::array<char, 128> message{};

        const int length =
            snprintf(
                message.data(),
                message.size(),
                "ERROR: CreateProcessW failed with Win32 error %lu.\r\n",
                static_cast<unsigned long>(
                    launchError));

        if (length > 0)
        {
            WriteBytes(
                sessionLog,
                std::string_view(
                    message.data(),
                    static_cast<std::size_t>(
                        (std::min)(
                            length,
                            static_cast<int>(
                                message.size() - 1U)))));
        }

        FlushFileBuffers(
            sessionLog);

        CloseHandle(
            sessionLog);

        ShowFailure(
            L"Orbit Launcher could not start OrbitSandbox. See the session log.");

        return 1;
    }

    CloseHandle(
        process.hThread);

    WaitForChildWindowOrTimeout(
        process.hProcess,
        process.dwProcessId,
        splash,
        /*timeoutMs=*/ 20000);

    splash.Close();

    static_cast<void>(
        WaitForSingleObject(
            process.hProcess,
            INFINITE));

    DWORD exitCode = 1;

    static_cast<void>(
        GetExitCodeProcess(
            process.hProcess,
            &exitCode));

    CloseHandle(
        process.hProcess);

    WriteBytes(
        sessionLog,
        "\r\n-------------------------\r\n");

    std::array<char, 128> exitMessage{};

    const int exitLength =
        snprintf(
            exitMessage.data(),
            exitMessage.size(),
            "OrbitSandbox exit code: 0x%08lX (%lu)\r\n",
            static_cast<unsigned long>(
                exitCode),
            static_cast<unsigned long>(
                exitCode));

    if (exitLength > 0)
    {
        WriteBytes(
            sessionLog,
            std::string_view(
                exitMessage.data(),
                static_cast<std::size_t>(
                    (std::min)(
                        exitLength,
                        static_cast<int>(
                            exitMessage.size() - 1U)))));
    }

    FlushFileBuffers(
        sessionLog);

    CloseHandle(
        sessionLog);

    if (exitCode != 0)
    {
        const std::wstring message =
            L"Orbit exited abnormally.\n\nSession log:\n" +
            sessionLogPath.wstring() +
            L"\n\nA crash .log and .dmp will also be in the same folder if the runtime hit an unhandled exception.";

        ShowFailure(message);
    }

    return static_cast<int>(
        exitCode);
}
