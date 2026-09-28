#include <orbit/editor_ui/FocusState.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <string_view>

namespace orbit::editor_ui
{
std::string_view FocusedWindowTitle() noexcept
{
    ImGuiContext* const context = ImGui::GetCurrentContext();
    if (context == nullptr || context->NavWindow == nullptr)
    {
        return {};
    }

    ImGuiWindow* window = context->NavWindow;
    if (window->RootWindow != nullptr)
    {
        window = window->RootWindow;
    }

    if (window->Name == nullptr)
    {
        return {};
    }

    std::string_view title{window->Name};
    if (const std::size_t separator = title.find("###");
        separator != std::string_view::npos)
    {
        title = title.substr(0U, separator);
    }

    return title;
}
} // namespace orbit::editor_ui
