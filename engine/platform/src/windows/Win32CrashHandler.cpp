#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <DbgHelp.h>

#include <orbit/core/Types.hpp>
#include <orbit/platform/CrashHandler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace orbit::platform
{
namespace
{
constexpr std::size_t kPathCapacity = 4096;
constexpr USHORT kMaximumFrames = 64;

struct CrashState
{
    std::array<wchar_t, kPathCapacity> directory{};
    std::array<wchar_t, 128> applicationName{};
    bool writeMiniDump{true};
    bool symbolsInitialized{false};
    LPTOP_LEVEL_EXCEPTION_FILTER previousExceptionFilter{nullptr};
    std::terminate_handler previousTerminateHandler{nullptr};
};

CrashState g_state;
std::atomic_flag g_handlingCrash = ATOMIC_FLAG_INIT;
std::atomic<bool> g_installed{false};

void CopyWideString(
    const std::wstring_view source,
    wchar_t* destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr ||
        capacity == 0)
    {
        return;
    }

    const std::size_t count =
        (std::min)(
            source.size(),
            capacity - 1U);

    if (count > 0)
    {
        std::wmemcpy(
            destination,
            source.data(),
            count);
    }

    destination[count] = L'\0';
}

void CopyUtf8AsWide(
    const std::string_view source,
    wchar_t* destination,
    const std::size_t capacity) noexcept
{
    if (destination == nullptr ||
        capacity == 0)
    {
        return;
    }

    const int converted =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            source.data(),
            static_cast<int>(source.size()),
            destination,
            static_cast<int>(capacity - 1U));

    if (converted <= 0)
    {
        destination[0] = L'\0';
        return;
    }

    destination[
        static_cast<std::size_t>(
            converted)] = L'\0';
}

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

void WriteFormat(
    const HANDLE file,
    const char* format,
    ...) noexcept
{
    std::array<char, 2048> buffer{};

    va_list arguments;
    va_start(arguments, format);

    const int length =
        vsnprintf(
            buffer.data(),
            buffer.size(),
            format,
            arguments);

    va_end(arguments);

    if (length <= 0)
    {
        return;
    }

    const std::size_t safeLength =
        (std::min)(
            static_cast<std::size_t>(
                length),
            buffer.size() - 1U);

    WriteBytes(
        file,
        std::string_view(
            buffer.data(),
            safeLength));
}

void WriteWideValue(
    const HANDLE file,
    const wchar_t* label,
    const wchar_t* value) noexcept
{
    std::array<char, 4096> utf8Label{};
    std::array<char, 8192> utf8Value{};

    const int labelLength =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            label,
            -1,
            utf8Label.data(),
            static_cast<int>(
                utf8Label.size()),
            nullptr,
            nullptr);

    const int valueLength =
        value != nullptr
            ? WideCharToMultiByte(
                CP_UTF8,
                0,
                value,
                -1,
                utf8Value.data(),
                static_cast<int>(
                    utf8Value.size()),
                nullptr,
                nullptr)
            : 0;

    if (labelLength <= 0)
    {
        return;
    }

    WriteBytes(
        file,
        std::string_view(
            utf8Label.data(),
            static_cast<std::size_t>(
                labelLength - 1)));

    if (valueLength > 0)
    {
        WriteBytes(
            file,
            std::string_view(
                utf8Value.data(),
                static_cast<std::size_t>(
                    valueLength - 1)));
    }

    WriteBytes(file, "\r\n");
}

[[nodiscard]] const char*
ExceptionName(
    const DWORD code) noexcept
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

[[nodiscard]] std::filesystem::path
ResolveDirectory(
    const CrashHandlerConfig& config,
    std::error_code& error)
{
    if (!config.directory.empty())
    {
        return config.directory;
    }

    std::array<wchar_t, kPathCapacity>
        environmentDirectory{};

    const DWORD environmentLength =
        GetEnvironmentVariableW(
            L"ORBIT_LOG_DIR",
            environmentDirectory.data(),
            static_cast<DWORD>(
                environmentDirectory.size()));

    if (environmentLength > 0 &&
        environmentLength <
            environmentDirectory.size())
    {
        return std::filesystem::path(
            environmentDirectory.data());
    }

    const std::filesystem::path current =
        std::filesystem::current_path(
            error);

    if (error)
    {
        return {};
    }

    return current / L"logs";
}

