#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>
#include <orbit/studio_ui/LightingDisplaySettingsRuntime.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/studio_ui/LightingSelectionInspection.hpp>
#include <orbit/studio_ui/StudioRuntimeProfiler.hpp>
#include <orbit/lighting/LightingRuntimeProfiler.hpp>
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

[[nodiscard]] f64 Mebibytes(const u64 bytes) noexcept
{
    return static_cast<f64>(bytes) /
        (1024.0 * 1024.0);
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

    if (inspectionSession_ != nullptr &&
        inspectionContent_ != nullptr)
    {
        PublishSelectedLightingAuthority(
            *inspectionSession_,
            *inspectionContent_);
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
    else if (inspectionSession_ == nullptr ||
             inspectionContent_ == nullptr)
    {
        context.MutedText(
            "Selected-emissive inspection is not bound to this Studio host.");
    }
    else
    {
        context.MutedText(
            "Select one authored object to inspect its physical emissive GI authority.");
    }

    context.Separator();
    context.Heading("Lighting / Volume Profiler");
    context.MutedText(
        "Live production telemetry. Budgets are scheduler targets; measured values are smoothed GPU timestamps from completed frames.");

    const auto& lightingProfile =
        lighting::StudioLightingRuntimeProfiler();

    if (lightingProfile.hasScheduledPlan)
    {
        const auto& budget = lightingProfile.scheduled.budget;
        const auto& measured = lightingProfile.measured;

        const auto drawLightingSection =
            [&context, &budget, &measured](
                const std::string_view sectionName,
                const lighting::LightingGpuSection section)
            {
                if (measured.HasSection(section))
                {
                    context.Text(
                        std::format(
                            "{}: budget {:.2f} ms | measured {:.2f} ms",
                            sectionName,
                            budget.SectionMs(section),
                            measured.SectionMs(section)));
                }
                else
                {
                    context.Text(
                        std::format(
                            "{}: budget {:.2f} ms | measured --",
                            sectionName,
                            budget.SectionMs(section)));
                }
            };

        drawLightingSection("Direct", lighting::LightingGpuSection::Direct);
        drawLightingSection("Visibility", lighting::LightingGpuSection::Visibility);
        drawLightingSection("GI", lighting::LightingGpuSection::Gi);
        drawLightingSection("Reflections", lighting::LightingGpuSection::Reflections);
        drawLightingSection("Emissive", lighting::LightingGpuSection::Emissive);
        drawLightingSection("Post", lighting::LightingGpuSection::PostProcess);

        context.Text(
            std::format(
                "Lighting total: budget {:.2f} ms | measured {}",
                budget.TotalMs(),
                lightingProfile.hasMeasuredTimings
                    ? std::format("{:.2f} ms", measured.TotalMs())
                    : std::string("--")));

        const auto& requested = lightingProfile.requested;
        const auto& scheduled = lightingProfile.scheduled;

        context.Text(
            std::format(
                "Visibility queries: requested {} | scheduled {} | scale {:.2f}",
                requested.exactVisibilityQueries,
                scheduled.exactVisibilityQueries,
                scheduled.visibilityScale));
        context.Text(
            std::format(
                "Radiance updates: requested {} | scheduled {} | scale {:.2f}",
                requested.radianceCacheUpdates,
                scheduled.radianceCacheUpdates,
                scheduled.giScale));
        context.Text(
            std::format(
                "Reflection queries: requested {} | scheduled {} | scale {:.2f}",
                requested.reflectionQueries,
                scheduled.reflectionQueries,
                scheduled.reflectionScale));
        context.Text(
            std::format(
                "Emissive updates: requested {} | scheduled {} | scale {:.2f} | quality x{:.2f}",
                requested.emissiveUpdates,
                scheduled.emissiveUpdates,
                scheduled.emissiveScale,
                lightingProfile.config.emissiveGiQualityScale));
        context.Text(
            std::format(
                "Ray-query backend: capability {} | policy {} | selected {}",
                scheduled.hardwareRayQueryAvailable ? "available" : "unavailable",
                lightingProfile.config.hardwareRayQueryEnabled ? "enabled" : "disabled",
                scheduled.preferHardwareRayQuery ? "hardware" : "shared fallback"));

        context.Text(
            std::format(
                "GI cache/emission: dirty {} | scheduled {} | emitters {} | invalidations {}",
                interaction.dirtyRadianceCells,
                interaction.scheduledRadianceUpdates,
                interaction.trackedEmissiveSources,
                interaction.invalidationEventsThisFrame));
    }
    else
    {
        context.MutedText(
            "Waiting for the lighting scheduler to publish a production work plan.");
    }

    context.Separator();
    context.Text("Selected Volume Runtime");

    const auto& volumeProfile =
        StudioVolumeRuntimeProfiler();

    if (!volumeProfile.hasSelection)
    {
        context.MutedText(
            "Select a Volume, Volume Source, or Volume Effector to inspect its live field/solver/raymarch state.");
        return;
    }

    if (volumeProfile.hasFields)
    {
        const auto& fields = volumeProfile.fields;
        context.Text(
            std::format(
                "Fields: {}x{}x{} | tile {} | resident {}/{} | pending {} | channels {}",
                fields.resolutionX,
                fields.resolutionY,
                fields.resolutionZ,
                fields.tileEdge,
                fields.residentTiles,
                fields.validTiles,
                fields.pendingTiles,
                fields.channels.size()));
        context.Text(
            std::format(
                "Field memory: {:.2f} MiB total ({:.2f} MiB channels + {:.2f} MiB residency) | invalidated {}",
                Mebibytes(fields.totalBytes),
                Mebibytes(fields.channelBytes),
                Mebibytes(fields.residencyBytes),
                volumeProfile.invalidatedTiles));
        context.Text(
            std::format(
                "Residency churn: +{} new | {} reused | -{} evicted",
                fields.lastUpdate.newTiles,
                fields.lastUpdate.reusedTiles,
                fields.lastUpdate.evictedTiles));
    }
    else
    {
        context.MutedText(
            "Selected volume has no resident field storage in this frame.");
    }

    if (volumeProfile.hasSolver)
    {
        const auto& solver = volumeProfile.solver;
        context.Text(
            std::format(
                "Solver: {}{} | iterations {}/{} | scalar channels {} | sources {} | effectors {}",
                solver.live ? "live" : "idle",
                solver.paused ? " / paused" : "",
                solver.iterationsThisFrame,
                solver.requestedIterations,
                solver.scalarChannelsSolved,
                solver.sourceCount,
                solver.effectorCount));

        if (solver.gpuTimingValid)
        {
            context.Text(
                std::format(
                    "Solver GPU: {:.3f} / {:.3f} ms | simulated {:.4f} s | scratch {:.2f} MiB | metadata {:.2f} MiB",
                    solver.gpuMilliseconds,
                    solver.gpuBudgetMilliseconds,
                    solver.simulatedSeconds,
                    Mebibytes(solver.scratchBytes),
                    Mebibytes(solver.metadataBytes)));
        }
        else
        {
            context.Text(
                std::format(
                    "Solver GPU: timing pending | budget {:.3f} ms | scratch {:.2f} MiB | metadata {:.2f} MiB",
                    solver.gpuBudgetMilliseconds,
                    Mebibytes(solver.scratchBytes),
                    Mebibytes(solver.metadataBytes)));
        }
    }

    if (volumeProfile.hasRenderer)
    {
        const auto& render = volumeProfile.renderer;
        context.Text(
            std::format(
                "Raymarch: rendered {} | representation {} | steps {} + shadow {} | local lights {} | resident tiles {}",
                render.rendered ? "yes" : "no",
                static_cast<u32>(render.representation),
                render.raymarchSteps,
                render.shadowSteps,
                render.localLightCount,
                render.residentTiles));
        context.Text(
            std::format(
                "Volume LOD: live {:.2f} | coarse {:.2f} | passive {:.2f} | baked {:.2f} | projected {:.1f}px | distance {:.1f}m",
                render.liveWeight,
                render.coarseWeight,
                render.passiveWeight,
                render.bakedWeight,
                render.projectedDiameterPixels,
                render.distanceToBoundsMeters));
        context.Text(
            std::format(
                "Temporal: history {} | transition {} | baked fallback {} | memory {:.2f} MiB history + {:.2f} MiB scratch",
                render.historyValid ? "valid" : "invalid",
                render.representationTransition ? "active" : "stable",
                render.bakedFallback ? "yes" : "no",
                Mebibytes(render.historyBytes),
                Mebibytes(render.scratchBytes)));
    }
}
} // namespace orbit::studio_ui
