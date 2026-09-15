#pragma once

#include <filesystem>

namespace orbit::platform
{
// Per-user writable Orbit data root. On Windows this resolves beneath
// LOCALAPPDATA and never points inside a source project.
[[nodiscard]] std::filesystem::path UserDataDirectory();
} // namespace orbit::platform
