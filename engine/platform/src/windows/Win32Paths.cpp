#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <orbit/platform/Paths.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::platform
{
std::filesystem::path UserDataDirectory()
{
    const DWORD required =
        GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            nullptr,
            0);

    if (required == 0)
    {
        throw std::runtime_error(
            "LOCALAPPDATA is unavailable.");
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(
            required));

    const DWORD written =
        GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            buffer.data(),
            required);

    if (written == 0 ||
        written >= required)
    {
        throw std::runtime_error(
            "Failed to resolve LOCALAPPDATA.");
    }

    return std::filesystem::path(
               buffer.data()) /
        "Orbit";
}
} // namespace orbit::platform
