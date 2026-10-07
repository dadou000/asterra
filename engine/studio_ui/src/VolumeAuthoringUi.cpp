// VolumeAuthoringUi::RegisterBase / DrawBase are the M30-M35 Volumes workflow (presets,
// sources, effectors, render and solver controls). Register and Draw, further down, are the
// M36+ panel: they call the base methods and add the representation policy, cache and
// output controls, so there is still one panel and one authoring workflow.
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <array>
#include <exception>
#include <format>
#include <optional>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::optional<scene::ObjectId>
SelectedVolumeId(
    editor_session::EditorWorldSession& world)
{
    const auto& selected =
        world.Selection().Ordered();

    if (selected.size() != 1U)
    {
        return std::nullopt;
    }

    const auto record =
        world.Objects().Find(
            selected.front());

    if (!record.has_value())
    {
        return std::nullopt;
    }

    if (record->type ==
        world_model::kVolumeType)
    {
        return record->id;
    }

    if ((record->type ==
             world_model::kVolumeSourceType ||
         record->type ==
             world_model::kVolumeEffectorType) &&
        record->parent.has_value())
    {
        const auto parent =
            world.Objects().Find(
                *record->parent);

        if (parent.has_value() &&
            parent->type ==
                world_model::kVolumeType)
        {
            return parent->id;
        }
    }

    return std::nullopt;
}
} // namespace

VolumeAuthoringUi::VolumeAuthoringUi(
    studio_session::StudioSession& session,
    volume_fields::VolumeFieldStorageService& fields,
    volume_solver::SurfaceVolumeSolverService& solver,
    StudioViewportRenderer& renderer) noexcept
    : session_(&session),
      fields_(&fields),
      solver_(&solver),
      renderer_(&renderer)
{
}

void VolumeAuthoringUi::RegisterBase(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Volumes",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 35,
        .minSize = {
            .width = 320.0F,
            .height = 300.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawBase(context);
            }
    });
}

