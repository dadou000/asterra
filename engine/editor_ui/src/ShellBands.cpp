#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>

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
ImGuiID g_endFrameHookId = 0;
ImGuiID g_shutdownHookId = 0;
std::vector<ShellBandDefinition> g_shellBands;
std::vector<PanelExtensionDefinition> g_panelExtensions;
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
                if (a.edge != b.edge)
                {
                    return a.edge < b.edge;
                }

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

    static void UpsertExtension(
        PanelExtensionDefinition extension)
    {
        if (extension.id.empty() ||
            extension.targetTitle.empty() ||
            !extension.draw)
        {
            throw std::invalid_argument(
                "Editor panel extension requires a stable ID, target title and draw callback.");
        }

        EnsureHooks();

        const auto existing =
            std::ranges::find_if(
                g_panelExtensions,
                [&extension](const PanelExtensionDefinition& candidate)
                {
                    return candidate.id == extension.id;
                });

        if (existing != g_panelExtensions.end())
        {
            *existing = std::move(extension);
        }
        else
        {
            g_panelExtensions.push_back(
                std::move(extension));
        }

        std::ranges::sort(
            g_panelExtensions,
            [](const PanelExtensionDefinition& a,
               const PanelExtensionDefinition& b)
            {
                if (a.targetTitle != b.targetTitle)
                {
                    return a.targetTitle < b.targetTitle;
                }

                if (a.order != b.order)
                {
                    return a.order < b.order;
                }

                return a.id < b.id;
            });
    }

    [[nodiscard]] static bool RemoveExtension(
        const std::string_view id) noexcept
    {
        const auto existing =
            std::ranges::find_if(
                g_panelExtensions,
                [id](const PanelExtensionDefinition& candidate)
                {
                    return candidate.id == id;
                });

        if (existing == g_panelExtensions.end())
        {
            return false;
        }

        g_panelExtensions.erase(existing);
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
                "Studio shell extensions require an active EditorUi context.");
        }

        if (g_shellContext == context &&
            g_newFrameHookId != 0 &&
            g_endFrameHookId != 0 &&
            g_shutdownHookId != 0)
        {
            return;
        }

        // Orbit Studio owns one EditorUi per process. Still reset stale state
        // defensively if a test destroys one context and creates another.
        g_shellContext = context;
        g_shellBands.clear();
        g_panelExtensions.clear();
        g_newFrameHookId = 0;
        g_endFrameHookId = 0;
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

        // EditorUi has finished drawing registered panels before ImGui's
        // EndFrame phase. Re-opening an already-active window here appends to
        // the same ImGui window, which lets extension providers participate in
        // the real panel without owning its docking or visibility state.
        ImGuiContextHook endFrameHook{};
        endFrameHook.Type =
            ImGuiContextHookType_EndFramePre;
        endFrameHook.Callback =
            &ShellBandRegistry::EndFrameHook;
        g_endFrameHookId =
            ImGui::AddContextHook(
                context,
                &endFrameHook);

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
        // shell bands reserve their edges before the dockspace is created.
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

            const ImGuiDir direction =
                band.edge == ShellBandEdge::Bottom
                    ? ImGuiDir_Down
                    : ImGuiDir_Up;

            const bool visible =
                ImGui::BeginViewportSideBar(
                    windowName.c_str(),
                    viewport,
                    direction,
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

    static void EndFrameHook(
        ImGuiContext* context,
        ImGuiContextHook*)
    {
        if (context != g_shellContext ||
            g_panelExtensions.empty())
        {
            return;
        }

        ImGui::SetCurrentContext(context);
        PanelContext panelContext(
            false,
            nullptr);

        std::size_t begin = 0U;
        while (begin < g_panelExtensions.size())
        {
            const std::string& target =
                g_panelExtensions[begin].targetTitle;

            std::size_t end = begin + 1U;
            while (end < g_panelExtensions.size() &&
                   g_panelExtensions[end].targetTitle == target)
            {
                ++end;
            }

            ImGuiWindow* const window =
                ImGui::FindWindowByName(
                    target.c_str());

            // Active windows may still represent a background dock tab. Keep
            // extensions aligned with EditorUi's actual visible-tab contract.
            const bool visibleDockTab =
                window != nullptr &&
                (window->DockNode == nullptr ||
                 window->DockTabIsVisible);

            if (window != nullptr &&
                window->Active &&
                !window->Hidden &&
                !window->Collapsed &&
                visibleDockTab)
            {
                const bool visible =
                    ImGui::Begin(
                        target.c_str());

                if (visible)
                {
                    for (std::size_t index = begin;
                         index < end;
                         ++index)
                    {
                        g_panelExtensions[index].draw(
                            panelContext);
                    }
                }

                ImGui::End();
            }

            begin = end;
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
        g_panelExtensions.clear();
        g_shellContext = nullptr;
        g_newFrameHookId = 0;
        g_endFrameHookId = 0;
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

void UpsertPanelExtension(
    PanelExtensionDefinition extension)
{
    ShellBandRegistry::UpsertExtension(
        std::move(extension));
}

bool RemovePanelExtension(
    const std::string_view id) noexcept
{
    return ShellBandRegistry::RemoveExtension(id);
}
} // namespace orbit::editor_ui
