#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::ComposeVolumePasses(
        render_graph::RenderGraph& graph,
        StudioRenderViewSet& views,
        studio_session::StudioSession& session,
        render_view::RenderView* view,
        const StudioRenderViewInfo& info,
        const render_view::ImportedTargets& targets,
        const std::string& prefix,
        std::optional<world_model::ResolvedVolumeDomain>& selectedVolume,
        volume_fields::VolumeFieldStorage*& sharedVolumeStorage,
        std::optional<volume_fields::ImportedVolumeFields>& sharedVolumeFields,
        u32 frameIndex,
        render_graph::BufferHandle sharedRadianceCellsHandle,
        render_graph::BufferHandle sharedRadianceLevelsHandle,
        u32 sharedRadianceLevelCount,
        const studio_session::StudioRuntimeSnapshot& snapshot,
        time::SimulationTime atTime,
        const viewport_detail::ResolvedStudioDirectLight& studioDirectLight,
        const std::function<void(std::string_view)>& recordComposeStage)
{
    const auto* logicalTarget = session.Viewports().Find(info.id);
    const auto width = view->Width();
    const auto height = view->Height();
    auto* color = &view->Color();
    if (selectedVolume.has_value())
    {
        if (!sharedVolumeFields.has_value())
        {
            auto& runtimeVolume = *selectedVolume;

            if (surfaceVolumeSolver_ != nullptr)
            {
                const auto& runtimeSettings =
                    surfaceVolumeSolver_->
                        Settings(
                            runtimeVolume.object);

                if (runtimeSettings.followCamera &&
                    runtimeVolume.solverPolicy ==
                        world_model::
                            VolumeSolverPolicy::
                                Surface2D5D)
                {
                    // One camera owns shared simulation residency. A map
                    // or second viewport must not recenter the same fields
                    // after the first viewport has queued GPU work.
                    const auto* followView = views.Find("studio.primary");
                    if (followView == nullptr ||
                        !followView->Camera().frame.IsValid())
                    {
                        followView = view;
                    }
                    runtimeVolume.centerMeters =
                        followView->Camera().
                            localPositionMeters;
                }
            }

            volumeFields_->RemoveMissing(
                session.World().Objects());

            sharedVolumeStorage = &volumeFields_->Ensure(runtimeVolume);

            static_cast<void>(
                volumeFields_->
                    SyncAuthoredInputs(
                        session.World().Objects(),
                        runtimeVolume.object));

            sharedVolumeFields =
                sharedVolumeStorage->Import(
                    graph,
                    prefix + ".VolumeFields");

            if (surfaceVolumeSolver_ != nullptr)
            {
                surfaceVolumeSolver_->
                    RemoveMissing(
                        session.World().Objects());

                surfaceVolumeSolver_->
                    AddPasses(
                        graph,
                        prefix + ".SurfaceVolumeSolver",
                        session.World().Objects(),
                        runtimeVolume,
                        *sharedVolumeStorage,
                        *sharedVolumeFields,
                        frameIndex %
                            framesInFlight_);

                universalVolumeRenderer_.
                    RemoveMissing(
                        session.World().Objects());
            }
        }

        // Field allocation, input synchronization and the solver step are
        // shared by every viewport; only presentation is view-dependent.
        const auto& runtimeVolume = *selectedVolume;
        auto& fieldStorage = *sharedVolumeStorage;
        const auto& importedFields = *sharedVolumeFields;

        {
            if (surfaceVolumeSolver_ != nullptr)
            {
                std::optional<scene::ObjectId>
                    volumeLightRoot;

                if (logicalTarget->
                        target.has_value() &&
                    snapshot.hasWorld)
                {
                    volumeLightRoot =
                        session.World().
                            Universe().
                            ObjectForBody(
                                logicalTarget->
                                    target->body);
                }

                const auto volumeLocalLights =
                    ResolveStudioLocalLights(
                        session,
                        view->Lighting(),
                        volumeLightRoot);

                render_graph::BufferHandle previousParticleLightGrid{};
                if (volumeParticleRenderer_.ParticleLightGridReady())
                {
                    previousParticleLightGrid = graph.ImportBuffer(
                        prefix + ".ParticleLightGridPrevious",
                        volumeParticleRenderer_.ParticleLightGrid(),
                        rhi::ResourceState::ShaderResource);
                }

                const lighting::DirectionalLight
                    volumeStellarLight{
                        .directionToLight =
                            studioDirectLight.
                                directionBody,
                        .colorLinear = {
                            1.0F,
                            1.0F,
                            1.0F
                        },
                        .irradianceScale =
                            studioDirectLight.
                                irradianceScale
                    };

                universalVolumeRenderer_.
                    AddPasses(
                        graph,
                        prefix +
                            ".UniversalVolume",
                        info.id,
                        targets.color,
                        targets.depth,
                        width,
                        height,
                        view->Camera(),
                        view->Lighting(),
                        runtimeVolume,
                        fieldStorage,
                        importedFields,
                        volumeStellarLight,
                        volumeLocalLights,
                        sharedRadianceCellsHandle,
                        sharedRadianceLevelsHandle,
                        sharedRadianceLevelCount,
                        previousParticleLightGrid,
                        view->Lighting().change !=
                            lighting::
                                LightingViewChange::
                                    None);

                const auto solverSettings =
                    surfaceVolumeSolver_->
                        Settings(
                            runtimeVolume.object);

                if (solverSettings.debugView !=
                    volume_solver::
                        SurfaceVolumeDebugView::Off)
                {
                    render_graph::BufferHandle
                        debugField{};

                    const auto desiredField =
                        solverSettings.debugView ==
                                volume_solver::
                                    SurfaceVolumeDebugView::
                                        FieldSlice
                            ? solverSettings.debugField
                            : solverSettings.debugView ==
                                      volume_solver::
                                          SurfaceVolumeDebugView::
                                              Velocity
                                ? world_model::
                                      VolumeField::Velocity
                                : world_model::
                                      VolumeField::Density;

                    for (const auto& channel :
                         importedFields.channels)
                    {
                        if (channel.field ==
                            desiredField)
                        {
                            debugField =
                                channel.buffer;
                            break;
                        }
                    }

                    if (debugField.IsValid())
                    {
                        const auto fieldDiagnostics =
                            fieldStorage.Diagnostics();
                        const auto camera =
                            view->Camera();
                        const auto debugView =
                            solverSettings.debugView;
                        const auto debugLayer =
                            solverSettings.debugLayer;
                        const auto sliceAxis =
                            solverSettings.sliceAxis;
                        const auto fieldChannel =
                            desiredField;

                        graph.AddPass(
                            prefix +
                                ".SurfaceVolumeDebug",
                            {
                                {
                                    .texture =
                                        targets.color,
                                    .state =
                                        rhi::ResourceState::
                                            RenderTarget,
                                    .access =
                                        render_graph::Access::
                                            Write
                                }
                            },
                            {
                                {
                                    .buffer =
                                        debugField,
                                    .state =
                                        rhi::ResourceState::
                                            ShaderResource,
                                    .access =
                                        render_graph::Access::
                                            Read
                                },
                                {
                                    .buffer =
                                        importedFields.
                                            residency,
                                    .state =
                                        rhi::ResourceState::
                                            ShaderResource,
                                    .access =
                                        render_graph::Access::
                                            Read
                                }
                            },
                            [this,
                             color,
                             width,
                             height,
                             camera,
                             domain =
                                 runtimeVolume,
                             fieldDiagnostics,
                             debugView,
                             sliceAxis,
                             fieldChannel,
                             debugLayer,
                             debugField,
                             residency =
                                 importedFields.
                                     residency](
                                rhi::CommandList& commands,
                                const render_graph::
                                    Resources& resources)
                            {
                                surfaceVolumeDebugRenderer_.
                                    Draw(
                                        commands,
                                        *color,
                                        width,
                                        height,
                                        camera,
                                        domain,
                                        fieldDiagnostics,
                                        debugView,
                                        sliceAxis,
                                        fieldChannel,
                                        debugLayer,
                                        resources.
                                            Buffer(
                                                debugField),
                                        resources.
                                            Buffer(
                                                residency));
                            });
                    }

                    if (runtimeVolume.solverPolicy ==
                            world_model::
                                VolumeSolverPolicy::
                                    Local3D &&
                        solverSettings.debugView ==
                            volume_solver::
                                SurfaceVolumeDebugView::
                                    FieldSlice)
                    {
                        auto sliceLines =
                            VolumeSlicePlaneLines(
                                runtimeVolume,
                                view->Camera(),
                                solverSettings.sliceAxis,
                                solverSettings.debugLayer);

                        if (!sliceLines.empty())
                        {
                            const auto camera =
                                view->Camera();

                            graph.AddPass(
                                prefix +
                                    ".Local3DSlicePlane",
                                {
                                    {
                                        .texture =
                                            targets.color,
                                        .state =
                                            rhi::ResourceState::
                                                RenderTarget,
                                        .access =
                                            render_graph::Access::
                                                Write
                                    }
                                },
                                [this,
                                 color,
                                 width,
                                 height,
                                 camera,
                                 sliceLines =
                                     std::move(
                                         sliceLines)](
                                    rhi::CommandList& commands,
                                    const render_graph::
                                        Resources&)
                                {
                                    pathRenderer_.
                                        DrawCameraRelativeLines(
                                            commands,
                                            *color,
                                            width,
                                            height,
                                            camera,
                                            sliceLines);
                                });
                        }
                    }
                }
            }

            std::vector<render_graph::BufferUse>
                fieldReads;
            fieldReads.reserve(
                importedFields.channels.size() + 1U);

            for (const auto& channel :
                 importedFields.channels)
            {
                fieldReads.push_back({
                    .buffer = channel.buffer,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                });
            }

            fieldReads.push_back({
                .buffer =
                    importedFields.residency,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            });

            graph.AddPass(
                prefix +
                    ".VolumeFieldsReady",
                {},
                std::move(fieldReads),
                [](
                    rhi::CommandList&,
                    const render_graph::Resources&)
                {
                });
        }
    }

    {
        auto volumeInputLines =
            VolumeInputGizmoLines(
                session,
                view->Camera(),
                volumeSourceDebugVisualization_);

        if (!volumeInputLines.empty())
        {
            const auto camera =
                view->Camera();

            graph.AddPass(
                prefix + ".VolumeInputGizmos",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 camera,
                 volumeInputLines =
                     std::move(
                         volumeInputLines)](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    pathRenderer_.
                        DrawCameraRelativeLines(
                            commands,
                            *color,
                            width,
                            height,
                            camera,
                            volumeInputLines);
                });
        }
    }

    {
        auto volumeDomainLines =
            SelectedVolumeDomainLines(
                session,
                view->Camera());

        if (!volumeDomainLines.empty())
        {
            const auto camera =
                view->Camera();

            graph.AddPass(
                prefix + ".VolumeDomainBounds",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 camera,
                 volumeDomainLines =
                     std::move(
                         volumeDomainLines)](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    pathRenderer_.
                        DrawCameraRelativeLines(
                            commands,
                            *color,
                            width,
                            height,
                            camera,
                            volumeDomainLines);
                });
        }
    }

    {
        const auto particleTerrainCollisionPages =
            volumeParticleRenderer_.TerrainCollisionPagesSnapshot();

        recordComposeStage("gi_passes");
        recordComposeStage("atmosphere");
        // M38 particle simulation is shared by all Studio viewports. The
        // authoritative output generation advances GPU state exactly once;
        // later viewports only render that already-advanced state.
        const auto& particleOutput =
            studio_session::VolumeParticleOutputs();
        const u64 outputGeneration =
            particleOutput.Diagnostics().generation;

        if (outputGeneration < particleOutputGeneration_)
        {
            volumeParticleRenderer_.Reset();
            particleOutputGeneration_ = 0U;
            particleSimulationTimeValid_ = false;
            particlePresentationOriginMeters_ = {};
        }

        bool advanceParticleState = false;
        f64 particleDeltaSeconds = 0.0;
        math::Double3 previousParticleOrigin =
            particlePresentationOriginMeters_;
        math::Double3 nextParticleOrigin =
            particlePresentationOriginMeters_;

        if (outputGeneration > 0U &&
            outputGeneration != particleOutputGeneration_)
        {
            nextParticleOrigin =
                view->Camera().localPositionMeters;
            previousParticleOrigin =
                particleOutputGeneration_ == 0U
                    ? nextParticleOrigin
                    : particlePresentationOriginMeters_;

            const auto particleSpawns =
                BuildVolumeParticleRenderBatch(
                    particleOutput.Events(),
                    nextParticleOrigin);
            volumeParticleRenderer_.SetSpawns(
                particleSpawns);

            if (particleSimulationTimeValid_)
            {
                particleDeltaSeconds =
                    static_cast<f64>(
                        (atTime - particleSimulationTime_).count()) /
                    1000000.0;
                if (!std::isfinite(particleDeltaSeconds) ||
                    particleDeltaSeconds < 0.0)
                {
                    particleDeltaSeconds = 0.0;
                }
            }

            particleOutputGeneration_ =
                outputGeneration;
            particleSimulationTime_ = atTime;
            particleSimulationTimeValid_ = true;
            particlePresentationOriginMeters_ =
                nextParticleOrigin;
            advanceParticleState = true;
        }

        if (particleOutputGeneration_ > 0U)
        {
            const auto camera =
                view->Camera();
            std::optional<scene::ObjectId> particleLightRoot;
            if(logicalTarget->target.has_value() && snapshot.hasWorld)
            {
                particleLightRoot=session.World().Universe().ObjectForBody(logicalTarget->target->body);
            }
            const auto particleLocalLights=ResolveStudioLocalLights(session,view->Lighting(),particleLightRoot);
            const lighting::DirectionalLight particleStellarLight{.directionToLight=studioDirectLight.directionBody,.colorLinear={1.0F,1.0F,1.0F},.irradianceScale=studioDirectLight.irradianceScale};
            const math::Double3 cameraRelativeToParticleOrigin{
                camera.localPositionMeters.x -
                    particlePresentationOriginMeters_.x,
                camera.localPositionMeters.y -
                    particlePresentationOriginMeters_.y,
                camera.localPositionMeters.z -
                    particlePresentationOriginMeters_.z
            };
            auto* particleColor = color;
            auto* particleDepth = &view->Depth();
        const u32 particleFrameIndex =
            frameIndex % framesInFlight_;
        const u64 particleTemporalHistoryKey =
            StableViewportHash(info.id);

            graph.AddPass(
                prefix + ".VolumeParticles",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::RenderTarget,
                        .access =
                            render_graph::Access::Write
                    },
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::DepthRead,
                        .access =
                            render_graph::Access::Read
                    }
                },
                [this,
                 particleColor,
                 particleDepth,
                 width,
                 height,
                 camera,
                 cameraRelativeToParticleOrigin,
                 particleFrameIndex,
                 particleTemporalHistoryKey,
                 particleStellarLight,
                 particleLocalLights,
                 advanceParticleState,
                 particleDeltaSeconds,
                 previousParticleOrigin,
                 nextParticleOrigin,
                 particleTerrainCollisionPages](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    if (advanceParticleState)
                    {
                        volumeParticleRenderer_.Advance(
                            commands,
                            particleFrameIndex,
                            particleDeltaSeconds,
                            previousParticleOrigin,
                            nextParticleOrigin);
                        volumeParticleRenderer_.ApplyTerrainCollision(
                            commands,
                            particleTerrainCollisionPages,
                            particleDeltaSeconds);
                    }

                    volumeParticleRenderer_.Draw(
                        commands,
                        *particleColor,
                        *particleDepth,
                        width,
                        height,
                        camera,
                        cameraRelativeToParticleOrigin,
                        particleFrameIndex,
                        particleTemporalHistoryKey,
                        particleStellarLight,
                        particleLocalLights);
                });

            // The particle pass leaves depth in DepthRead; return it to
            // the persistent DepthWrite state RenderView imports next frame.
            graph.AddPass(
                prefix + ".RestoreDepthWriteAfterParticles",
                {
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::DepthWrite,
                        .access =
                            render_graph::Access::Write
                    }
                },
                [](
                    rhi::CommandList&,
                    const render_graph::Resources&)
                {
                });
        }
    }

}
} // namespace orbit::studio_ui
