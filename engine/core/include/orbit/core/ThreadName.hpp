#pragma once

#include <string_view>

namespace orbit::core
{
// Names the calling thread so debuggers, crash dumps and profilers show what it
// is ("Orbit.TerrainPages.3", "Orbit.GlobePatch", ...). Best effort: does
// nothing where the platform has no thread names. Safe to call repeatedly.
void SetCurrentThreadName(std::string_view name) noexcept;

// Best-effort scheduler hint for long procedural and file I/O tasks.
// Reserves responsiveness for foreground editor/render and simulation work.
void SetBackgroundThreadPriority() noexcept;

// Best-effort priority hint for the single foreground editor/render driver;
// this is not a substitute for an independently scheduled render thread.
void SetForegroundThreadPriority() noexcept;
} // namespace orbit::core
