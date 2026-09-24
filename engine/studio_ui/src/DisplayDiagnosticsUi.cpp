#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>
#include <orbit/studio_ui/LightingDisplaySettingsRuntime.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/lighting/LightingScheduler.hpp>

#include <algorithm>
#include <format>
#include <string>

#define Register RegisterBase
#define DrawViewport DrawViewportBase
#include "DisplayDiagnosticsUiBase.cpp"
#undef DrawViewport
#undef Register

namespace orbit::studio_ui
{
namespace
{
void ApplyDisplayDefaults(
    StudioViewportRenderer& renderer,
    const StudioDisplayDefaults& defaults)
{
    for (const std::string_view viewport :
         {std::string_view("studio.primary"),
          std::string_view("studio.map")})
    {
        renderer.SetLuminanceHistogramConfig(
            viewport,
            defaults.histogram);
        renderer.SetHumanEyeAdaptationConfig(
            viewport,
            defaults.eye);
        renderer.ResetHumanEyeAdaptation(viewport);
        renderer.SetHighlightEffectsConfig(
            viewport,
            defaults.highlights);
        renderer.SetToneMappingConfig(
            viewport,
            defaults.toneMapping);
    }

    renderer.SetColorLutSettings(defaults.colorLut);

    if (!defaults.colorLutAsset.empty())
    {
        static_cast<void>(
            renderer.SelectColorLutAsset(defaults.colorLutAsset));
    }
    else
    {
        renderer.SetColorLut(
            post_process::BuildIdentityColorLut());
    }

    renderer.SetOutputTransformSettings(defaults.output);
}
} // namespace

void DisplayDiagnosticsUi::Register(
    editor_ui::EditorUi& ui)
{
    if (renderer_ != nullptr)
    {
        RegisterStudioDisplayDefaultsConsumer(
            this,
            [this](const StudioDisplayDefaults& defaults)
            {
                if (renderer_ != nullptr)
                {
                    ApplyDisplayDefaults(*renderer_, defaults);
                }
            });
    }

    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Display Diagnostics",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Right,
        .dockOrder = 55,
        .minSize = {.width = 320.0F,.height = 260.0F},
        .draw = [this](editor_ui::PanelContext& context)
        {
            DrawViewport(context, "studio.primary", "Primary View");
            context.Separator();
            DrawViewport(context, "studio.map", "Body Map");
        }
    });
}

