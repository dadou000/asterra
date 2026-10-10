#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

namespace
{
// Fraction of the sun light that reaches `position` (body-local metres)
// through the body atmosphere, per colour channel: Rayleigh + Mie extinction
// and the ozone layer integrated along the sun ray, zero when the planet is in
// the way. The planet clouds read the same quantity from the transmittance LUT.
[[nodiscard]] math::Float3 SunTransmittanceAt(
    const celestial_atmosphere::AtmosphereParameters& atmosphere,
    const math::Double3& position,
    const math::Float3& sunDirection)
{
    const f64 radius = std::sqrt(
        position.x * position.x + position.y * position.y +
        position.z * position.z);
    if (!(radius > 0.0))
    {
        return {1.0F, 1.0F, 1.0F};
    }
    const math::Double3 up{
        position.x / radius, position.y / radius, position.z / radius};
    const math::Double3 sun{
        static_cast<f64>(sunDirection.x),
        static_cast<f64>(sunDirection.y),
        static_cast<f64>(sunDirection.z)};
    const f64 mu = up.x * sun.x + up.y * sun.y + up.z * sun.z;

    // Below the horizon and inside the planet shadow cylinder the planet
    // blocks the sun completely.
    if (mu < 0.0 &&
        radius * radius * (1.0 - mu * mu) <
            atmosphere.bottomRadiusMeters * atmosphere.bottomRadiusMeters)
    {
        return {0.0F, 0.0F, 0.0F};
    }

    const f64 top = atmosphere.topRadiusMeters;
    const f64 b = radius * mu;
    const f64 discriminant = b * b - (radius * radius - top * top);
    if (discriminant <= 0.0)
    {
        return {1.0F, 1.0F, 1.0F};
    }
    const f64 pathLength = -b + std::sqrt(discriminant);
    if (!(pathLength > 0.0))
    {
        return {1.0F, 1.0F, 1.0F};
    }

    constexpr u32 kSteps = 48U;
    const f64 step = pathLength / static_cast<f64>(kSteps);
    f64 tau[3]{0.0, 0.0, 0.0};
    const f64 rayleigh[3]{
        atmosphere.rayleighScatteringPerMeter.x,
        atmosphere.rayleighScatteringPerMeter.y,
        atmosphere.rayleighScatteringPerMeter.z};
    const f64 mie[3]{
        atmosphere.mieExtinctionPerMeter.x,
        atmosphere.mieExtinctionPerMeter.y,
        atmosphere.mieExtinctionPerMeter.z};
    const f64 ozone[3]{
        atmosphere.absorptionExtinctionPerMeter.x,
        atmosphere.absorptionExtinctionPerMeter.y,
        atmosphere.absorptionExtinctionPerMeter.z};

    for (u32 index = 0U; index < kSteps; ++index)
    {
        const f64 t = (static_cast<f64>(index) + 0.5) * step;
        const math::Double3 point{
            position.x + sun.x * t,
            position.y + sun.y * t,
            position.z + sun.z * t};
        const f64 altitude = std::max(
            std::sqrt(
                point.x * point.x + point.y * point.y + point.z * point.z) -
                atmosphere.bottomRadiusMeters,
            0.0);
        const f64 rayleighDensity =
            std::exp(-altitude / atmosphere.rayleighScaleHeightMeters);
        const f64 mieDensity =
            std::exp(-altitude / atmosphere.mieScaleHeightMeters);
        const f64 ozoneDensity = std::max(
            0.0,
            1.0 -
                std::abs(altitude - atmosphere.absorptionCenterHeightMeters) /
                    atmosphere.absorptionHalfWidthMeters);
        for (u32 channel = 0U; channel < 3U; ++channel)
        {
            tau[channel] +=
                (rayleigh[channel] * rayleighDensity +
                 mie[channel] * mieDensity +
                 ozone[channel] * ozoneDensity) *
                step;
        }
    }

    return {
        static_cast<f32>(std::exp(-tau[0])),
        static_cast<f32>(std::exp(-tau[1])),
        static_cast<f32>(std::exp(-tau[2]))};
}
} // namespace

