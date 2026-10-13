#include <orbit/studio_ui/DebugViewUi.hpp>
#include <cmath>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] const char* ViewportLabel(
    const std::string_view id) noexcept
{
    if (id == "studio.primary")
    {
        return "Perspective Viewport";
    }
    if (id == "studio.map")
    {
        return "Body Map / Debug Viewport";
    }

    return "Viewport";
}

[[nodiscard]] const char* SurfaceDebugModeName(
    const lighting::SurfaceDebugMode mode) noexcept
{
    switch (mode)
    {
    case lighting::SurfaceDebugMode::Lit:
        return "Lit";
    case lighting::SurfaceDebugMode::BaseColorRoughness:
        return "Albedo / Roughness";
    case lighting::SurfaceDebugMode::NormalMetallic:
        return "Normal / Metallic";
    case lighting::SurfaceDebugMode::EmissionMetadata:
        return "Emission / Meta";
    }

    return "Lit";
}
} // namespace

DebugViewUi::DebugViewUi(StudioRenderViewSet& views,
    studio_session::StudioSession& session) noexcept
    : views_(&views), session_(&session)
{
}

void DebugViewUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Debug",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Right,
        .dockOrder = 60,
        .minSize = {.width = 260.0F, .height = 160.0F},
        .draw =
            [this, &ui](editor_ui::PanelContext& context)
            {
                Draw(ui, context);
            }});
}

void DebugViewUi::Draw(
    editor_ui::EditorUi& ui,
    editor_ui::PanelContext& context)
{
    context.Heading("Debug / Test Center");
    context.MutedText("One entry point for rendering, terrain, lighting, performance and validation tools.");
    const auto open = [&](std::string_view title, std::string_view button) {
        if (context.Button(button))
        {
            if (!ui.FocusPanelByTitle(title))
                context.MutedText("This tool is not registered in the current workspace.");
        }
    };

    context.Separator();
    context.Heading("Terrain / Infrastructure");
    open("Surface Authoring", "Terrain authoring##dbg-terrain");
    context.SameLine();
    open("Body Map / Debug View", "Terrain map##dbg-map");
    context.MutedText("Road cut, railway grading, physical-page residency and displacement tests use the existing terrain tools. An automated validation experiment is not registered yet.");

    context.Heading("Construction terrain validation");
    context.MutedText("Read-only validation of the active observer physical page.");
    if (context.Button("Run validation##construction-terrain"))
    {
        constructionResult_ = "No live terrain viewport.";
        if (session_ != nullptr)
        {
            const auto runtimes = session_->TerrainRuntime().Catalog();
            for (const auto& runtime : runtimes)
            {
                if (runtime.viewportId != "studio.primary") continue;
                const auto address = runtime.observerPhysicalPage;
                const auto page = session_->TerrainPhysicalPages().Find(address);
                const auto status = session_->TerrainPhysicalPages().PageStatus(address);
                if (!page || !page->material || !status)
                    constructionResult_ = "NOT READY: observer physical page unavailable.";
                else if (page->revisionFingerprint == 0U ||
                         page->revisionFingerprint != status->revisionFingerprint)
                    constructionResult_ = "FAIL: published physical-page revision mismatch.";
                else
                {
                    const auto h = page->SampleSurfaceHeight(0.5, 0.5);
                    if (!h || !std::isfinite(*h))
                        constructionResult_ = "FAIL: nonfinite or missing center height.";
                    else
                        constructionResult_ = "PASS: physical page ready, revision matched, height finite.";
                }
                break;
            }
        }
    }
    context.KeyValue("Construction", constructionResult_);

    context.Separator();
    context.Heading("Lighting / Atmosphere");
    open("Display Diagnostics", "Display and eye adaptation##dbg-display");
    context.SameLine();
    open("Shading", "Shading##dbg-shading");
    open("Celestial", "Atmosphere and celestial##dbg-celestial");

    context.Separator();
    context.Heading("Performance / Reports");
    open("Profiler", "Profiler##dbg-profiler");
    context.SameLine();
    open("Reports", "Reports##dbg-reports");

    context.Separator();
    context.Heading("Viewport GBuffer");
    context.MutedText(
        "Chooses which GBuffer channel each viewport renders. "
        "The same switch is available per-viewport in its own toolbar, "
        "and over RPC/MCP as view.surface_debug_set.");

    if (views_ == nullptr)
    {
        context.ErrorText("No render-view set is bound.");
        return;
    }

    bool first = true;
    for (const auto& info : views_->Catalog())
    {
        if (!first)
        {
            context.Separator();
        }
        first = false;

        DrawViewportRow(context, info);
    }
}

void DebugViewUi::DrawViewportRow(
    editor_ui::PanelContext& context,
    const StudioRenderViewInfo& info)
{
    context.Text(ViewportLabel(info.id));

    const std::string lit = "Lit##debug-view-lit:" + info.id;
    const std::string base =
        "Albedo / Roughness##debug-view-base:" + info.id;
    const std::string normal =
        "Normal / Metallic##debug-view-normal:" + info.id;
    const std::string emission =
        "Emission / Meta##debug-view-emission:" + info.id;

    if (context.Button(lit))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::Lit);
    }
    context.SameLine();
    if (context.Button(base))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::BaseColorRoughness);
    }
    context.SameLine();
    if (context.Button(normal))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::NormalMetallic);
    }
    context.SameLine();
    if (context.Button(emission))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::EmissionMetadata);
    }

    context.KeyValue(
        "Current",
        SurfaceDebugModeName(info.surfaceDebugMode));
}
} // namespace orbit::studio_ui
