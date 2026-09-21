#include <orbit/studio_ui/StudioUiBundle.hpp>

#include <format>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
StudioUiBundle::StudioUiBundle(
    editor_ui::EditorUi& ui,
    rhi::Device& device,
    const shader::Compiler& compiler,
    studio_session::StudioWorkspace& workspace,
    std::filesystem::path recentProjectsFile)
    : device_(&device),
      workspace_(&workspace),
      projectAuthoring_(
          workspace,
          std::move(recentProjectsFile)),
      surfaceAuthoring_(workspace),
      viewportRenderer_(
          device,
          compiler)
{
    projectAuthoring_.SetWorkspaceChangedCallback(
        [this]()
        {
            static_cast<void>(SynchronizeProject());
        });

    projectAuthoring_.Register(ui);
    surfaceAuthoring_.Register(ui);
    viewportPanels_.Register(ui);

    ui.RegisterPanel({
        .id = {
            .high = 0x4f5242495443454cULL,
            .low = 0x504552464d333101ULL
        },
        .title = "Celestial Performance",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 40,
        .minSize = {
            .width = 280.0F,
            .height = 180.0F
        },
        .draw =
            [this](
                editor_ui::PanelContext& context)
            {
                const auto stats =
                    viewportRenderer_.
                        CelestialSchedulerStats();
                const auto budget =
                    viewportRenderer_.
                        CelestialSchedulerBudget();

                context.Text(
                    std::format(
                        "CPU grants: {}/{} jobs, {}/{} cost",
                        stats.cpuJobsGranted,
                        budget.maxCpuJobsPerFrame,
                        stats.cpuCostGranted,
                        budget.maxCpuCostUnitsPerFrame));

                context.Text(
                    std::format(
                        "GPU grants: {}/{} jobs, {}/{} cost",
                        stats.gpuJobsGranted,
                        budget.maxGpuJobsPerFrame,
                        stats.gpuCostGranted,
                        budget.maxGpuCostUnitsPerFrame));

                context.Separator();

                context.Text(
                    std::format(
                        "Pending: {} / {}",
                        stats.pendingRequests,
                        budget.maxPendingRequests));

                context.Text(
                    std::format(
                        "In flight: {}",
                        stats.inFlightRequests));

                context.Text(
                    std::format(
                        "Stale completions rejected: {}",
                        stats.staleCompletionsRejected));

                context.Text(
                    std::format(
                        "Queue drops: {}",
                        stats.queueDrops));

                context.Separator();

                if (stats.pendingRequests == 0U &&
                    stats.inFlightRequests == 0U)
                {
                    context.Text(
                        "Celestial derived caches are current.");
                }
                else
                {
                    context.Text(
                        "Derived celestial work is being budgeted across frames.");
                }

                context.Separator();
                context.Text("Advanced Representation Quality");

                auto quality =
                    viewportRenderer_.
                        CelestialQualityPolicy();

                bool qualityChanged = false;

                qualityChanged |=
                    context.InputDouble(
                        "Quality Scale##celestial-quality-scale",
                        quality.qualityScale);

                qualityChanged |=
                    context.InputDouble(
                        "Surface Error (px)##celestial-quality-surface",
                        quality.productionSurfaceErrorPixels);

                qualityChanged |=
                    context.InputDouble(
                        "Macro Error (px)##celestial-quality-macro",
                        quality.macroDisplacementErrorPixels);

                qualityChanged |=
                    context.InputDouble(
                        "Smooth Globe Min Radius (px)##celestial-quality-smooth",
                        quality.smoothGlobeMinimumRadiusPixels);

                qualityChanged |=
                    context.InputDouble(
                        "Disc Min Radius (px)##celestial-quality-disc",
                        quality.discImpostorMinimumRadiusPixels);

                qualityChanged |=
                    context.InputDouble(
                        "LOD Hysteresis##celestial-quality-hysteresis",
                        quality.hysteresisFraction);

                if (qualityChanged)
                {
                    viewportRenderer_.
                        SetCelestialQualityPolicy(
                            quality);
                }

                if (context.Button(
                        "Reset Celestial Quality##celestial-quality-reset"))
                {
                    viewportRenderer_.
                        SetCelestialQualityPolicy({});
                }

                const auto drawRepresentationDiagnostics =
                    [this, &context](
                        const std::string_view viewportId,
                        const std::string_view label)
                    {
                        const auto transition =
                            viewportRenderer_.
                                SurfaceGlobeTransitionDiagnostics(
                                    viewportId);

                        if (!transition.has_value())
                        {
                            return;
                        }

                        const auto qualityPolicy =
                            viewportRenderer_.
                                CelestialQualityPolicy();

                        context.Separator();
                        context.Text(
                            std::format(
                                "{}: {}",
                                label,
                                celestial_representation::
                                    Name(
                                        transition->
                                            representation)));

                        context.Text(
                            std::format(
                                "Projected radius: {:.3f} px",
                                transition->
                                    projectedRadiusPixels));

                        switch (
                            transition->
                                representation)
                        {
                        case celestial_representation::
                                 Representation::
                                     ProductionSurface:
                            context.Text(
                                std::format(
                                    "Why: production detail error {:.3f} px >= {:.3f} px threshold.",
                                    transition->
                                        productionDetailErrorPixels,
                                    qualityPolicy.
                                        productionSurfaceErrorPixels /
                                        qualityPolicy.qualityScale));
                            break;

                        case celestial_representation::
                                 Representation::
                                     MacroDisplacedGlobe:
                            context.Text(
                                std::format(
                                    "Why: macro displacement error {:.3f} px >= {:.3f} px threshold.",
                                    transition->
                                        macroDisplacementErrorPixels,
                                    qualityPolicy.
                                        macroDisplacementErrorPixels /
                                        qualityPolicy.qualityScale));
                            break;

                        case celestial_representation::
                                 Representation::
                                     SmoothGlobe:
                            context.Text(
                                std::format(
                                    "Why: apparent radius is above the smooth-globe threshold ({:.3f} px).",
                                    qualityPolicy.
                                        smoothGlobeMinimumRadiusPixels /
                                        qualityPolicy.qualityScale));
                            break;

                        case celestial_representation::
                                 Representation::
                                     AnalyticDiscImpostor:
                        case celestial_representation::
                                 Representation::
                                     CachedDiscImpostor:
                            context.Text(
                                std::format(
                                    "Why: apparent radius is below smooth-globe and above disc threshold ({:.3f} px).",
                                    qualityPolicy.
                                        discImpostorMinimumRadiusPixels /
                                        qualityPolicy.qualityScale));
                            break;

                        case celestial_representation::
                                 Representation::
                                     PointProxy:
                        case celestial_representation::
                                 Representation::
                                     StellarPointProxy:
                            context.Text(
                                std::format(
                                    "Why: apparent radius is below the disc threshold ({:.3f} px).",
                                    qualityPolicy.
                                        discImpostorMinimumRadiusPixels /
                                        qualityPolicy.qualityScale));
                            break;
                        }

                        if (transition->
                                hysteresisHeld)
                        {
                            context.Text(
                                "Hysteresis is holding the previous representation.");
                        }

                        if (transition->
                                overlapping)
                        {
                            context.Text(
                                "Representation cross-fade is active.");
                        }
                    };

                drawRepresentationDiagnostics(
                    "studio.primary",
                    "Primary View");

                drawRepresentationDiagnostics(
                    "studio.map",
                    "Body Map");

                const auto drawPhysicalDiagnostics =
                    [this, &context](
                        const std::string_view viewportId,
                        const std::string_view label)
                    {
                        const auto atmosphere =
                            viewportRenderer_.
                                AtmosphereDiagnostics(
                                    viewportId);
                        const auto clouds =
                            viewportRenderer_.
                                CloudDiagnostics(
                                    viewportId);
                        const auto rings =
                            viewportRenderer_.
                                RingDiagnostics(
                                    viewportId);
                        const auto magnetosphere =
                            viewportRenderer_.
                                MagnetosphereDiagnostics(
                                    viewportId);
                        const auto compact =
                            viewportRenderer_.
                                CompactObjectDiagnostics(
                                    viewportId);
                        const auto lighting =
                            viewportRenderer_.
                                CelestialLightingDiagnostics(
                                    viewportId);

                        if (!atmosphere.has_value() &&
                            !clouds.has_value() &&
                            !rings.has_value() &&
                            !magnetosphere.has_value() &&
                            !compact.has_value() &&
                            !lighting.has_value())
                        {
                            return;
                        }

                        context.Separator();
                        context.Text(
                            std::format(
                                "{} Physical Telemetry",
                                label));

                        if (lighting.has_value())
                        {
                            context.Text(
                                std::format(
                                    "Lighting: visible {:.3f} | {:.3f} W/m2 | {} occluders",
                                    lighting->
                                        visibleFraction,
                                    lighting->
                                        irradianceWattsPerSquareMeter,
                                    lighting->
                                        contributingOccluders));
                        }

                        if (atmosphere.has_value())
                        {
                            context.Text(
                                std::format(
                                    "Atmosphere LUT: static {} | sky {}",
                                    atmosphere->
                                        staticFingerprint,
                                    atmosphere->
                                        skyFingerprint));

                            context.Text(
                                std::format(
                                    "  T {}x{} | MS {}x{} | Sky {}x{}",
                                    atmosphere->
                                        transmittanceWidth,
                                    atmosphere->
                                        transmittanceHeight,
                                    atmosphere->
                                        multiScatteringWidth,
                                    atmosphere->
                                        multiScatteringHeight,
                                    atmosphere->
                                        skyViewWidth,
                                    atmosphere->
                                        skyViewHeight));
                        }

                        if (clouds.has_value())
                        {
                            context.Text(
                                std::format(
                                    "Clouds: {} layers | fingerprint {} | GPU {}",
                                    clouds->
                                        layerCount,
                                    clouds->
                                        fingerprint,
                                    clouds->
                                            gpuResident
                                        ? "resident"
                                        : "pending"));

                            context.Text(
                                std::format(
                                    "  coverage {:.3f} | optical depth {:.3f}",
                                    clouds->
                                        meanCoverage,
                                    clouds->
                                        meanOpticalDepth));
                        }

                        if (rings.has_value())
                        {
                            context.Text(
                                std::format(
                                    "Rings: {} bands | {} seg | far samples {}",
                                    rings->
                                        bandCount,
                                    rings->
                                        angularSegments,
                                    rings->
                                        farProfileSamples));
                        }

                        if (magnetosphere.has_value())
                        {
                            context.Text(
                                std::format(
                                    "Aurora: activity {:.3f} | {} seg | {}",
                                    magnetosphere->
                                        activity,
                                    magnetosphere->
                                        angularSegments,
                                    magnetosphere->
                                            nearRepresentation
                                        ? "near"
                                        : "far"));

                            context.Text(
                                std::format(
                                    "  magnetopause {:.3f} km | tail {:.3f} km",
                                    magnetosphere->
                                        subsolarStandoffMeters /
                                        1000.0,
                                    magnetosphere->
                                        tailExtentMeters /
                                        1000.0));
                        }

                        if (compact.has_value())
                        {
                            context.Text(
                                std::format(
                                    "Compact: {} | shadow {:.3f} px | optical {:.3f} px",
                                    celestial_representation::
                                        Name(
                                            compact->
                                                representation),
                                    compact->
                                        projectedShadowRadiusPixels,
                                    compact->
                                        projectedOpticalRadiusPixels));

                            context.Text(
                                std::format(
                                    "  rg {:.3f} km | Rs {:.3f} km | photon {:.3f} km | ISCO {:.3f} km",
                                    compact->
                                        gravitationalRadiusMeters /
                                        1000.0,
                                    compact->
                                        schwarzschildRadiusMeters /
                                        1000.0,
                                    compact->
                                        photonSphereRadiusMeters /
                                        1000.0,
                                    compact->
                                        iscoRadiusMeters /
                                        1000.0));
                        }
                    };

                drawPhysicalDiagnostics(
                    "studio.primary",
                    "Primary View");

                drawPhysicalDiagnostics(
                    "studio.map",
                    "Body Map");
            }
    });

    static_cast<void>(SynchronizeProject());
}

