#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

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
} // namespace

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int)
{
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
