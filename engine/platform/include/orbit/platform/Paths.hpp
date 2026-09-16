#pragma once

#include <filesystem>

namespace orbit::platform
{
// Per-user writable Orbit data root. On Windows this resolves beneath
// LOCALAPPDATA and never points inside a source project.
[[nodiscard]] std::filesystem::path UserDataDirectory();


// Absolute path of the currently running executable. This is the stable
// anchor used by Studio/OrbitBuild to locate sibling runtime binaries.
[[nodiscard]] std::filesystem::path ExecutablePath();
} // namespace orbit::platform