void BuildCrashPaths(
    wchar_t* logPath,
    const std::size_t logCapacity,
    wchar_t* dumpPath,
    const std::size_t dumpCapacity) noexcept
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    const DWORD processId =
        GetCurrentProcessId();

    const DWORD threadId =
        GetCurrentThreadId();

    _snwprintf_s(
        logPath,
        logCapacity,
        _TRUNCATE,
        L"%s\\%s-crash-%04u%02u%02u-%02u%02u%02u-p%lu-t%lu.log",
        g_state.directory.data(),
        g_state.applicationName.data(),
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned long>(
            processId),
        static_cast<unsigned long>(
            threadId));

    _snwprintf_s(
        dumpPath,
        dumpCapacity,
        _TRUNCATE,
        L"%s\\%s-crash-%04u%02u%02u-%02u%02u%02u-p%lu-t%lu.dmp",
        g_state.directory.data(),
        g_state.applicationName.data(),
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned long>(
            processId),
        static_cast<unsigned long>(
            threadId));
}

void WriteCommonHeader(
    const HANDLE file,
    const char* reason) noexcept
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    WriteBytes(
        file,
        "Orbit crash report\r\n"
        "==================\r\n");

    WriteFormat(
        file,
        "Application: %ls\r\n"
        "Reason: %s\r\n"
        "Time: %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
        "Process ID: %lu\r\n"
        "Thread ID: %lu\r\n",
        g_state.applicationName.data(),
        reason,
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds),
        static_cast<unsigned long>(
            GetCurrentProcessId()),
        static_cast<unsigned long>(
            GetCurrentThreadId()));

    std::array<wchar_t, kPathCapacity>
        executablePath{};

    const DWORD executableLength =
        GetModuleFileNameW(
            nullptr,
            executablePath.data(),
            static_cast<DWORD>(
                executablePath.size()));

    if (executableLength > 0 &&
        executableLength <
            executablePath.size())
    {
        WriteWideValue(
            file,
            L"Executable: ",
            executablePath.data());
    }

    WriteWideValue(
        file,
        L"Command line: ",
        GetCommandLineW());

    std::array<wchar_t, kPathCapacity>
        sessionLog{};

    const DWORD sessionLength =
        GetEnvironmentVariableW(
            L"ORBIT_SESSION_LOG",
            sessionLog.data(),
            static_cast<DWORD>(
                sessionLog.size()));

    if (sessionLength > 0 &&
        sessionLength <
            sessionLog.size())
    {
        WriteWideValue(
            file,
            L"Session log: ",
            sessionLog.data());
    }

    WriteBytes(file, "\r\n");
}

void WriteSymbolizedAddress(
    const HANDLE file,
    const DWORD64 address,
    const u32 index) noexcept
{
    std::array<std::byte,
        sizeof(SYMBOL_INFO) +
        MAX_SYM_NAME * sizeof(char)>
        symbolStorage{};

    auto* symbol =
        reinterpret_cast<SYMBOL_INFO*>(
            symbolStorage.data());

    symbol->SizeOfStruct =
        sizeof(SYMBOL_INFO);

    symbol->MaxNameLen =
        MAX_SYM_NAME;

    DWORD64 displacement = 0;

    const BOOL hasSymbol =
        SymFromAddr(
            GetCurrentProcess(),
            address,
            &displacement,
            symbol);

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct =
        sizeof(IMAGEHLP_LINE64);

    DWORD lineDisplacement = 0;

    const BOOL hasLine =
        SymGetLineFromAddr64(
            GetCurrentProcess(),
            address,
            &lineDisplacement,
            &line);

    if (hasSymbol != FALSE &&
        hasLine != FALSE)
    {
        WriteFormat(
            file,
            "#%02u 0x%016llX %s + 0x%llX (%s:%lu)\r\n",
            index,
            static_cast<unsigned long long>(
                address),
            symbol->Name,
            static_cast<unsigned long long>(
                displacement),
            line.FileName,
            static_cast<unsigned long>(
                line.LineNumber));

        return;
    }

    if (hasSymbol != FALSE)
    {
        WriteFormat(
            file,
            "#%02u 0x%016llX %s + 0x%llX\r\n",
            index,
            static_cast<unsigned long long>(
                address),
            symbol->Name,
            static_cast<unsigned long long>(
                displacement));

        return;
    }

    WriteFormat(
        file,
        "#%02u 0x%016llX\r\n",
        index,
        static_cast<unsigned long long>(
            address));
}