bool StudioUiBundle::SynchronizeProject()
{
    if (workspace_ == nullptr ||
        device_ == nullptr)
    {
        return false;
    }

    const u64 generation =
        workspace_->Generation();

    if (generation == observedWorkspaceGeneration_)
    {
        return false;
    }

    // Registered panel callbacks outlive individual projects. Remove their
    // old pointers before destroying presentation objects that were bound to
    // the previous StudioSession.
    viewportPanels_.ClearBinding();
    views_.reset();
    runtime_.reset();

    observedWorkspaceGeneration_ = generation;

    if (!workspace_->HasProject())
    {
        return true;
    }

    auto& session = workspace_->Session();

    runtime_ =
        std::make_unique<studio_session::StudioRuntimeBinding>(
            session);
    views_ =
        std::make_unique<StudioRenderViewSet>(
            *device_,
            session);

    viewportPanels_.Rebind(
        *views_,
        session);
    return true;
}

std::optional<studio_session::StudioRuntimeSnapshot>
StudioUiBundle::Refresh()
{
    static_cast<void>(SynchronizeProject());

    if (runtime_ == nullptr ||
        views_ == nullptr)
    {
        return std::nullopt;
    }

    auto snapshot = runtime_->Refresh();
    static_cast<void>(
        views_->Refresh(snapshot));
    return snapshot;
}

