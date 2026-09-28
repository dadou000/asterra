#include <orbit/editor_ui/EditorUi.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::editor_ui
{
namespace
{
ImGuiContext* g_shellContext = nullptr;
ImGuiID g_newFrameHookId = 0;
ImGuiID g_shutdownHookId = 0;
std::vector<ShellBandDefinition> g_shellBands;
} // namespace

class ShellBandRegistry
{
public:
    static void Upsert(ShellBandDefinition band)
    {
        if (band.id.empty() ||
            band.height <= 0.0F ||
            !band.draw)
        {
            throw std::invalid_argument(
                "Studio shell band requires a stable ID, positive height and draw callback.");
        }

        EnsureHooks();

        const auto existing =
            std::ranges::find_if(
                g_shellBands,
                [&band](const ShellBandDefinition& candidate)
                {
                    return candidate.id == band.id;
                });

        if (existing != g_shellBands.end())
        {
            *existing = std::move(band);
        }
        else
        {
            g_shellBands.push_back(std::move(band));
        }

        std::ranges::sort(
            g_shellBands,
            [](const ShellBandDefinition& a,
               const ShellBandDefinition& b)
            {
                if (a.order != b.order)
                {
                    return a.order < b.order;
                }

                return a.id < b.id;
            });
    }

    [[nodiscard]] static bool Remove(
        const std::string_view id) noexcept
    {
        const auto existing =
            std::ranges::find_if(
                g_shellBands,
                [id](const ShellBandDefinition& candidate)
                {
                    return candidate.id == id;
                });

        if (existing == g_shellBands.end())
        {
            return false;
        }

        g_shellBands.erase(existing);
        return true;
    }

private:
    static void EnsureHooks()
    {
        ImGuiContext* const context =
            ImGui::GetCurrentContext();

        if (context == nullptr)
        {
            throw std::logic_error(
                "Studio shell bands require an active EditorUi context.");
        }

        if (g_shellContext == context &&
            g_newFrameHookId != 0 &&
            g_shutdownHookId != 0)
        {
            return;
        }

        // Orbit Studio owns one EditorUi per process. Still reset stale state
        // defensively if a test destroys one context and creates another.
        g_shellContext = context;
        g_shellBands.clear();
        g_newFrameHookId = 0;
        g_shutdownHookId = 0;

        ImGuiContextHook newFrameHook{};
        newFrameHook.Type =
            ImGuiContextHookType_NewFramePost;
        newFrameHook.Callback =
            &ShellBandRegistry::NewFrameHook;
        g_newFrameHookId =
            ImGui::AddContextHook(
                context,
                &newFrameHook);

        ImGuiContextHook shutdownHook{};
        shutdownHook.Type =
            ImGuiContextHookType_Shutdown;
        shutdownHook.Callback =
            &ShellBandRegistry::ShutdownHook;
        g_shutdownHookId =
            ImGui::AddContextHook(
                context,
                &shutdownHook);
    }

    static void NewFrameHook(
        ImGuiContext* context,
        ImGuiContextHook*)
    {
        if (context != g_shellContext ||
            g_shellBands.empty())
        {
            return;
        }

        ImGui::SetCurrentContext(context);

        // The shell currently submits DockSpaceOverViewport before its real
        // BeginMainMenuBar call. Submit an empty first pass of the same
        // ##MainMenuBar window here: it reserves the canonical menu row now,
        // then DrawStudioShell appends the actual menus to that same window
        // later in the frame without reserving it a second time. This lets the
        // bands sit below File/Home/View while the dockspace sees all reserved
        // rows before it is created.
        if (ImGui::BeginMainMenuBar())
        {
            ImGui::EndMainMenuBar();
        }

        PanelContext panelContext(
            false,
            nullptr);

        ImGuiViewport* const viewport =
            ImGui::GetMainViewport();
        const f32 scale =
            CurrentUiScale();

        for (const ShellBandDefinition& band :
             g_shellBands)
        {
            const std::string windowName =
                "##orbit-shell-band-" + band.id;

            constexpr ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus;

            ImGui::PushStyleVar(
                ImGuiStyleVar_WindowPadding,
                ImVec2(
                    12.0F * scale,
                    5.0F * scale));
            ImGui::PushStyleVar(
                ImGuiStyleVar_WindowBorderSize,
                0.0F);
            ImGui::PushStyleColor(
                ImGuiCol_WindowBg,
                ImGui::GetStyle().Colors[
                    ImGuiCol_MenuBarBg]);

            const bool visible =
                ImGui::BeginViewportSideBar(
                    windowName.c_str(),
                    viewport,
                    ImGuiDir_Up,
                    band.height * scale,
                    flags);

            if (visible)
            {
                band.draw(panelContext);
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }
    }

    static void ShutdownHook(
        ImGuiContext* context,
        ImGuiContextHook*)
    {
        if (context != g_shellContext)
        {
            return;
        }

        g_shellBands.clear();
        g_shellContext = nullptr;
        g_newFrameHookId = 0;
        g_shutdownHookId = 0;
    }
};

void UpsertShellBand(
    ShellBandDefinition band)
{
    ShellBandRegistry::Upsert(
        std::move(band));
}

bool RemoveShellBand(
    const std::string_view id) noexcept
{
    return ShellBandRegistry::Remove(id);
}
} // namespace orbit::editor_ui