void WriteCapturedStack(
    const HANDLE file) noexcept
{
    std::array<void*, kMaximumFrames>
        frames{};

    const USHORT frameCount =
        CaptureStackBackTrace(
            0,
            kMaximumFrames,
            frames.data(),
            nullptr);

    WriteBytes(
        file,
        "Stack trace:\r\n");

    for (USHORT index = 0;
         index < frameCount;
         ++index)
    {
        WriteSymbolizedAddress(
            file,
            reinterpret_cast<DWORD64>(
                frames[index]),
            index);
    }
}

void WriteExceptionStack(
    const HANDLE file,
    const EXCEPTION_POINTERS* pointers) noexcept
{
    if (pointers == nullptr ||
        pointers->ContextRecord == nullptr)
    {
        WriteCapturedStack(file);
        return;
    }

    CONTEXT context =
        *pointers->ContextRecord;

    STACKFRAME64 frame{};
    DWORD machineType = 0;

#if defined(_M_X64)
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#elif defined(_M_ARM64)
    machineType = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    WriteCapturedStack(file);
    return;
#endif

    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    WriteBytes(
        file,
        "Stack trace:\r\n");

    for (u32 index = 0;
         index < kMaximumFrames;
         ++index)
    {
        if (frame.AddrPC.Offset == 0)
        {
            break;
        }

        WriteSymbolizedAddress(
            file,
            frame.AddrPC.Offset,
            index);

        if (StackWalk64(
                machineType,
                GetCurrentProcess(),
                GetCurrentThread(),
                &frame,
                &context,
                nullptr,
                SymFunctionTableAccess64,
                SymGetModuleBase64,
                nullptr) == FALSE)
        {
            break;
        }
    }
}