StudioViewportRenderer::VolumeSunLighting
StudioViewportRenderer::ResolveVolumeSunLighting(
    const std::string_view viewId,
    const studio_session::ViewportTargetState* logicalTarget,
    const world_model::ResolvedVolumeDomain& domain,
    const math::Float3& sunDirection) const
{
    VolumeSunLighting result;
    const auto found = atmospherePresentations_.find(viewId);
    if (found == atmospherePresentations_.end() ||
        logicalTarget == nullptr ||
        !logicalTarget->target.has_value() ||
        found->second.body != logicalTarget->target->body)
    {
        return result;
    }

    const auto& atmosphere = found->second.parameters;
    const auto& centre = domain.centerMeters;
    const f64 centreRadius = std::sqrt(
        centre.x * centre.x + centre.y * centre.y + centre.z * centre.z);
    if (centreRadius > 0.0)
    {
        const math::Double3 up{
            centre.x / centreRadius,
            centre.y / centreRadius,
            centre.z / centreRadius};
        const f64 half = std::max(domain.halfExtentsMeters.z, 1.0);
        const f64 bottomRadius = std::max(
            centreRadius - half, atmosphere.bottomRadiusMeters + 1.0);
        const f64 topRadius = std::min(
            centreRadius + half, atmosphere.topRadiusMeters - 1.0);
        result.bottom = SunTransmittanceAt(
            atmosphere,
            {up.x * bottomRadius, up.y * bottomRadius, up.z * bottomRadius},
            sunDirection);
        result.top = SunTransmittanceAt(
            atmosphere,
            {up.x * topRadius, up.y * topRadius, up.z * topRadius},
            sunDirection);
    }

    if (found->second.gpu != nullptr)
    {
        result.luts = {
            .transmittance = &found->second.gpu->Transmittance(),
            .multiScattering = &found->second.gpu->MultiScattering(),
            .bottomRadiusMeters =
                static_cast<f32>(atmosphere.bottomRadiusMeters),
            .topRadiusMeters = static_cast<f32>(atmosphere.topRadiusMeters)};
    }
    return result;
}

void StudioViewportRenderer::ComposeVolumeLayerPasses(
    render_graph::RenderGraph& graph,
    studio_session::StudioSession& session,
    render_view::RenderView* view,
    const StudioRenderViewInfo& info,
    const render_view::ImportedTargets& targets,
    const std::string& prefix,
    const std::vector<world_model::ResolvedVolumeDomain>& layers,
    const viewport_detail::ResolvedStudioDirectLight& studioDirectLight)
{
    if (layers.empty() || volumeFields_ == nullptr)
    {
        return;
    }

    const auto* logicalTarget = session.Viewports().Find(info.id);
    const auto width = view->Width();
    const auto height = view->Height();

    u32 index = 0U;
    for (const auto& domain : layers)
    {
        const std::string layerPrefix =
            prefix + ".VolumeLayer" + std::to_string(index++);

        auto& storage = volumeFields_->Ensure(domain);
        const auto fields = storage.Import(graph, layerPrefix + ".Fields");

        const auto sun = ResolveVolumeSunLighting(
            info.id, logicalTarget, domain, studioDirectLight.directionBody);
        const lighting::DirectionalLight stellar{
            .directionToLight = studioDirectLight.directionBody,
            .colorLinear = sun.bottom,
            .irradianceScale = studioDirectLight.irradianceScale};

        universalVolumeRenderer_.AddPasses(
            graph,
            layerPrefix + ".UniversalVolume",
            info.id,
            targets.color,
            targets.depth,
            width,
            height,
            view->Camera(),
            view->Lighting(),
            domain,
            storage,
            fields,
            stellar,
            {},
            {},
            {},
            0U,
            {},
            view->Lighting().change != lighting::LightingViewChange::None,
            sun.top,
            sun.luts);
    }
}

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

                const auto sunLighting =
                    ResolveVolumeSunLighting(
                        info.id,
                        logicalTarget,
                        runtimeVolume,
                        studioDirectLight.directionBody);
                const math::Float3 sunColorBottom = sunLighting.bottom;
                const math::Float3 sunColorTop = sunLighting.top;
                const volume_render::VolumeAtmosphereLuts& atmosphereLuts =
                    sunLighting.luts;

                const lighting::DirectionalLight
                    volumeStellarLight{
                        .directionToLight =
                            studioDirectLight.
                                directionBody,
                        .colorLinear = sunColorBottom,
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
                                    None,
                        sunColorTop,
                        atmosphereLuts);

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
            particleEverSpawned_ = false;
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
            if (!particleSpawns.empty())
            {
                particleEverSpawned_ = true;
                particleLastSpawn_ = std::chrono::steady_clock::now();
            }

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

        constexpr auto kParticleGrace = std::chrono::seconds(30);
        const bool particlesMayExist =
            particleEverSpawned_ &&
            std::chrono::steady_clock::now() - particleLastSpawn_ <
                kParticleGrace;

        if (particleOutputGeneration_ > 0U && particlesMayExist)
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