void DisplayDiagnosticsUi::DrawViewport(
    editor_ui::PanelContext& context,
    const std::string_view viewportId,
    const std::string_view label)
{
    DrawViewportBase(context, viewportId, label);

    if (viewportId != "studio.primary")
    {
        return;
    }

    context.Separator();
    context.Heading("Lighting Runtime Override");
    context.MutedText(
        "These controls are session overrides. Project Settings owns the persisted defaults.");

    auto config =
        lighting::StudioLightingRuntimeConfig().
            value_or(lighting::LightingSchedulerConfig{});

    bool hardwareRt = config.hardwareRayQueryEnabled;
    f64 emissiveQuality = config.emissiveGiQualityScale;

    bool changed =
        context.Checkbox("Hardware Ray Query##m40-hwrt", hardwareRt);
    changed |=
        context.InputDouble(
            "Emissive GI Quality##m40-emissive-quality",
            emissiveQuality);

    context.MutedText(
        "Ray query changes visibility backend selection only. Emissive GI quality changes emissive refresh work only; neither changes the lighting model.");

    context.Text("Advanced GPU Budgets (ms)");

    f64 direct = config.budget.directLightingMs;
    f64 visibility = config.budget.visibilityMs;
    f64 gi = config.budget.giMs;
    f64 reflections = config.budget.reflectionMs;
    f64 emissive = config.budget.emissiveMs;
    f64 post = config.budget.postProcessMs;

    changed |= context.InputDouble("Direct##m40-budget-direct", direct);
    changed |= context.InputDouble("Visibility##m40-budget-visibility", visibility);
    changed |= context.InputDouble("GI##m40-budget-gi", gi);
    changed |= context.InputDouble("Reflections##m40-budget-reflections", reflections);
    changed |= context.InputDouble("Emissive##m40-budget-emissive", emissive);
    changed |= context.InputDouble("Post Process##m40-budget-post", post);

    if (changed)
    {
        config.hardwareRayQueryEnabled = hardwareRt;
        config.emissiveGiQualityScale =
            static_cast<f32>(std::clamp(emissiveQuality, 0.0, 4.0));
        config.budget.directLightingMs =
            static_cast<f32>(std::max(direct, 0.0));
        config.budget.visibilityMs =
            static_cast<f32>(std::max(visibility, 0.0));
        config.budget.giMs =
            static_cast<f32>(std::max(gi, 0.0));
        config.budget.reflectionMs =
            static_cast<f32>(std::max(reflections, 0.0));
        config.budget.emissiveMs =
            static_cast<f32>(std::max(emissive, 0.0));
        config.budget.postProcessMs =
            static_cast<f32>(std::max(post, 0.0));

        lighting::SetStudioLightingRuntimeConfig(config);
    }

    const auto runtime = lighting::StudioLightingRuntimeConfig();
    if (runtime.has_value())
    {
        context.MutedText(
            std::format(
                "Runtime lighting budget {:.2f} ms | emissive quality x{:.2f} | ray query {}",
                runtime->budget.TotalMs(),
                runtime->emissiveGiQualityScale,
                runtime->hardwareRayQueryEnabled ? "enabled" : "disabled"));
    }

    context.Separator();
    context.Heading("Viewport Lighting Inspection");
    context.MutedText(
        "Transient production inspection state. It never alters lighting authority, project state, GI cache identity or renderer budgets.");

    auto& overlays = StudioLightingOverlays();

    bool showGi = overlays.giUpdateCells;
    bool showCache = overlays.radianceCacheRegions;
    bool showReflections = overlays.reflectionInspection;
    bool showEmissive = overlays.emissiveInfluence;

    bool overlayChanged =
        context.Checkbox("GI Update Cells##m41-gi-cells", showGi);
    overlayChanged |=
        context.Checkbox("Radiance Cache Regions##m41-cache-regions", showCache);
    overlayChanged |=
        context.Checkbox("Reflection Inspection##m41-reflections", showReflections);
    overlayChanged |=
        context.Checkbox("Emissive Affected Cells##m41-emissive", showEmissive);

    i64 maximumCells = static_cast<i64>(overlays.maximumGiCells);
    i64 cacheLevels = static_cast<i64>(overlays.cacheLevels);

    overlayChanged |=
        context.InputInteger("Maximum GI Cells##m41-max-cells", maximumCells);
    overlayChanged |=
        context.InputInteger("Cache Levels##m41-cache-levels", cacheLevels);

    if (overlayChanged)
    {
        overlays.giUpdateCells = showGi;
        overlays.radianceCacheRegions = showCache;
        overlays.reflectionInspection = showReflections;
        overlays.emissiveInfluence = showEmissive;
        overlays.maximumGiCells =
            static_cast<u32>(std::clamp<i64>(maximumCells, 1, 256));
        overlays.cacheLevels =
            static_cast<u32>(std::clamp<i64>(cacheLevels, 1, 8));
    }

    auto& interaction = StudioLightingInteractionState();

    if (renderer_ != nullptr)
    {
        if (const auto emissiveDiagnostics =
                renderer_->EmissiveGiDiagnostics("studio.primary");
            emissiveDiagnostics.has_value())
        {
            interaction.trackedEmissiveSources =
                emissiveDiagnostics->trackedSources;
            interaction.invalidationEventsThisFrame =
                emissiveDiagnostics->invalidationEventsThisFrame;
            interaction.dirtyRadianceCells =
                emissiveDiagnostics->dirtyRadianceCells;
            interaction.scheduledRadianceUpdates =
                emissiveDiagnostics->scheduledRadianceUpdates;
        }

        if (const auto visibilityDiagnostics =
                renderer_->VisibilityProxyDiagnostics("studio.primary");
            visibilityDiagnostics.has_value())
        {
            interaction.hardwareRayQuerySupported =
                visibilityDiagnostics->hardwareRayQuerySupported;
            interaction.hardwareRayQueryReady =
                visibilityDiagnostics->hardwareRayQueryReady;
            interaction.hardwarePrimitiveCount =
                visibilityDiagnostics->hardwarePrimitiveCount;
        }
    }

    context.Text(
        std::format(
            "Emissive sources {} | invalidations {} | dirty cache cells {} | scheduled {}",
            interaction.trackedEmissiveSources,
            interaction.invalidationEventsThisFrame,
            interaction.dirtyRadianceCells,
            interaction.scheduledRadianceUpdates));

    context.Text(
        std::format(
            "Ray query: {} / {} | proxy primitives {}",
            interaction.hardwareRayQuerySupported ? "supported" : "unsupported",
            interaction.hardwareRayQueryReady ? "ready" : "not ready",
            interaction.hardwarePrimitiveCount));

    if (interaction.hasSelection)
    {
        context.Text("Selected: " + interaction.selectedObject);

        if (interaction.selectedEmissive)
        {
            context.Text(
                std::format(
                    "Emissive GI: {:.1f} nits | GI scale x{:.3f} | {}",
                    interaction.emissionLuminanceNits,
                    interaction.emissionGiScale,
                    interaction.emissionGiEnabled
                        ? "contributing"
                        : "visible only"));
        }
        else
        {
            context.MutedText(
                "Selected object has no positive physical material emission authority.");
        }
    }
    else
    {
        context.MutedText(
            "Selected-emissive inspection seam is ready; viewport/session selection binding is the remaining M41 integration step.");
    }
}
} // namespace orbit::studio_ui