std::vector<StudioRenderedView>
StudioUiBundle::Compose(
    render_graph::RenderGraph& graph,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug)
{
    static_cast<void>(SynchronizeProject());

    if (runtime_ == nullptr ||
        views_ == nullptr ||
        workspace_ == nullptr ||
        !workspace_->HasProject())
    {
        return {};
    }

    if (!runtime_->IsCurrent(snapshot))
    {
        throw std::logic_error(
            "Studio UI bundle received a stale runtime snapshot after a project/world change.");
    }

    return viewportRenderer_.Compose(
        graph,
        *views_,
        workspace_->Session(),
        *runtime_,
        snapshot,
        atTime,
        drawPathDebug);
}

StudioRenderViewSet* StudioUiBundle::Views() noexcept
{
    return views_.get();
}

const StudioRenderViewSet*
StudioUiBundle::Views() const noexcept
{
    return views_.get();
}

studio_session::StudioRuntimeBinding*
StudioUiBundle::Runtime() noexcept
{
    return runtime_.get();
}

const studio_session::StudioRuntimeBinding*
StudioUiBundle::Runtime() const noexcept
{
    return runtime_.get();
}

u64 StudioUiBundle::ObservedWorkspaceGeneration() const noexcept
{
    return observedWorkspaceGeneration_;
}
} // namespace orbit::studio_ui
