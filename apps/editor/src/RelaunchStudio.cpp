#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace orbit::editor_app
{
// Starts a fresh Studio process opened on `projectManifest`. The caller is
// expected to exit its own event loop afterwards; project-bound runtime state
// is intentionally rebuilt from scratch rather than hot-swapped.
bool RelaunchStudioWithProject(
    const std::filesystem::path& projectManifest)
{
#ifdef _WIN32
    wchar_t exePath[MAX_PATH]{};

    if (GetModuleFileNameW(
            nullptr,
            exePath,
            MAX_PATH) == 0U)
    {
        return false;
    }

    std::wstring commandLine =
        L"\"" + std::wstring(exePath) + L"\" \"" +
        projectManifest.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (CreateProcessW(
            exePath,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process) == FALSE)
    {
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    static_cast<void>(projectManifest);
    return false;
#endif
}
} // namespace orbit::editor_app
