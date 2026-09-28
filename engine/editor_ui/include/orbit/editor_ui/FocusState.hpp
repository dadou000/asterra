#pragma once

#include <string>

namespace orbit::editor_ui
{
// Returns the visible title of the Dear ImGui root window that currently owns
// navigation/focus. Empty when no UI context/window is focused. Studio uses
// this only as presentation state; project/runtime state must not depend on it.
[[nodiscard]] std::string FocusedWindowTitle();
} // namespace orbit::editor_ui
