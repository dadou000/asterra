#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::platform
{
// Per-user writable Orbit data root. On Windows this resolves beneath
// LOCALAPPDATA and never points inside a source project.
[[nodiscard]] std::filesystem::path UserDataDirectory();


// Folders where people usually keep projects: Documents, Desktop and
// Downloads (including their OneDrive-redirected forms) plus Orbit's own
// Projects folder. Only folders that exist are returned.
[[nodiscard]] std::vector<std::filesystem::path> UsualProjectFolders();

// Value of an environment variable, or empty when it is unset.
[[nodiscard]] std::string EnvironmentVariable(std::string_view name);

// Absolute path of the currently running executable. This is the stable
// anchor used by Studio/OrbitBuild to locate sibling runtime binaries.
[[nodiscard]] std::filesystem::path ExecutablePath();
} // namespace orbit::platform
