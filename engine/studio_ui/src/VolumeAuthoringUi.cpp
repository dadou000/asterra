#include <orbit/studio_ui/VolumeAuthoringUi.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <array>
#include <exception>
#include <format>
#include <optional>

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
