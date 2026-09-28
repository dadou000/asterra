#include <orbit/editor_ui/PanelExtensions.hpp>

#include <imgui.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
void Check(
    const bool condition,
    const std::string_view what)
{
    if (!condition)
    {
        std::cerr <<
            "Panel extension test failed: " <<
            what << "\n";
        std::exit(EXIT_FAILURE);
    }
}

void BeginTestFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 720.0F);
    io.DeltaTime = 1.0F / 60.0F;
    ImGui::NewFrame();
}

void DrawTargetPanel(
    const bool collapsed = false)
{
    if (collapsed)
    {
        ImGui::SetNextWindowCollapsed(
            true,
            ImGuiCond_Always);
    }

    if (ImGui::Begin("Inspector"))
    {
        ImGui::TextUnformatted("Core Inspector");
    }
    ImGui::End();
}
} // namespace

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    bool drew = false;
    orbit::editor_ui::UpsertPanelExtension({
        .id = "test.inspector.extension",
        .targetTitle = "Inspector",
        .order = 10,
        .draw =
            [&drew](orbit::editor_ui::PanelContext& context)
            {
                drew = true;
                context.Text("Extended Inspector");
            }
    });

    BeginTestFrame();
    DrawTargetPanel();
    ImGui::EndFrame();
    Check(
        drew,
        "extension draws after the active target panel");

    drew = false;
    BeginTestFrame();
    DrawTargetPanel(true);
    ImGui::EndFrame();
    Check(
        !drew,
        "collapsed target suppresses extension drawing");

    Check(
        orbit::editor_ui::RemovePanelExtension(
            "test.inspector.extension"),
        "registered extension can be removed");

    drew = false;
    BeginTestFrame();
    DrawTargetPanel();
    ImGui::EndFrame();
    Check(
        !drew,
        "removed extension no longer draws");

    ImGui::DestroyContext();
    return EXIT_SUCCESS;
}
