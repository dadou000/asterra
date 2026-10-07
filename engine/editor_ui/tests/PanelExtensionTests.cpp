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
    // Set the state every frame so each assertion is independent from the
    // previous ImGui window state.
    ImGui::SetNextWindowCollapsed(
        collapsed,
        ImGuiCond_Always);

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

    // There is no renderer backend here, so build the font atlas explicitly: ImGui
    // 1.91 dereferences the atlas' default font in NewFrame().
    {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
            &pixels,
            &width,
            &height);
    }

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

    // ImGui hides a window's contents on the frame it is created (its size is not
    // known yet), so the first frame only brings the target panel into existence.
    BeginTestFrame();
    DrawTargetPanel();
    ImGui::EndFrame();

    drew = false;
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
    DrawTargetPanel(false);
    ImGui::EndFrame();
    Check(
        !drew,
        "removed extension no longer draws after target reopens");

    ImGui::DestroyContext();
    return EXIT_SUCCESS;
}