void WriteMiniDump(
    const wchar_t* dumpPath,
    EXCEPTION_POINTERS* pointers) noexcept
{
    if (!g_state.writeMiniDump)
    {
        return;
    }

    const HANDLE dumpFile =
        CreateFileW(
            dumpPath,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (dumpFile ==
        INVALID_HANDLE_VALUE)
    {
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION
        exceptionInformation{};

    MINIDUMP_EXCEPTION_INFORMATION*
        exceptionInformationPointer =
            nullptr;

    if (pointers != nullptr)
    {
        exceptionInformation.ThreadId =
            GetCurrentThreadId();

        exceptionInformation.
            ExceptionPointers =
                pointers;

        exceptionInformation.
            ClientPointers =
                FALSE;

        exceptionInformationPointer =
            &exceptionInformation;
    }

    static_cast<void>(
        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            dumpFile,
            static_cast<MINIDUMP_TYPE>(
                MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpScanMemory),
            exceptionInformationPointer,
            nullptr,
            nullptr));

    FlushFileBuffers(dumpFile);
    CloseHandle(dumpFile);
}

void WriteCrashReport(
    const char* reason,
    EXCEPTION_POINTERS* pointers) noexcept
{
    if (g_handlingCrash.
            test_and_set(
                std::memory_order_acq_rel))
    {
        return;
    }

    std::array<wchar_t, kPathCapacity>
        logPath{};

    std::array<wchar_t, kPathCapacity>
        dumpPath{};

    BuildCrashPaths(
        logPath.data(),
        logPath.size(),
        dumpPath.data(),
        dumpPath.size());

    const HANDLE logFile =
        CreateFileW(
            logPath.data(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (logFile !=
        INVALID_HANDLE_VALUE)
    {
        WriteCommonHeader(
            logFile,
            reason);

        if (pointers != nullptr &&
            pointers->ExceptionRecord !=
                nullptr)
        {
            const DWORD code =
                pointers->ExceptionRecord->
                    ExceptionCode;

            WriteFormat(
                logFile,
                "Exception: %s (0x%08lX)\r\n"
                "Exception address: 0x%p\r\n",
                ExceptionName(code),
                static_cast<unsigned long>(
                    code),
                pointers->ExceptionRecord->
                    ExceptionAddress);

            if (code ==
                    EXCEPTION_ACCESS_VIOLATION &&
                pointers->ExceptionRecord->
                    NumberParameters >= 2)
            {
                const ULONG_PTR operation =
                    pointers->ExceptionRecord->
                        ExceptionInformation[0];

                const ULONG_PTR address =
                    pointers->ExceptionRecord->
                        ExceptionInformation[1];

                const char* operationName =
                    operation == 0
                        ? "read"
                        : operation == 1
                            ? "write"
                            : operation == 8
                                ? "execute"
                                : "unknown";

                WriteFormat(
                    logFile,
                    "Access violation operation: %s\r\n"
                    "Access violation address: 0x%p\r\n",
                    operationName,
                    reinterpret_cast<void*>(
                        address));
            }

            WriteBytes(
                logFile,
                "\r\n");
        }

        WriteExceptionStack(
            logFile,
            pointers);

        WriteBytes(
            logFile,
            "\r\nMiniDump: ");

        std::array<char, kPathCapacity * 2>
            dumpPathUtf8{};

        const int converted =
            WideCharToMultiByte(
                CP_UTF8,
                0,
                dumpPath.data(),
                -1,
                dumpPathUtf8.data(),
                static_cast<int>(
                    dumpPathUtf8.size()),
                nullptr,
                nullptr);

        if (converted > 0)
        {
            WriteBytes(
                logFile,
                std::string_view(
                    dumpPathUtf8.data(),
                    static_cast<std::size_t>(
                        converted - 1)));
        }

        WriteBytes(
            logFile,
            "\r\n");

        FlushFileBuffers(logFile);
        CloseHandle(logFile);
    }

    WriteMiniDump(
        dumpPath.data(),
        pointers);
}

LONG WINAPI UnhandledExceptionFilter(
    EXCEPTION_POINTERS* pointers) noexcept
{
    WriteCrashReport(
        "Unhandled structured exception",
        pointers);

    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void TerminateHandler() noexcept
{
    const char* reason =
        "std::terminate";

    try
    {
        const std::exception_ptr current =
            std::current_exception();

        if (current)
        {
            std::rethrow_exception(
                current);
        }
    }
    catch (const std::exception& exception)
    {
        reason =
            exception.what();
    }
    catch (...)
    {
        reason =
            "std::terminate with non-standard exception";
    }

    WriteCrashReport(
        reason,
        nullptr);

    TerminateProcess(
        GetCurrentProcess(),
        static_cast<UINT>(
            ERROR_UNHANDLED_EXCEPTION));

    std::abort();
}
} // namespace

bool InstallCrashHandler(
    const CrashHandlerConfig& config) noexcept
{
    if (g_installed.exchange(
            true,
            std::memory_order_acq_rel))
    {
        return true;
    }

    try
    {
        std::error_code error;

        const std::filesystem::path directory =
            ResolveDirectory(
                config,
                error);

        if (error ||
            directory.empty())
        {
            g_installed.store(
                false,
                std::memory_order_release);

            return false;
        }

        std::filesystem::create_directories(
            directory,
            error);

        if (error)
        {
            g_installed.store(
                false,
                std::memory_order_release);

            return false;
        }

        const std::wstring directoryWide =
            directory.wstring();

        CopyWideString(
            directoryWide,
            g_state.directory.data(),
            g_state.directory.size());

        CopyUtf8AsWide(
            config.applicationName,
            g_state.applicationName.data(),
            g_state.applicationName.size());

        if (g_state.applicationName[0] ==
            L'\0')
        {
            CopyWideString(
                L"Orbit",
                g_state.applicationName.data(),
                g_state.applicationName.size());
        }

        g_state.writeMiniDump =
            config.writeMiniDump;

        SymSetOptions(
            SYMOPT_DEFERRED_LOADS |
            SYMOPT_UNDNAME |
            SYMOPT_LOAD_LINES);

        g_state.symbolsInitialized =
            SymInitialize(
                GetCurrentProcess(),
                nullptr,
                TRUE) != FALSE;

        g_state.previousExceptionFilter =
            SetUnhandledExceptionFilter(
                UnhandledExceptionFilter);

        g_state.previousTerminateHandler =
            std::set_terminate(
                TerminateHandler);

        g_handlingCrash.clear(
            std::memory_order_release);

        return true;
    }
    catch (...)
    {
        g_installed.store(
            false,
            std::memory_order_release);

        return false;
    }
}

void UninstallCrashHandler() noexcept
{
    if (!g_installed.exchange(
            false,
            std::memory_order_acq_rel))
    {
        return;
    }

    SetUnhandledExceptionFilter(
        g_state.previousExceptionFilter);

    std::set_terminate(
        g_state.previousTerminateHandler);

    if (g_state.symbolsInitialized)
    {
        static_cast<void>(
            SymCleanup(
                GetCurrentProcess()));

        g_state.symbolsInitialized =
            false;
    }
}
} // namespace orbit::platform