void VolumeAuthoringUi::DrawBase(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to author volumes.");
        return;
    }

    auto& world =
        session_->World();

    context.Heading("Create Volume");
    context.MutedText(
        "Presets create ordinary editable Volume domains.");

    constexpr std::array presets{
        "Empty",
        "Smoke",
        "Fire",
        "Fog",
        "Dust",
        "Snow",
        "Surface Flow"
    };

    for (std::size_t index = 0U;
         index < presets.size();
         ++index)
    {
        const std::string label =
            std::string(presets[index]) +
            "##volume-preset-" +
            std::to_string(index);

        if (context.Button(label))
        {
            try
            {
                commands::CommandArguments args;
                args.emplace(
                    "preset",
                    std::string(
                        presets[index]));

                world.CommandRegistry().Invoke(
                    editor_model::
                        authoring_commands::
                            kCreateVolume,
                    args);

                status_ =
                    std::string(presets[index]) +
                    " Volume created.";
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }

        if (index + 1U < presets.size())
        {
            context.SameLine();
        }
    }

    const auto volumeId =
        SelectedVolumeId(world);

    if (volumeId.has_value())
    {
        const auto volume =
            world_model::ResolveVolumeDomain(
                world.Objects(),
                *volumeId);

        if (volume.has_value())
        {
            context.Separator();
            context.Heading("Selected Volume");

            context.Text(
                std::format(
                    "{} | {} | fields 0x{:X}",
                    world_model::
                        VolumeSolverPolicyName(
                            volume->solverPolicy),
                    world_model::
                        VolumeRepresentationModeName(
                            volume->representationMode),
                    volume->fieldMask));

            if (fields_ != nullptr)
            {
                fields_->RemoveMissing(
                    world.Objects());

                auto& storage =
                    fields_->Ensure(
                        *volume);

                const u32 invalidated =
                    fields_->SyncAuthoredInputs(
                        world.Objects(),
                        *volumeId);

                const auto& diagnostics =
                    storage.Diagnostics();

                context.Text(
                    std::format(
                        "GPU fields {:.2f} MiB | cells {}x{}x{} | tiles {}x{}x{} @ {}^3",
                        static_cast<double>(
                            diagnostics.totalBytes) /
                            (1024.0 * 1024.0),
                        diagnostics.resolutionX,
                        diagnostics.resolutionY,
                        diagnostics.resolutionZ,
                        diagnostics.tilesX,
                        diagnostics.tilesY,
                        diagnostics.tilesZ,
                        diagnostics.tileEdge));

                context.Text(
                    std::format(
                        "Residency {} | valid {} | pending {} | source invalidation {} tile{}",
                        diagnostics.residentTiles,
                        diagnostics.validTiles,
                        diagnostics.pendingTiles,
                        invalidated,
                        invalidated == 1U ? "" : "s"));
            }

            context.Separator();
            context.Heading("Rendering / Lighting");

            bool renderEnabled =
                volume->renderEnabled;

            if (context.Checkbox(
                    "Render Enabled##volume-render-enabled",
                    renderEnabled))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeRenderEnabled,
                    renderEnabled);
            }

            f64 extinction =
                volume->extinctionScale;
            f64 albedo =
                volume->
                    singleScatteringAlbedo;
            f64 anisotropy =
                volume->anisotropy;
            f64 emissionScale =
                volume->emissionScale;
            f64 giEmissionScale =
                volume->giEmissionScale;
            f64 temporalWeight =
                volume->temporalWeight;
            i64 renderSteps =
                volume->renderSteps;
            i64 shadowSteps =
                volume->shadowSteps;

            auto scatteringColor =
                math::Double3{
                    volume->scatteringColor.x,
                    volume->scatteringColor.y,
                    volume->scatteringColor.z
                };
            auto emissionColor =
                math::Double3{
                    volume->emissionColor.x,
                    volume->emissionColor.y,
                    volume->emissionColor.z
                };

            if (context.InputDouble(
                    "Extinction Scale##volume-render-extinction",
                    extinction))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeExtinctionScale,
                    std::max(
                        extinction,
                        0.0));
            }

            if (context.InputDouble(
                    "Single Scattering Albedo##volume-render-albedo",
                    albedo))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeSingleScatteringAlbedo,
                    std::clamp(
                        albedo,
                        0.0,
                        1.0));
            }

            if (context.InputDouble3(
                    "Scattering Color##volume-render-scatter-color",
                    scatteringColor))
            {
                scatteringColor.x =
                    std::max(
                        scatteringColor.x,
                        0.0);
                scatteringColor.y =
                    std::max(
                        scatteringColor.y,
                        0.0);
                scatteringColor.z =
                    std::max(
                        scatteringColor.z,
                        0.0);

                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeScatteringColor,
                    scatteringColor);
            }

            if (context.InputDouble(
                    "Phase Anisotropy##volume-render-anisotropy",
                    anisotropy))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeAnisotropy,
                    std::clamp(
                        anisotropy,
                        -0.95,
                        0.95));
            }

            if (context.InputDouble3(
                    "Emission Color##volume-render-emission-color",
                    emissionColor))
            {
                emissionColor.x =
                    std::max(
                        emissionColor.x,
                        0.0);
                emissionColor.y =
                    std::max(
                        emissionColor.y,
                        0.0);
                emissionColor.z =
                    std::max(
                        emissionColor.z,
                        0.0);

                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeEmissionColor,
                    emissionColor);
            }

            if (context.InputDouble(
                    "Emission Scale##volume-render-emission-scale",
                    emissionScale))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeEmissionScale,
                    std::max(
                        emissionScale,
                        0.0));
            }

            if (context.InputDouble(
                    "GI Emission Scale##volume-render-gi-emission-scale",
                    giEmissionScale))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeGiEmissionScale,
                    std::max(
                        giEmissionScale,
                        0.0));
            }

            if (context.InputInteger(
                    "Raymarch Steps##volume-render-steps",
                    renderSteps))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeRenderSteps,
                    std::clamp<i64>(
                        renderSteps,
                        8,
                        256));
            }

            if (context.InputInteger(
                    "Shadow Steps##volume-render-shadow-steps",
                    shadowSteps))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeShadowSteps,
                    std::clamp<i64>(
                        shadowSteps,
                        0,
                        32));
            }

            if (context.InputDouble(
                    "Temporal Weight##volume-render-temporal-weight",
                    temporalWeight))
            {
                world.Commands().SetProperty(
                    *volumeId,
                    world_model::
                        kVolumeTemporalWeight,
                    std::clamp(
                        temporalWeight,
                        0.0,
                        0.98));
            }

            if (renderer_ != nullptr)
            {
                auto& renderSettings =
                    renderer_->
                        VolumeRenderSettings(
                            *volumeId);

                bool temporalEnabled =
                    renderSettings.
                        temporalEnabled;

                if (context.Checkbox(
                        "Temporal Stabilization##volume-render-temporal",
                        temporalEnabled))
                {
                    renderSettings.
                        temporalEnabled =
                            temporalEnabled;
                }

                context.Text(
                    "Render Debug");

                constexpr std::array<
                    std::pair<
                        const char*,
                        volume_render::
                            VolumeRenderDebugMode>,
                    5>
                    debugModes{{
                        {
                            "Composite",
                            volume_render::
                                VolumeRenderDebugMode::
                                    Composite
                        },
                        {
                            "Scattering",
                            volume_render::
                                VolumeRenderDebugMode::
                                    Scattering
                        },
                        {
                            "Extinction",
                            volume_render::
                                VolumeRenderDebugMode::
                                    Extinction
                        },
                        {
                            "Emission",
                            volume_render::
                                VolumeRenderDebugMode::
                                    Emission
                        },
                        {
                            "Shadow",
                            volume_render::
                                VolumeRenderDebugMode::
                                    Shadow
                        }
                    }};

                for (const auto&
                     [label, mode] :
                     debugModes)
                {
                    const std::string widget =
                        std::string(label) +
                        "##volume-render-debug-" +
                        std::to_string(
                            static_cast<u32>(
                                mode));

                    if (context.Selectable(
                            widget,
                            renderSettings.
                                    debugMode ==
                                mode))
                    {
                        renderSettings.debugMode =
                            mode;
                    }
                }

                const auto renderDiagnostics =
                    renderer_->
                        VolumeRenderDiagnostics(
                            *volumeId);

                context.MutedText(
                    std::format(
                        "Raymarch {} steps | shadows {} | local lights {} | resident tiles {}",
                        renderDiagnostics.
                            raymarchSteps,
                        renderDiagnostics.
                            shadowSteps,
                        renderDiagnostics.
                            localLightCount,
                        renderDiagnostics.
                            residentTiles));

                context.MutedText(
                    std::format(
                        "Temporal history {:.2f} MiB | scratch {:.2f} MiB | {}",
                        static_cast<double>(
                            renderDiagnostics.
                                historyBytes) /
                            (1024.0 * 1024.0),
                        static_cast<double>(
                            renderDiagnostics.
                                scratchBytes) /
                            (1024.0 * 1024.0),
                        renderDiagnostics.
                                historyValid
                            ? "history valid"
                            : "history warming"));
            }

            if (solver_ != nullptr &&
                (volume->solverPolicy ==
                     world_model::
                         VolumeSolverPolicy::
                             Surface2D5D ||
                 volume->solverPolicy ==
                     world_model::
                         VolumeSolverPolicy::
                             Local3D))
            {
                const bool local3D =
                    volume->solverPolicy ==
                        world_model::
                            VolumeSolverPolicy::
                                Local3D;

                context.Separator();
                context.Heading(
                    local3D
                        ? "Local 3D Live Solver"
                        : "Surface Live Solver");

                auto& settings =
                    solver_->Settings(
                        *volumeId);
                const auto diagnostics =
                    solver_->Diagnostics(
                        *volumeId);

                bool live =
                    settings.live;

                if (context.Checkbox(
                        "Live##volume-surface-live",
                        live))
                {
                    settings.live =
                        live;
                }

                bool followCamera =
                    settings.followCamera;

                if (context.Checkbox(
                        "Follow Camera##volume-surface-follow-camera",
                        followCamera))
                {
                    settings.followCamera =
                        followCamera;
                }

                bool paused =
                    settings.paused;

                if (context.Checkbox(
                        "Pause##volume-surface-pause",
                        paused))
                {
                    settings.paused =
                        paused;
                }

                if (context.Button(
                        "Step##volume-surface-step"))
                {
                    settings.live = true;
                    settings.paused = true;
                    settings.singleStepRequested =
                        true;
                }

                context.SameLine();

                if (context.Button(
                        "Reset##volume-surface-reset"))
                {
                    settings.resetRequested =
                        true;
                }

                f64 dt =
                    settings.timeStepSeconds;
                f64 scalarDissipation =
                    settings.
                        scalarDissipationPerSecond;
                f64 velocityDissipation =
                    settings.
                        velocityDissipationPerSecond;
                f64 sourceScale =
                    settings.sourceScale;
                i64 iterations =
                    settings.iterationsPerFrame;
                f64 gpuBudgetMilliseconds =
                    settings.gpuBudgetMilliseconds;

                bool settingsChanged =
                    context.InputDouble(
                        "Time Step s##volume-surface-dt",
                        dt);
                settingsChanged |=
                    context.InputDouble(
                        "Scalar Dissipation /s##volume-surface-scalar-diss",
                        scalarDissipation);
                settingsChanged |=
                    context.InputDouble(
                        "Velocity Dissipation /s##volume-surface-velocity-diss",
                        velocityDissipation);
                settingsChanged |=
                    context.InputDouble(
                        "Source Scale##volume-surface-source-scale",
                        sourceScale);
                settingsChanged |=
                    context.InputInteger(
                        "Iterations / Frame##volume-surface-iterations",
                        iterations);
                settingsChanged |=
                    context.InputDouble(
                        "GPU Budget ms##volume-solver-budget",
                        gpuBudgetMilliseconds);

                if (settingsChanged)
                {
                    settings.timeStepSeconds =
                        static_cast<f32>(
                            std::clamp(
                                dt,
                                0.001,
                                0.1));
                    settings.
                        scalarDissipationPerSecond =
                            static_cast<f32>(
                                std::max(
                                    scalarDissipation,
                                    0.0));
                    settings.
                        velocityDissipationPerSecond =
                            static_cast<f32>(
                                std::max(
                                    velocityDissipation,
                                    0.0));
                    settings.sourceScale =
                        static_cast<f32>(
                            std::max(
                                sourceScale,
                                0.0));
                    settings.iterationsPerFrame =
                        static_cast<u32>(
                            std::clamp<i64>(
                                iterations,
                                1,
                                16));
                    settings.gpuBudgetMilliseconds =
                        static_cast<f32>(
                            std::clamp(
                                gpuBudgetMilliseconds,
                                0.05,
                                20.0));
                }

                i64 resolution =
                    volume->resolution;
                i64 surfaceLayers =
                    volume->surfaceLayers;

                bool layoutChanged =
                    context.InputInteger(
                        local3D
                            ? "3D Resolution##volume-local3d-resolution"
                            : "Surface Resolution##volume-surface-resolution",
                        resolution);

                if (!local3D)
                {
                    layoutChanged |=
                        context.InputInteger(
                            "Surface Layers##volume-surface-layers",
                            surfaceLayers);
                }

                if (layoutChanged)
                {
                    world.Commands().SetProperty(
                        *volumeId,
                        world_model::
                            kVolumeResolution,
                        std::clamp<i64>(
                            resolution,
                            8,
                            1024));

                    if (!local3D)
                    {
                        world.Commands().SetProperty(
                            *volumeId,
                            world_model::
                                kVolumeSurfaceLayers,
                            std::clamp<i64>(
                                surfaceLayers,
                                1,
                                32));
                    }

                    settings.resetRequested =
                        true;
                }

                if (local3D)
                {
                    auto center =
                        volume->centerMeters;
                    auto halfExtents =
                        volume->halfExtentsMeters;

                    bool boundsChanged =
                        context.InputDouble3(
                            "Domain Center##volume-local3d-center",
                            center);
                    boundsChanged |=
                        context.InputDouble3(
                            "Half Extents##volume-local3d-half-extents",
                            halfExtents);

                    if (boundsChanged)
                    {
                        halfExtents.x =
                            std::max(
                                std::abs(
                                    halfExtents.x),
                                0.01);
                        halfExtents.y =
                            std::max(
                                std::abs(
                                    halfExtents.y),
                                0.01);
                        halfExtents.z =
                            std::max(
                                std::abs(
                                    halfExtents.z),
                                0.01);

                        world.Commands().SetProperty(
                            *volumeId,
                            world_model::
                                kVolumeCenterMeters,
                            center);
                        world.Commands().SetProperty(
                            *volumeId,
                            world_model::
                                kVolumeHalfExtentsMeters,
                            halfExtents);

                        settings.resetRequested =
                            true;
                    }
                }

                context.Text(
                    std::format(
                        "GPU live: {} | {} | iterations {}/{} | simulated {:.3f}s",
                        diagnostics.eligible
                            ? "eligible"
                            : "inactive",
                        diagnostics.paused
                            ? "paused"
                            : "running",
                        diagnostics.iterationsThisFrame,
                        diagnostics.requestedIterations,
                        diagnostics.simulatedSeconds));

                if (local3D)
                {
                    context.Text(
                        diagnostics.gpuTimingValid
                            ? std::format(
                                  "Local3D GPU {:.3f} ms / {:.3f} ms budget",
                                  diagnostics.gpuMilliseconds,
                                  diagnostics.gpuBudgetMilliseconds)
                            : std::format(
                                  "Local3D GPU timing pending / {:.3f} ms budget",
                                  diagnostics.gpuBudgetMilliseconds));
                }

                context.MutedText(
                    std::format(
                        "Scratch {:.2f} MiB | metadata {:.1f} KiB | scalar channels {}",
                        static_cast<double>(
                            diagnostics.scratchBytes) /
                            (1024.0 * 1024.0),
                        static_cast<double>(
                            diagnostics.metadataBytes) /
                            1024.0,
                        diagnostics.
                            scalarChannelsSolved));

                context.Text("Field Debug View");

                if (local3D)
                {
                    if (context.Selectable(
                            "Density##volume-local3d-field-density",
                            settings.debugField ==
                                world_model::
                                    VolumeField::Density))
                    {
                        settings.debugField =
                            world_model::
                                VolumeField::Density;
                    }
                    if (context.Selectable(
                            "Velocity##volume-local3d-field-velocity",
                            settings.debugField ==
                                world_model::
                                    VolumeField::Velocity))
                    {
                        settings.debugField =
                            world_model::
                                VolumeField::Velocity;
                    }
                    if (context.Selectable(
                            "Temperature##volume-local3d-field-temperature",
                            settings.debugField ==
                                world_model::
                                    VolumeField::Temperature))
                    {
                        settings.debugField =
                            world_model::
                                VolumeField::Temperature;
                    }

                    if (context.Selectable(
                            "Slice X##volume-local3d-slice-x",
                            settings.sliceAxis ==
                                volume_solver::
                                    VolumeSliceAxis::X))
                    {
                        settings.sliceAxis =
                            volume_solver::
                                VolumeSliceAxis::X;
                    }
                    if (context.Selectable(
                            "Slice Y##volume-local3d-slice-y",
                            settings.sliceAxis ==
                                volume_solver::
                                    VolumeSliceAxis::Y))
                    {
                        settings.sliceAxis =
                            volume_solver::
                                VolumeSliceAxis::Y;
                    }
                    if (context.Selectable(
                            "Slice Z##volume-local3d-slice-z",
                            settings.sliceAxis ==
                                volume_solver::
                                    VolumeSliceAxis::Z))
                    {
                        settings.sliceAxis =
                            volume_solver::
                                VolumeSliceAxis::Z;
                    }
                }

                if (local3D &&
                    context.Selectable(
                        "Show Selected Field Slice##volume-local3d-show-slice",
                        settings.debugView ==
                            volume_solver::
                                SurfaceVolumeDebugView::
                                    FieldSlice))
                {
                    settings.debugView =
                        volume_solver::
                            SurfaceVolumeDebugView::
                                FieldSlice;
                }

                if (context.Selectable(
                        "Off##volume-debug-off",
                        settings.debugView ==
                            volume_solver::
                                SurfaceVolumeDebugView::
                                    Off))
                {
                    settings.debugView =
                        volume_solver::
                            SurfaceVolumeDebugView::
                                Off;
                }

                if (!local3D)
                {
                    if (context.Selectable(
                            "Density Slice##volume-debug-density",
                            settings.debugView ==
                                volume_solver::
                                    SurfaceVolumeDebugView::
                                        Density))
                    {
                        settings.debugView =
                            volume_solver::
                                SurfaceVolumeDebugView::
                                    Density;
                    }

                    if (context.Selectable(
                            "Velocity Overlay##volume-debug-velocity",
                            settings.debugView ==
                                volume_solver::
                                    SurfaceVolumeDebugView::
                                        Velocity))
                    {
                        settings.debugView =
                            volume_solver::
                                SurfaceVolumeDebugView::
                                    Velocity;
                    }
                }

                i64 debugLayer =
                    settings.debugLayer;

                if (context.InputInteger(
                        local3D
                            ? "Slice Index##volume-local3d-slice-index"
                            : "Debug Layer##volume-debug-layer",
                        debugLayer))
                {
                    const i64 maximumSlice =
                        local3D
                            ? static_cast<i64>(
                                  volume->resolution) -
                                  1
                            : static_cast<i64>(
                                  volume->
                                      surfaceLayers) -
                                  1;

                    settings.debugLayer =
                        static_cast<u32>(
                            std::clamp<i64>(
                                debugLayer,
                                0,
                                std::max<i64>(
                                    maximumSlice,
                                    0)));
                }
            }

            context.Separator();
            context.Heading("Sources");

            constexpr std::array sourceKinds{
                "Brush",
                "Texture / Mask",
                "Terrain Paint",
                "Spline",
                "Mesh / SDF",
                "Collision Proxy",
                "Particles",
                "Object Motion",
                "World Motion"
            };

            for (std::size_t index = 0U;
                 index < sourceKinds.size();
                 ++index)
            {
                const std::string label =
                    "+ " +
                    std::string(
                        sourceKinds[index]) +
                    "##volume-source-add-" +
                    std::to_string(index);

                if (context.Button(label))
                {
                    try
                    {
                        commands::CommandArguments args;
                        args.emplace(
                            "kind",
                            std::string(
                                sourceKinds[index]));

                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kAddVolumeSource,
                            args);

                        status_ =
                            std::string(
                                sourceKinds[index]) +
                            " source added.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ =
                            exception.what();
                    }
                }

                if (index + 1U < sourceKinds.size())
                {
                    context.SameLine();
                }
            }

            context.Heading("Effectors");

            constexpr std::array effectorKinds{
                "Obstacle",
                "Drag",
                "Wind",
                "Temperature",
                "Dissipation"
            };

            for (std::size_t index = 0U;
                 index < effectorKinds.size();
                 ++index)
            {
                const std::string label =
                    "+ " +
                    std::string(
                        effectorKinds[index]) +
                    "##volume-effector-add-" +
                    std::to_string(index);

                if (context.Button(label))
                {
                    try
                    {
                        commands::CommandArguments args;
                        args.emplace(
                            "kind",
                            std::string(
                                effectorKinds[index]));

                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kAddVolumeEffector,
                            args);

                        status_ =
                            std::string(
                                effectorKinds[index]) +
                            " effector added.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ =
                            exception.what();
                    }
                }

                if (index + 1U < effectorKinds.size())
                {
                    context.SameLine();
                }
            }

            const auto inputs =
                world_model::ResolveVolumeInputs(
                    world.Objects(),
                    *volumeId);

            const auto& selection =
                world.Selection().Ordered();
            const auto selectedInput =
                selection.size() == 1U
                    ? std::optional<scene::ObjectId>(
                          selection.front())
                    : std::nullopt;

            context.Separator();
            context.Heading("Evaluation Order");

            for (const auto& input :
                 inputs)
            {
                const std::string kindName =
                    input.role ==
                            world_model::
                                VolumeInputRole::Source
                        ? std::string(
                              world_model::
                                  VolumeSourceKindName(
                                      static_cast<
                                          world_model::
                                              VolumeSourceKind>(
                                          input.kind)))
                        : std::string(
                              world_model::
                                  VolumeEffectorKindName(
                                      static_cast<
                                          world_model::
                                              VolumeEffectorKind>(
                                          input.kind)));

                const std::string label =
                    std::format(
                        "{:02} {} | {} | {}##volume-input-{}",
                        input.order,
                        input.role ==
                                world_model::
                                    VolumeInputRole::Source
                            ? "Source"
                            : "Effector",
                        kindName,
                        world_model::
                            VolumeSourceShapeName(
                                input.shape),
                        input.object.ToString());

                if (context.Selectable(
                        label,
                        selectedInput.has_value() &&
                            *selectedInput ==
                                input.object))
                {
                    const scene::ObjectId selected[]{
                        input.object};
                    world.Selection().Set(
                        selected);
                }
            }

            if (selectedInput.has_value())
            {
                const auto selectedRecord =
                    world.Objects().Find(
                        *selectedInput);

                if (selectedRecord.has_value() &&
                    (selectedRecord->type ==
                         world_model::
                             kVolumeSourceType ||
                     selectedRecord->type ==
                         world_model::
                             kVolumeEffectorType))
                {
                    if (context.Button(
                            "Up##volume-input-up"))
                    {
                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kMoveVolumeInputUp);
                    }

                    context.SameLine();

                    if (context.Button(
                            "Down##volume-input-down"))
                    {
                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kMoveVolumeInputDown);
                    }

                    context.SameLine();

                    if (context.Button(
                            "Remove##volume-input-remove"))
                    {
                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kRemoveVolumeInput);
                    }
                }
            }

            context.Separator();
            context.Heading("Terrain Source Painting");
            context.MutedText(
                "Each stroke is an authored TerrainPatch source with a stable object ID. Viewport tools/MCP may invoke the same command with picked world coordinates.");

            static_cast<void>(
                context.InputDouble3(
                    "World Position##volume-paint-position",
                    paintPosition_));
            static_cast<void>(
                context.InputDouble(
                    "Radius m##volume-paint-radius",
                    paintRadius_));
            static_cast<void>(
                context.InputDouble(
                    "Strength##volume-paint-strength",
                    paintStrength_));

            if (context.PrimaryButton(
                    "Paint Terrain Source Stroke##volume-paint-stroke"))
            {
                try
                {
                    commands::CommandArguments args;
                    args.emplace(
                        "position",
                        paintPosition_);
                    args.emplace(
                        "radius",
                        paintRadius_);
                    args.emplace(
                        "strength",
                        paintStrength_);

                    world.CommandRegistry().Invoke(
                        editor_model::
                            authoring_commands::
                                kPaintVolumeTerrainSource,
                        args);

                    status_ =
                        "Terrain source stroke authored.";
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }

            if (renderer_ != nullptr)
            {
                bool debug =
                    renderer_->
                        VolumeSourceDebugVisualization();

                if (context.Checkbox(
                        "Show All Source / Effector Gizmos##volume-source-debug",
                        debug))
                {
                    renderer_->
                        SetVolumeSourceDebugVisualization(
                            debug);
                }
            }

            context.MutedText(
                "Exact source geometry, field mask, asset/path, target object and values remain editable in the normal Properties Inspector.");

            const auto selectedRecord =
                world.Selection().Ordered().size() == 1U
                    ? world.Objects().Find(
                          world.Selection().Ordered().front())
                    : std::optional<scene::ObjectRecord>{};

            if (selectedRecord.has_value() &&
                selectedRecord->type ==
                    world_model::kVolumeType &&
                context.Button(
                    "Remove Selected Volume##volume-remove"))
            {
                try
                {
                    world.CommandRegistry().Invoke(
                        editor_model::
                            authoring_commands::
                                kRemoveVolume);
                    status_ =
                        "Volume removed.";
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui

#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeOutputCoupling.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::studio_ui
{
void VolumeAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Volumes",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 35,
        .minSize = {
            .width = 320.0F,
            .height = 300.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void VolumeAuthoringUi::Draw(
    editor_ui::PanelContext& context)
{
    DrawBase(context);
    DrawRepresentationPolicy(context);
}

void VolumeAuthoringUi::DrawRepresentationPolicy(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        renderer_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto& world =
        session_->World();
    const auto volumeId =
        SelectedVolumeId(world);

    if (!volumeId.has_value())
    {
        return;
    }

    const auto volume =
        world_model::ResolveVolumeDomain(
            world.Objects(),
            *volumeId);

    if (!volume.has_value())
    {
        return;
    }

    auto& settings =
        renderer_->VolumeRenderSettings(
            *volumeId);
    const auto diagnostics =
        renderer_->VolumeRenderDiagnostics(
            *volumeId);

    context.Separator();
    context.Heading("Representation / LOD");
    context.MutedText(
        "Auto promotes near/important effects to Live, reduces medium effects to Coarse, and uses bounded passive representation at distance.");

    context.Text("Force Representation");

    constexpr std::array modes{
        std::pair{
            "Auto",
            world_model::VolumeRepresentationMode::Auto},
        std::pair{
            "Live",
            world_model::VolumeRepresentationMode::Live},
        std::pair{
            "Coarse",
            world_model::VolumeRepresentationMode::Coarse},
        std::pair{
            "Passive",
            world_model::VolumeRepresentationMode::Passive},
        std::pair{
            "Baked",
            world_model::VolumeRepresentationMode::Baked}
    };

    for (const auto& [label, mode] : modes)
    {
        const std::string widget =
            std::string(label) +
            "##volume-representation-force-" +
            std::to_string(
                static_cast<i64>(mode));

        if (context.Selectable(
                widget,
                volume->representationMode == mode))
        {
            world.Commands().SetProperty(
                *volumeId,
                world_model::
                    kVolumeRepresentationMode,
                static_cast<i64>(mode));
        }
    }

    context.Text("Follow Target");

    constexpr std::array followModes{
        std::pair{
            "Authored Domain",
            volume_representation::
                FollowTarget::AuthoredDomain},
        std::pair{
            "Camera",
            volume_representation::
                FollowTarget::Camera},
        std::pair{
            "Object / Important Actor",
            volume_representation::
                FollowTarget::Object}
    };

    for (const auto& [label, mode] : followModes)
    {
        const std::string widget =
            std::string(label) +
            "##volume-representation-follow-" +
            std::to_string(
                static_cast<u32>(mode));

        if (context.Selectable(
                widget,
                settings.followTarget == mode))
        {
            settings.followTarget = mode;
        }
    }

    if (settings.followTarget ==
        volume_representation::FollowTarget::Object)
    {
        std::string objectText =
            settings.followObject.has_value()
                ? settings.followObject->ToString()
                : std::string{};

        if (context.InputText(
                "Follow Object ID##volume-representation-follow-object",
                objectText))
        {
            if (objectText.empty())
            {
                settings.followObject.reset();
                settings.
                    followObjectPositionInFrameMeters.
                    reset();
            }
            else if (const auto parsed =
                         scene::ObjectId::Parse(
                             objectText);
                     parsed.has_value())
            {
                settings.followObject =
                    *parsed;
                status_ =
                    "M36 follow object selected. Runtime actor adapter must publish its frame-space position.";
            }
            else
            {
                status_ =
                    "Follow Object ID is not a valid Orbit object ID.";
            }
        }

        bool previewResolvedPosition =
            settings.
                followObjectPositionInFrameMeters.
                has_value();

        if (context.Checkbox(
                "Preview Resolved Actor Position##volume-representation-preview-actor",
                previewResolvedPosition))
        {
            if (previewResolvedPosition)
            {
                settings.
                    followObjectPositionInFrameMeters =
                        diagnostics.
                                followTargetResolved
                            ? diagnostics.
                                  runtimeCenterInFrameMeters
                            : volume->centerMeters;
            }
            else
            {
                settings.
                    followObjectPositionInFrameMeters.
                    reset();
            }
        }

        if (settings.
                followObjectPositionInFrameMeters.
                has_value())
        {
            auto previewPosition =
                *settings.
                    followObjectPositionInFrameMeters;

            if (context.InputDouble3(
                    "Resolved Actor Position##volume-representation-actor-position",
                    previewPosition))
            {
                settings.
                    followObjectPositionInFrameMeters =
                        previewPosition;
            }

            context.MutedText(
                "Preview seam only. Game/runtime actor adapters publish this position automatically; it is not written into the authored Volume center.");
        }
        else
        {
            context.MutedText(
                "Object target unresolved: authored domain center remains active until a runtime actor adapter publishes a frame-space position.");
        }
    }

    f64 liveDistance =
        settings.liveDistanceMeters;
    f64 passiveDistance =
        settings.passiveDistanceMeters;
    f64 livePixels =
        settings.liveProjectedPixels;
    f64 passivePixels =
        settings.passiveProjectedPixels;
    f64 hysteresisPercent =
        static_cast<f64>(
            settings.hysteresisFraction) *
        100.0;
    i64 coarseResolution =
        settings.coarseResolution;
    i64 passiveResolution =
        settings.passiveResolution;
    i64 coarseSteps =
        settings.coarseRaymarchSteps;
    i64 passiveSteps =
        settings.passiveRaymarchSteps;

    bool policyChanged =
        context.InputDouble(
            "Live Radius m##volume-lod-live-distance",
            liveDistance);
    policyChanged |=
        context.InputDouble(
            "Passive Radius m##volume-lod-passive-distance",
            passiveDistance);
    policyChanged |=
        context.InputDouble(
            "Live Projected px##volume-lod-live-pixels",
            livePixels);
    policyChanged |=
        context.InputDouble(
            "Passive Projected px##volume-lod-passive-pixels",
            passivePixels);
    policyChanged |=
        context.InputDouble(
            "Hysteresis %##volume-lod-hysteresis",
            hysteresisPercent);
    policyChanged |=
        context.InputInteger(
            "Coarse Resolution##volume-lod-coarse-resolution",
            coarseResolution);
    policyChanged |=
        context.InputInteger(
            "Passive Resolution##volume-lod-passive-resolution",
            passiveResolution);
    policyChanged |=
        context.InputInteger(
            "Coarse Raymarch Steps##volume-lod-coarse-steps",
            coarseSteps);
    policyChanged |=
        context.InputInteger(
            "Passive Raymarch Steps##volume-lod-passive-steps",
            passiveSteps);

    if (policyChanged)
    {
        settings.liveDistanceMeters =
            std::max(
                liveDistance,
                0.01);
        settings.passiveDistanceMeters =
            std::max(
                passiveDistance,
                settings.liveDistanceMeters +
                    0.01);
        settings.liveProjectedPixels =
            static_cast<f32>(
                std::max(
                    livePixels,
                    1.0));
        settings.passiveProjectedPixels =
            static_cast<f32>(
                std::clamp(
                    passivePixels,
                    0.1,
                    static_cast<f64>(
                        settings.
                            liveProjectedPixels)));
        settings.hysteresisFraction =
            static_cast<f32>(
                std::clamp(
                    hysteresisPercent /
                        100.0,
                    0.0,
                    0.45));
        settings.coarseResolution =
            static_cast<u32>(
                std::clamp<i64>(
                    coarseResolution,
                    8,
                    128));
        settings.passiveResolution =
            static_cast<u32>(
                std::clamp<i64>(
                    passiveResolution,
                    8,
                    64));
        settings.coarseRaymarchSteps =
            static_cast<u32>(
                std::clamp<i64>(
                    coarseSteps,
                    8,
                    96));
        settings.passiveRaymarchSteps =
            static_cast<u32>(
                std::clamp<i64>(
                    passiveSteps,
                    4,
                    32));
    }

    bool showRegions =
        settings.showRepresentationRegions;

    if (context.Checkbox(
            "Visualize Representation Regions##volume-lod-regions",
            showRegions))
    {
        settings.showRepresentationRegions =
            showRegions;
    }

    context.Text(
        std::format(
            "Resolved {}{} | previous {}{}",
            volume_representation::
                ResolvedRepresentationName(
                    diagnostics.representation),
            diagnostics.representationForced
                ? " (forced)"
                : "",
            volume_representation::
                ResolvedRepresentationName(
                    diagnostics.previousRepresentation),
            diagnostics.representationTransition
                ? " | transition active"
                : ""));

    context.Text(
        std::format(
            "Blend Live {:.3f} | Coarse {:.3f} | Passive {:.3f} | Baked {:.3f}",
            diagnostics.liveWeight,
            diagnostics.coarseWeight,
            diagnostics.passiveWeight,
            diagnostics.bakedWeight));

    context.Text(
        std::format(
            "Bounds distance {:.1f} m | projected {:.1f} px | {} field",
            diagnostics.distanceToBoundsMeters,
            diagnostics.projectedDiameterPixels,
            diagnostics.denseFieldRequired
                ? "full live"
                : "bounded aggregate"));

    context.MutedText(
        std::format(
            "Runtime center [{:.2f}, {:.2f}, {:.2f}] | stable 0x{:016X}",
            diagnostics.runtimeCenterInFrameMeters.x,
            diagnostics.runtimeCenterInFrameMeters.y,
            diagnostics.runtimeCenterInFrameMeters.z,
            diagnostics.stableAddressFingerprint));

    if (!diagnostics.followTargetResolved)
    {
        context.MutedText(
            "Follow target is unresolved; runtime safely retains the authored center.");
    }

    if (diagnostics.bakedFallback)
    {
        context.MutedText(
            "Baked was forced but no validated M37 cache is attached; Passive is used explicitly as the bounded fallback.");
    }

    // M37 native cache pipeline. A bake is immediately attached for preview;
    // export is optional. Import validates schema, dimensions and checksum
    // before the renderer can see the cache.
    context.Separator();
    context.Heading("Volume Cache / Bake");
    context.MutedText(
        "Bake authored static density/emission into a versioned .orbitvol asset, or import an existing validated cache. Baked representation resamples it into the current runtime residency tier.");

    i64 bakeResolution =
        static_cast<i64>(cacheBakeResolution_);
    if (context.InputInteger(
            "Bake Resolution##volume-cache-resolution",
            bakeResolution))
    {
        cacheBakeResolution_ =
            static_cast<u32>(
                std::clamp<i64>(
                    bakeResolution,
                    4,
                    512));
    }

    static_cast<void>(context.InputText(
        "Cache Path##volume-cache-path",
        cachePath_));

    const auto inputs =
        world_model::ResolveVolumeInputs(
            world.Objects(),
            *volumeId);

    const u64 cacheFields =
        volume->fieldMask &
        (static_cast<u64>(
             world_model::VolumeField::Density) |
         static_cast<u64>(
             world_model::VolumeField::Emission));

    const volume_representation::
        VolumeCacheBakeSettings bakeSettings{
            .resolution = cacheBakeResolution_,
            .fieldMask =
                cacheFields != 0U
                    ? cacheFields
                    : static_cast<u64>(
                          world_model::
                              VolumeField::Density)
        };

    if (context.Button(
            "Bake & Attach##volume-cache-bake"))
    {
        auto cache =
            volume_representation::BakeVolumeCache(
                *volume,
                inputs,
                bakeSettings);

        const auto bytes =
            cache.ByteSize();
        const auto fingerprint =
            cache.payloadFingerprint;

        volume_representation::
            VolumeCaches().Attach(
                *volumeId,
                std::move(cache));

        status_ =
            std::format(
                "M37 cache baked: {}^3, {:.2f} MiB, payload 0x{:016X}.",
                cacheBakeResolution_,
                static_cast<double>(bytes) /
                    (1024.0 * 1024.0),
                fingerprint);

        if (!cachePath_.empty())
        {
            const auto* attached =
                volume_representation::
                    VolumeCaches().Find(
                        *volumeId);
            std::string error;
            if (attached != nullptr &&
                !volume_representation::
                    SaveVolumeCache(
                        cachePath_,
                        *attached,
                        &error))
            {
                status_ +=
                    " Export failed: " + error;
            }
            else
            {
                status_ +=
                    " Exported to " + cachePath_ + ".";
            }
        }
    }

    context.SameLine();
    if (context.Button(
            "Import##volume-cache-import"))
    {
        if (cachePath_.empty())
        {
            status_ =
                "Set a .orbitvol path before importing.";
        }
        else
        {
            auto loaded =
                volume_representation::
                    LoadVolumeCache(
                        cachePath_);
            if (loaded)
            {
                volume_representation::
                    VolumeCaches().Attach(
                        *volumeId,
                        std::move(*loaded.cache));
                status_ =
                    "M37 cache imported, checksum validated and attached.";
            }
            else
            {
                status_ =
                    std::format(
                        "Import failed [{}]: {}",
                        volume_representation::
                            VolumeCacheLoadStatusName(
                                loaded.status),
                        loaded.message);
            }
        }
    }

    context.SameLine();
    if (context.Button(
            "Export##volume-cache-export"))
    {
        const auto* cache =
            volume_representation::
                VolumeCaches().Find(
                    *volumeId);
        if (cache == nullptr)
        {
            status_ =
                "No cache is attached to this Volume.";
        }
        else if (cachePath_.empty())
        {
            status_ =
                "Set a .orbitvol path before exporting.";
        }
        else
        {
            std::string error;
            if (volume_representation::
                    SaveVolumeCache(
                        cachePath_,
                        *cache,
                        &error))
            {
                status_ =
                    "M37 cache exported to " +
                    cachePath_ + ".";
            }
            else
            {
                status_ =
                    "Cache export failed: " +
                    error;
            }
        }
    }

    context.SameLine();
    if (context.Button(
            "Detach##volume-cache-detach"))
    {
        volume_representation::
            VolumeCaches().Detach(
                *volumeId);
        status_ =
            "M37 cache detached. Forced Baked will explicitly fall back to Passive.";
    }

    if (const auto* cache =
            volume_representation::
                VolumeCaches().Find(
                    *volumeId);
        cache != nullptr)
    {
        std::string freshness;
        const volume_representation::VolumeCacheBakeSettings
            attachedSettings{
                .resolution =
                    cache->descriptor.resolutionX,
                .fieldMask =
                    cache->descriptor.fieldMask
            };
        const bool current =
            volume_representation::
                IsVolumeCacheCurrent(
                    *cache,
                    *volume,
                    inputs,
                    attachedSettings,
                    &freshness);

        context.Text(
            std::format(
                "Attached {}x{}x{} | {:.2f} MiB | payload 0x{:016X}",
                cache->descriptor.resolutionX,
                cache->descriptor.resolutionY,
                cache->descriptor.resolutionZ,
                static_cast<double>(
                    cache->ByteSize()) /
                    (1024.0 * 1024.0),
                cache->payloadFingerprint));
        context.MutedText(
            current
                ? "Cache matches current authored inputs and bake settings."
                : freshness);
        if (!cache->sourcePath.empty())
        {
            context.MutedText(
                "Imported from: " +
                cache->sourcePath);
        }
    }
    else
    {
        context.MutedText(
            "No cache attached. Baked remains unavailable and cannot silently masquerade as live data.");
    }

    context.Separator();
    context.Heading("Particle / Surface Output");
    context.MutedText(
        "M38 samples the authoritative volume field on simulation time and publishes bounded particle and physical-surface requests. The current CPU-readable producer is the validated M37 cache; live GPU fields will use the same output contract through compact GPU production.");

    bool particlesEnabled = volume->outputParticlesEnabled;
    if (context.Checkbox("Particles##volume-output-particles", particlesEnabled))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputParticlesEnabled, particlesEnabled);

    bool depositsEnabled = volume->outputSurfaceDepositsEnabled;
    if (context.Checkbox("Surface Deposits##volume-output-deposits", depositsEnabled))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceDepositsEnabled, depositsEnabled);

    f64 threshold = volume->outputFieldThreshold;
    if (context.InputDouble("Field Threshold##volume-output-threshold", threshold))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputFieldThreshold, std::clamp(threshold, 0.0, 64.0));

    f64 particleRate = volume->outputParticleRatePerSecond;
    i64 particleBudget = static_cast<i64>(volume->outputParticleBudgetPerStep);
    if (context.InputDouble("Particle Rate / s##volume-output-particle-rate", particleRate))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputParticleRate, std::clamp(particleRate, 0.0, 1000000.0));
    if (context.InputInteger("Particle Budget / Step##volume-output-particle-budget", particleBudget))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputParticleBudget, std::clamp<i64>(particleBudget, 1, 1000000));

    f64 lifetime = volume->particleLifetimeSeconds;
    f64 drag = volume->particleLinearDragPerSecond;
    f64 particleRadius = volume->particleRadiusMeters;
    f64 particleEmissionScale = volume->particleEmissionScale;
    if (context.InputDouble("Particle Lifetime s##volume-output-particle-life", lifetime))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleLifetime, std::clamp(lifetime, 0.001, 3600.0));
    if (context.InputDouble("Particle Linear Drag / s##volume-output-particle-drag", drag))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleLinearDrag, std::clamp(drag, 0.0, 100.0));
    if (context.InputDouble("Particle Radius m##volume-output-particle-radius", particleRadius))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleRadiusMeters, std::clamp(particleRadius, 0.001, 100.0));
    if (context.InputDouble("Particle Emission Scale##volume-output-particle-emission", particleEmissionScale))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleEmissionScale, std::clamp(particleEmissionScale, 0.0, 1024.0));

    context.Text("Particle Gravity");
    for (const auto& [label, mode] : std::array{
             std::pair{"None", world_model::VolumeParticleGravityMode::None},
             std::pair{"Owning Body", world_model::VolumeParticleGravityMode::OwningBody}})
        if (context.Selectable(std::string(label) + "##volume-output-gravity-" + std::to_string(static_cast<i64>(mode)), volume->particleGravityMode == mode))
            world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleGravityMode, static_cast<i64>(mode));

    f64 gravityScale = volume->particleGravityScale;
    if (context.InputDouble("Gravity Scale##volume-output-gravity-scale", gravityScale))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleGravityScale, std::clamp(gravityScale, 0.0, 16.0));

    context.Text("Particle Collision");
    for (const auto& [label, mode] : std::array{
             std::pair{"None", world_model::VolumeParticleCollisionMode::None},
             std::pair{"Kill", world_model::VolumeParticleCollisionMode::Kill},
             std::pair{"Slide", world_model::VolumeParticleCollisionMode::Slide},
             std::pair{"Bounce", world_model::VolumeParticleCollisionMode::Bounce}})
        if (context.Selectable(std::string(label) + "##volume-output-collision-" + std::to_string(static_cast<i64>(mode)), volume->particleCollisionMode == mode))
            world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleCollisionMode, static_cast<i64>(mode));

    f64 restitution = volume->particleRestitution;
    if (context.InputDouble("Restitution##volume-output-restitution", restitution))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleRestitution, std::clamp(restitution, 0.0, 1.0));

    context.Text("Standing Water Response");
    f64 waterDensityRatio = volume->particleWaterDensityRatio;
    f64 waterDrag = volume->particleWaterDragPerSecond;
    f64 waterBuoyancy = volume->particleWaterBuoyancyScale;
    f64 waterSplashScale = volume->particleWaterSplashScale;
    bool killOnWater = volume->particleKillOnWaterImmersion;
    bool splashOnWaterEntry = volume->particleSplashOnWaterEntry;
    if (context.InputDouble("Density Ratio to Water##volume-output-water-density", waterDensityRatio))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterDensityRatio, std::clamp(waterDensityRatio, 0.01, 100.0));
    if (context.InputDouble("Water Drag / s##volume-output-water-drag", waterDrag))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterDrag, std::clamp(waterDrag, 0.0, 1000.0));
    if (context.InputDouble("Buoyancy Scale##volume-output-water-buoyancy", waterBuoyancy))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterBuoyancyScale, std::clamp(waterBuoyancy, 0.0, 16.0));
    if (context.Checkbox("Kill On Water Immersion##volume-output-water-kill", killOnWater))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleKillOnWaterImmersion, killOnWater);
    if (context.Checkbox("Splash On Water Entry##volume-output-water-splash", splashOnWaterEntry))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleSplashOnWaterEntry, splashOnWaterEntry);
    if (context.InputDouble("Splash Scale##volume-output-water-splash-scale", waterSplashScale))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterSplashScale, std::clamp(waterSplashScale, 0.0, 64.0));
    context.MutedText("Density ratio 1.0 is neutrally buoyant at full immersion. Water entry is latched on GPU for the splash-output pass; no particle-state CPU readback is used.");

    f64 depositRate = volume->outputSurfaceDepositRatePerSecond;
    i64 depositBudget = static_cast<i64>(volume->outputSurfaceDepositBudgetPerStep);
    f64 depositRadius = volume->outputSurfaceDepositRadiusMeters;
    if (context.InputDouble("Deposit Rate / s##volume-output-deposit-rate", depositRate))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceRate, std::clamp(depositRate, 0.0, 1000000.0));
    if (context.InputInteger("Deposit Budget / Step##volume-output-deposit-budget", depositBudget))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceBudget, std::clamp<i64>(depositBudget, 1, 1000000));
    if (context.InputDouble("Deposit Radius m##volume-output-deposit-radius", depositRadius))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceRadius, std::clamp(depositRadius, 0.001, 10000.0));

    context.Text("Surface Effect");
    for (const auto& [label, effect] : std::array{
             std::pair{"Wetness", world_model::VolumeSurfaceOutputEffect::Wetness},
             std::pair{"Soot", world_model::VolumeSurfaceOutputEffect::Soot},
             std::pair{"Ash", world_model::VolumeSurfaceOutputEffect::Ash},
             std::pair{"Sediment", world_model::VolumeSurfaceOutputEffect::Sediment},
             std::pair{"Heat", world_model::VolumeSurfaceOutputEffect::Heat}})
        if (context.Selectable(std::string(label) + "##volume-output-effect-" + std::to_string(static_cast<i64>(effect)), volume->outputSurfaceEffect == effect))
            world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceEffect, static_cast<i64>(effect));

    f64 halfLife = volume->outputSurfaceEffectHalfLifeSeconds;
    if (context.InputDouble("Surface Effect Half Life s##volume-output-effect-half-life", halfLife))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceHalfLife, std::clamp(halfLife, 0.0, 86400.0));

    i64 candidateMultiplier = static_cast<i64>(volume->outputCandidateMultiplier);
    if (context.InputInteger("Candidate Multiplier##volume-output-candidates", candidateMultiplier))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputCandidateMultiplier, std::clamp<i64>(candidateMultiplier, 1, 64));

    context.MutedText("Gravity, terrain collision and standing-water response are authored per particle and remain GPU-resident. Water-entry events are latched for the GPU splash-output consumer.");

    if ((volume->outputParticlesEnabled ||
         volume->outputSurfaceDepositsEnabled) &&
        volume_representation::
            VolumeCaches().Find(
                *volumeId) == nullptr)
    {
        context.MutedText(
            "Output is enabled but this Volume has no CPU-readable field authority yet. No synthetic events are emitted; attach a current M37 cache or use the upcoming live GPU producer.");
    }

    if (const auto* output =
            volume_representation::
                VolumeOutputs().Latest(
                    *volumeId);
        output != nullptr)
    {
        const auto& d =
            output->diagnostics;

        context.Text(
            std::format(
                "Last step {:.4f} s | particles {}/{} | deposits {}/{}",
                d.deltaSeconds,
                d.emittedParticles,
                d.requestedParticles,
                d.emittedSurfaceDeposits,
                d.requestedSurfaceDeposits));
        context.MutedText(
            std::format(
                "Candidates {} | threshold rejects {} | budget drops P {} / S {}",
                d.candidatesTested,
                d.thresholdRejected,
                d.particleBudgetDropped,
                d.surfaceBudgetDropped));
    }

    const auto& runtimeDiagnostics =
        volume_representation::
            VolumeOutputRuntimeService().
                Diagnostics();

    context.MutedText(
        std::format(
            "World runtime: {} volumes | {} eligible | {} advanced | {} without readable authority | dispatched P {} / S {}",
            runtimeDiagnostics.discoveredVolumes,
            runtimeDiagnostics.eligibleVolumes,
            runtimeDiagnostics.advancedVolumes,
            runtimeDiagnostics.volumesWithoutReadableAuthority,
            runtimeDiagnostics.dispatchedParticleRequests,
            runtimeDiagnostics.dispatchedSurfaceRequests));

    if (runtimeDiagnostics.timeReversed)
    {
        context.MutedText(
            "Simulation time moved backward this step; M38 sequence/carry state was reset deterministically.");
    }

    context.MutedText(
        std::format(
            "Pending consumer queues: particles {} | surface requests {}",
            volume_representation::
                VolumeParticleRequests().
                    Pending().size(),
            volume_representation::
                VolumeSurfaceRequests().
                    Pending().size()));

    if (!status_.empty())
    {
        context.MutedText(status_);
    }

    if (settings.showRepresentationRegions)
    {
        const auto available =
            context.ContentAvailable();
        const f32 width =
            std::max(
                std::min(
                    available.width,
                    420.0F),
                220.0F);
        constexpr f32 height =
            190.0F;

        static_cast<void>(
            context.Canvas(
                "##volume-lod-region-map",
                {
                    .width = width,
                    .height = height
                }));

        const math::Float2 center{
            width * 0.5F,
            height * 0.52F
        };

        const f64 outerMeters =
            std::max(
                settings.passiveDistanceMeters,
                settings.liveDistanceMeters +
                    0.01);
        const f32 outerRadius =
            std::max(
                std::min(
                    width,
                    height) * 0.42F,
                20.0F);
        const f32 liveRadius =
            static_cast<f32>(
                settings.liveDistanceMeters /
                outerMeters) *
            outerRadius;

        context.CanvasCircle(
            center,
            outerRadius,
            {0.42F,0.44F,0.48F,0.85F},
            false,
            2.0F);
        context.CanvasCircle(
            center,
            liveRadius,
            {0.20F,0.82F,0.44F,0.95F},
            false,
            2.0F);

        const f32 observerRadius =
            static_cast<f32>(
                std::clamp(
                    diagnostics.
                        distanceToBoundsMeters /
                    outerMeters,
                    0.0,
                    1.0)) *
            outerRadius;

        context.CanvasCircle(
            {
                center.x + observerRadius,
                center.y
            },
            4.0F,
            {1.0F,0.82F,0.18F,1.0F},
            true);

        context.CanvasCircle(
            center,
            4.0F,
            {0.25F,0.72F,1.0F,1.0F},
            true);

        context.CanvasText(
            {8.0F,8.0F},
            {0.86F,0.88F,0.92F,1.0F},
            "Live / Coarse / Passive policy map");
        context.CanvasText(
            {8.0F,height - 34.0F},
            {0.70F,0.73F,0.78F,1.0F},
            std::format(
                "Live <= {:.0f} m | Passive >= {:.0f} m",
                settings.liveDistanceMeters,
                settings.passiveDistanceMeters));
        context.CanvasText(
            {8.0F,height - 18.0F},
            {0.70F,0.73F,0.78F,1.0F},
            std::format(
                "Current: {} | {:.1f} px",
                volume_representation::
                    ResolvedRepresentationName(
                        diagnostics.representation),
                diagnostics.projectedDiameterPixels));
    }
}
} // namespace orbit::studio_ui
