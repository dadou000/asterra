#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#include <orbit/platform/FileBrowser.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace orbit::platform
{
void OpenInFileBrowser(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error || !std::filesystem::exists(absolute, error))
    {
        throw std::runtime_error(
            "Cannot show a path that does not exist: " + path.generic_string());
    }

    HINSTANCE result = nullptr;
    if (std::filesystem::is_directory(absolute, error))
    {
        result = ShellExecuteW(
            nullptr, L"open", absolute.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else
    {
        // Explorer wants backslashes and the whole argument quoted.
        std::filesystem::path preferred = absolute;
        std::wstring native = preferred.make_preferred().wstring();
        const std::wstring arguments = L"/select,\"" + native + L"\"";
        result = ShellExecuteW(
            nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }

    // ShellExecute reports success with a value above 32.
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        throw std::runtime_error(
            "The file browser could not be opened for " + path.generic_string());
    }
}
} // namespace orbit::platform
