#pragma once

#include <string_view>

namespace orbit::editor_ui
{
// Returns the visible title of the Dear ImGui root window that currently owns
// navigation/focus. The view aliases ImGui-owned window-name storage and is
// valid until that window is destroyed or renamed. Empty when nothing is focused.
[[nodiscard]] std::string_view FocusedWindowTitle() noexcept;
} // namespace orbit::editor_ui
