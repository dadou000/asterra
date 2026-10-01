#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <orbit/platform/Paths.hpp>

#include <algorithm>
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

namespace
{
[[nodiscard]] std::filesystem::path EnvironmentPath(
    const wchar_t* name)
{
    const DWORD required =
        GetEnvironmentVariableW(name, nullptr, 0);

    if (required == 0)
    {
        return {};
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(required));

    const DWORD written =
        GetEnvironmentVariableW(
            name,
            buffer.data(),
            required);

    if (written == 0 || written >= required)
    {
        return {};
    }

    return std::filesystem::path(buffer.data());
}
} // namespace

std::vector<std::filesystem::path> UsualProjectFolders()
{
    std::vector<std::filesystem::path> candidates;

    try
    {
        candidates.push_back(UserDataDirectory() / "Projects");
    }
    catch (const std::exception&)
    {
    }

    for (const wchar_t* base : {L"USERPROFILE", L"OneDrive", L"OneDriveConsumer"})
    {
        const auto root = EnvironmentPath(base);
        if (root.empty())
        {
            continue;
        }

        for (const char* folder : {"Documents", "Desktop", "Downloads"})
        {
            candidates.push_back(root / folder);
        }
    }

    std::vector<std::filesystem::path> result;
    for (const auto& candidate : candidates)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(candidate, error) || error)
        {
            continue;
        }

        const auto canonical =
            std::filesystem::weakly_canonical(candidate, error);
        const auto& resolved = error ? candidate : canonical;

        const bool duplicate = std::ranges::any_of(
            result,
            [&resolved](const std::filesystem::path& existing)
            {
                return existing == resolved;
            });

        if (!duplicate)
        {
            result.push_back(resolved);
        }
    }

    return result;
}

std::string EnvironmentVariable(const std::string_view name)
{
    const std::string owned(name);
    const DWORD required =
        GetEnvironmentVariableA(owned.c_str(), nullptr, 0);

    if (required == 0)
    {
        return {};
    }

    std::string value(static_cast<std::size_t>(required), char{});
    const DWORD written =
        GetEnvironmentVariableA(
            owned.c_str(),
            value.data(),
            required);

    if (written == 0 || written >= required)
    {
        return {};
    }

    value.resize(static_cast<std::size_t>(written));
    return value;
}

std::filesystem::path ExecutablePath()
{
    std::vector<wchar_t> buffer(
        1024);

    for (;;)
    {
        const DWORD written =
            GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()));

        if (written == 0)
        {
            throw std::runtime_error(
                "Failed to resolve executable path.");
        }

        if (written <
            buffer.size() - 1U)
        {
            return std::filesystem::
                weakly_canonical(
                    std::filesystem::path(
                        buffer.data()));
        }

        if (buffer.size() >=
            32768U)
        {
            throw std::runtime_error(
                "Executable path exceeds the Windows path limit.");
        }

        buffer.resize(
            buffer.size() * 2U);
    }
}
} // namespace orbit::platform
