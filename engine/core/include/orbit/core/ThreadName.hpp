#pragma once

#include <string_view>

namespace orbit::core
{
// Names the calling thread so debuggers, crash dumps and profilers show what it
// is ("Orbit.TerrainPages.3", "Orbit.GlobePatch", ...). Best effort: does
// nothing where the platform has no thread names. Safe to call repeatedly.
void SetCurrentThreadName(std::string_view name) noexcept;
} // namespace orbit::core
