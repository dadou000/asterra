#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::ComposeTerrainPass(StudioScenePassContext& context)
{
    auto& [graph, views, session, view, info, targets, prefix, terrainRuntime,
        nearFieldWaterWeight, nearFieldSeaLevelMeters, hasMacroGlobe,
        studioDirectLight, cpuTimingRecorder, composeStageStarted,
        resolvedOceanForView, frameIndex, drawBackgroundBodies,
        recordComposeStage, macroGlobeSurface, shape,
        acquireCelestialGrant, completeCelestialGrant] = context;

    const auto width = view->Width();
    const auto height = view->Height();
    auto* color = &view->Color();

    if (device_ == nullptr ||
        compiler_ == nullptr ||
        !terrainRuntime.has_value())
    {
        throw std::logic_error(
            "Studio production-terrain presentation lost its device, compiler or runtime binding.");
    }

    const auto& source =
        session.TerrainRuntime().
            TerrainSource(
                *terrainRuntime);

    const auto* analytic =
        dynamic_cast<
            const terrain::
                AnalyticTerrainSource*>(
                    &source);

    if (analytic == nullptr)
    {
        throw std::logic_error(
            "Studio production terrain currently requires the composed AnalyticTerrainSource used by the shared GPU field generator.");
    }

    const auto& terrainDescription =
        analytic->Description();

    const f64 maximumProductionDetailMeters =
        std::max(
            terrainDescription.detailAmplitudeMeters *
                2.0,
            terrainDescription.mountains.reliefMeters);

    const f64 maximumMacroDisplacementMeters =
        std::max(
            terrainDescription.
                maximumElevationAboveSeaLevelMeters,
            std::abs(
                terrainDescription.
                    global.
                    seaLevelMeters));

    celestial_representation::ResolveInput
        representationInput{
            .bodyRadiusMeters =
                terrainRuntime->planet.radiusMeters,
            .maximumProductionDetailMeters =
                maximumProductionDetailMeters,
            .maximumMacroDisplacementMeters =
                maximumMacroDisplacementMeters,
            .cameraDistanceToCenterMeters =
                math::Length(
                    terrainRuntime->
                        observer.meters),
            .verticalFieldOfViewRadians =
                static_cast<f64>(
                    view->Camera().
                        verticalFovRadians),
            .viewportHeightPixels =
                static_cast<f64>(
                    std::max(
                        height,
                        1U)),
            .features = {
                .productionSurfaceAvailable = true,
                // The full clipmap renderer never hands over to the
                // orbital globe: with no macro representation the
                // resolver keeps the production clipmap at every range.
                .macroDisplacementAvailable =
                    hasMacroGlobe &&
                    !info.layers.fullClipmap,
                .complexFarAppearance =
                    hasMacroGlobe &&
                    !info.layers.fullClipmap &&
                    !studioDirectLight.
                        direct.has_value(),
                .radiativeEmitter = false
            },
            .policy =
                celestialQualityPolicy_
        };
    // The view's LOD bias keeps richer representations longer (+) or
    // hands over to coarser ones sooner (-).
    representationInput.policy.qualityScale =
        std::clamp(
            representationInput.policy.qualityScale *
                std::exp2(
                    static_cast<f64>(
                        info.layers.lodBiasStops)),
            0.05,
            64.0);

    const celestial_representation::
        RepresentationSubjectId
        representationSubject{
            .high =
                terrainRuntime->body.high,
            .low =
                terrainRuntime->body.low
        };

    auto representationDecision =
        representationTracker_.ResolveFor(
            representationSubject,
            representationInput);

    auto representationBlend =
        celestial_representation::
            ResolveRepresentationBlend(
                representationInput,
                representationDecision);

    // Full clipmap renderer: the production clipmap is the only terrain
    // representation, from the ground to orbit. The resolver would hand
    // over to a smooth globe or impostor as the body shrinks on screen;
    // that choice is overridden here so no other representation draws.
    if (info.layers.fullClipmap)
    {
        representationDecision.representation =
            celestial_representation::Representation::ProductionSurface;
        representationDecision.lowerFidelityNeighbor =
            celestial_representation::Representation::ProductionSurface;
        representationDecision.hysteresisHeld = false;
        representationBlend.richer =
            celestial_representation::Representation::ProductionSurface;
        representationBlend.lower =
            celestial_representation::Representation::ProductionSurface;
        representationBlend.richerWeight = 1.0;
        representationBlend.lowerWeight = 0.0;
        representationBlend.overlapping = false;
    }

    const auto weightFor =
        [&](const celestial_representation::
                Representation representation)
        {
            f64 weight = 0.0;

            if (representationBlend.richer ==
                representation)
            {
                weight +=
                    representationBlend.
                        richerWeight;
            }

            if (representationBlend.lower ==
                    representation &&
                representationBlend.lower !=
                    representationBlend.richer)
            {
                weight +=
                    representationBlend.
                        lowerWeight;
            }

            return std::clamp(
                weight,
                0.0,
                1.0);
        };

    const f64 productionWeight =
        info.layers.productionSurface
            ? weightFor(
                  celestial_representation::
                      Representation::
                          ProductionSurface)
            : 0.0;
    const f64 macroWeight =
        weightFor(
            celestial_representation::
                Representation::
                    MacroDisplacedGlobe);

    nearFieldWaterWeight = productionWeight;
    nearFieldSeaLevelMeters = static_cast<f32>(
        terrainDescription.global.seaLevelMeters);

    transitionDiagnostics_.insert_or_assign(
        info.id,
        StudioSurfaceGlobeTransitionDiagnostics{
            .body =
                terrainRuntime->body,
            .representation =
                representationDecision.
                    representation,
            .lowerFidelityNeighbor =
                representationDecision.
                    lowerFidelityNeighbor,
            .richerWeight =
                representationBlend.richerWeight,
            .lowerWeight =
                representationBlend.lowerWeight,
            .productionSurfaceWeight =
                productionWeight,
            .macroGlobeWeight =
                macroWeight,
            .projectedRadiusPixels =
                representationDecision.
                    projectedRadiusPixels,
            .productionDetailErrorPixels =
                representationDecision.
                    productionDetailErrorPixels,
            .macroDisplacementErrorPixels =
                representationDecision.
                    macroDisplacementErrorPixels,
            .hysteresisHeld =
                representationDecision.
                    hysteresisHeld,
            .overlapping =
                representationBlend.
                    overlapping
        });

    auto& terrain =
        terrainPresentations_[
            info.id];
    if (cpuTimingRecorder)
    {
        const auto now = std::chrono::steady_clock::now();
        cpuTimingRecorder(
            "celestial",
            std::chrono::duration<double, std::milli>(
                now - composeStageStarted).count());
        composeStageStarted = now;
    }

    // The field generator depends only on the planet and its terrain
    // source, not on the clipmap. The adaptive coverage tier changes the
    // clipmap at a few altitudes; rebuilding the generator there
    // recompiled its compute shader on the frame thread (a ~3 s freeze
    // each time), so a clipmap-only change keeps it and rebuilds just
    // the renderer.
    const bool recreateGenerator =
        terrain.fieldGenerator == nullptr ||
        terrain.universeGeneration !=
            terrainRuntime->
                universeGeneration ||
        terrain.body !=
            terrainRuntime->body ||
        terrain.planet !=
            terrainRuntime->planet.id ||
        terrain.terrainSourceRevision !=
            terrainRuntime->
                terrainSourceRevision;

    const terrain_view::ClipmapConfig effectiveClipmap =
        EffectiveClipmapConfig(terrainRuntime->clipmap, info.layers);
    const bool recreate =
        recreateGenerator ||
        terrain.renderer == nullptr ||
        !SameClipmapConfig(
            terrain.clipmap,
            effectiveClipmap);

    if (recreate)
    {
        terrain_render::
            TerrainPreviewConfig
            config{};

        config.clipmap =
            effectiveClipmap;
        config.adaptiveCoverage.enabled =
            false;
        config.nearPlaneMeters =
            std::max(
                view->Camera().
                    nearPlaneMeters,
                0.01F);
        config.farPlaneMeters =
            std::max(
                view->Camera().
                    farPlaneMeters,
                config.nearPlaneMeters *
                    100.0F);
        config.framesInFlight =
            framesInFlight_;
        config.drySurface =
            !resolvedOceanForView.has_value();

        if (recreateGenerator)
        {
            // The renderer references the generator; drop it first.
            terrain.renderer.reset();
            terrain.fieldGenerator =
                std::make_unique<
                    terrain_gpu::
                        GpuFieldGenerator>(
                            *device_,
                            *compiler_,
                            terrainRuntime->
                                planet,
                            *analytic);
        }

        terrain.renderer =
            std::make_unique<
                terrain_render::
                    TerrainPreviewRenderer>(
                        *device_,
                        *compiler_,
                        terrainRuntime->
                            planet,
                        *terrain.
                            fieldGenerator,
                        terrainRuntime->
                            observer,
                        config);

        terrain.universeGeneration =
            terrainRuntime->
                universeGeneration;
        terrain.body =
            terrainRuntime->body;
        terrain.planet =
            terrainRuntime->planet.id;
        terrain.terrainSourceRevision =
            terrainRuntime->
                terrainSourceRevision;
        terrain.clipmap =
            effectiveClipmap;
        terrain.observer =
            terrainRuntime->observer;
    }
    else if (
        terrain.observer.meters !=
            terrainRuntime->
                observer.meters)
    {
        terrain.renderer->
            UpdateObserver(
                terrainRuntime->
                    observer);

        terrain.observer =
            terrainRuntime->observer;
    }

    terrain.runtimeGeneration =
        terrainRuntime->
            runtimeGeneration;

    auto physicalPages =
        BuildPhysicalRenderPages(
            session,
            *terrainRuntime,
            *analytic,
            *device_);

    if (info.layers.physicalPages)
    {
        terrain.renderer->
            SetPhysicalPages(
                physicalPages.pages,
                physicalPages.generation);
    }
    else
    {
        // A distinct generation so toggling back re-applies the pages.
        terrain.renderer->SetPhysicalPages({}, 0U);
    }

    const auto particleBodyIdentity =
        VolumeParticleBodyIdentityWords(
            terrainRuntime->body);
    std::vector<
        volume_render::VolumeParticleTerrainCollisionPage>
        particleCollisionPages;
    particleCollisionPages.reserve(
        physicalPages.pages.size());
    for (const auto& page : physicalPages.pages)
    {
        if (!page.IsValid())
        {
            continue;
        }
        particleCollisionPages.push_back({
            .samples = page.samples,
            .resolution = page.resolution,
            .face = static_cast<u32>(page.address.tile.face),
            .level = page.address.tile.level,
            .tileX = page.address.tile.x,
            .tileY = page.address.tile.y,
            .bodyIdentity = particleBodyIdentity
        });
    }
    volumeParticleRenderer_.UpdateTerrainCollisionPages(
        particleBodyIdentity,
        particleCollisionPages);

    const auto surfaceEffects =
        BuildVolumeSurfaceEffectRenderBatch(
            terrainRuntime->body,
            studio_session::VolumeSurfaceEffects().Stamps(),
            terrain_render::SurfaceEffectGpuBinding::MaximumStampCount);

    terrain.renderer->
        SetSurfaceEffects(
            info.layers.surfaceEffects
                ? surfaceEffects.stamps
                : decltype(surfaceEffects.stamps){});
    terrain.renderer->
        SetDrySurface(
            !resolvedOceanForView.has_value());

    // Dynamic clipmap levels: which of the ladder's levels this camera
    // needs, planned from distance to the ground rather than a fixed ring.
    {
        terrain_view::ClipmapPlannerConfig planner{};
        planner.enabled = info.layers.dynamicClipmaps;
        // The LOD bias moves the planner's target spacing too: +1 stop
        // asks for twice the samples per pixel.
        planner.pixelsPerVertex =
            static_cast<f64>(info.layers.clipmapPixelsPerVertex) *
            std::exp2(-static_cast<f64>(info.layers.lodBiasStops));
        terrain.renderer->SetClipmapPlanner(planner);
        terrain.renderer->SetLevelFadeSeconds(
            static_cast<f64>(info.layers.clipmapFadeSeconds));
        // "Tint terrain by clipmap level": the clipmap shader colours
        // each level differently, so the active set is visible.
        const auto overlays = views.TerrainDiagnosticOverlays(info.id);
        terrain.renderer->SetDebugVisuals(
            overlays.clipmapLevels, false, overlays.clipmapSampleHealth, overlays.clipmapHoleView,
            overlays.clipmapProjectionView, overlays.clipmapShadingView);
        terrain.renderer->SetWireframe(overlays.clipmapWireframe);
        terrain.renderer->SetClipmapFrozen(overlays.clipmapFreeze);

        const f64 groundElevation = analytic->Sample({
            .unitDirection = math::Normalize(
                terrainRuntime->observer.meters),
            .footprintMeters = 500.0,
            .planet = terrainRuntime->planet.id,
            .radialOffsetMeters = 0.0
        }).elevationMeters;
        terrain.renderer->SetGroundElevationHint(groundElevation);

        // The ground the clipmap actually draws under the camera (read
        // back from the GPU a few frames late) is the floor navigation
        // uses next to the CPU terrain.
        const auto drawnGround = terrain.renderer->RenderedGround();
        // The CPU terrain at exactly the vertices the GPU value came from
        // (same positions, same footprint), to tell a real generator
        // mismatch from a difference in what is being compared.
        f64 cpuAtDrawnGround = 0.0;
        if (drawnGround.valid)
        {
            cpuAtDrawnGround = -1.0e30;
            for (const auto& cornerDirection : drawnGround.cornerDirections)
            {
                cpuAtDrawnGround = std::max(
                    cpuAtDrawnGround,
                    analytic->Sample({
                        .unitDirection = cornerDirection,
                        .footprintMeters = drawnGround.footprintMeters,
                        .planet = terrainRuntime->planet.id,
                        .radialOffsetMeters = 0.0
                    }).elevationMeters);
            }
        }
        if (drawnGround.valid)
        {
            views.SetRenderedGround(
                info.id,
                drawnGround.direction,
                drawnGround.elevationMeters);
        }

        const auto& clipmapPlan = terrain.renderer->ClipmapPlan();
        const auto& streaming = terrain.renderer->StreamingStats();
        std::vector<StudioClipmapLevelStats> drawnLevels;
        for (const auto& level : terrain.renderer->ClipmapLevels())
        {
            if (!level.active)
            {
                continue;
            }
            drawnLevels.push_back({
                .level = level.level,
                .spacingMeters = level.spacingMeters,
                .halfExtentMeters = level.halfExtentMeters,
                .bandInnerMeters = level.bandInnerMeters,
                .bandOuterMeters = level.bandOuterMeters,
                .gridResolution = level.gridResolution,
                .drawnVertices = level.drawnVertices});
        }
        clipmapPlanStats_.insert_or_assign(
            info.id,
            StudioClipmapPlanStats{
                .valid = true,
                .dynamic = clipmapPlan.dynamic,
                .banded = terrain.renderer->ClipmapBanded(),
                .levels = std::move(drawnLevels),
                .frozen = overlays.clipmapFreeze,
                .wireframe = overlays.clipmapWireframe,
                .ladderLevels = streaming.ladderLevels,
                .firstLevel = clipmapPlan.firstLevel,
                .lastLevel = clipmapPlan.lastLevel,
                .nearestGroundMeters = streaming.nearestGroundMeters,
                .visibleArcMeters = streaming.visibleArcMeters,
                .requiredSpacingMeters = streaming.requiredSpacingMeters,
                .finestSpacingMeters = clipmapPlan.finestSpacingMeters,
                .coarsestHalfExtentMeters =
                    clipmapPlan.coarsestHalfExtentMeters,
                .planChanges = streaming.planChanges,
                .generatedSamples = streaming.cumulativeGeneratedSamples,
                .groundElevationMeters = groundElevation,
                .renderedGroundValid = drawnGround.valid,
                .renderedGroundElevationMeters =
                    drawnGround.elevationMeters,
                .renderedGroundCpuElevationMeters = cpuAtDrawnGround,
                .renderedGroundFootprintMeters =
                    drawnGround.footprintMeters});
    }

    const auto camera =
        TerrainCameraFromBodyCamera(
            view->Camera(),
            terrain.renderer->CameraFrame());

    auto* terrainRenderer =
        terrain.renderer.get();
    auto* surfaceBaseRoughness =
        &view->SurfaceBaseRoughness();
    auto* surfaceNormalMetallic =
        &view->SurfaceNormalMetallic();
    auto* surfaceEmissionClass =
        &view->SurfaceEmissionClass();
    auto* depth =
        &view->Depth();

    auto* performance =
        &session.TerrainPerformance();

    const std::string
        performanceViewportId =
            info.id;

    const world::WorldPosition
        performanceObserver =
            terrainRuntime->observer;

    const auto performanceCacheStats =
        terrainRuntime->cacheStats;

    const std::string
        performanceAdapter =
            std::string(
                device_->
                    AdapterName());

    if (productionWeight > 0.0)
    {
        graph.AddPass(
            prefix + ".ProductionTerrain",
            {
                {
                    .texture = targets.color,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceBaseRoughness,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceNormalMetallic,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceEmissionClass,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.depth,
                    .state = rhi::ResourceState::DepthWrite,
                    .access = render_graph::Access::Write
                }
            },
            [color,
             surfaceBaseRoughness,
             surfaceNormalMetallic,
             surfaceEmissionClass,
             depth,
             width,
             height,
             terrainRenderer,
             camera,
             frameIndex,
             performance,
             performanceViewportId,
             performanceObserver,
             performanceCacheStats,
             performanceAdapter,
             drawBackgroundBodies,
             framesInFlight =
                framesInFlight_](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                commands.ClearColorTarget(
                    *color,
                    {
                        .red = 0.0F,
                        .green = 0.0F,
                        .blue = 0.0F,
                        .alpha = 1.0F
                    });

                commands.ClearColorTarget(
                    *surfaceBaseRoughness,
                    {0.0F, 0.0F, 0.0F, 1.0F});
                commands.ClearColorTarget(
                    *surfaceNormalMetallic,
                    {0.0F, 1.0F, 0.0F, 0.0F});
                commands.ClearColorTarget(
                    *surfaceEmissionClass,
                    {0.0F, 0.0F, 0.0F, 0.0F});
                commands.ClearDepthTarget(
                    *depth,
                    0.0F);

                drawBackgroundBodies(commands);

                const std::array<rhi::Texture*, 4> surfaceTargets{
                    color,
                    surfaceBaseRoughness,
                    surfaceNormalMetallic,
                    surfaceEmissionClass
                };
                commands.SetRenderTargets(
                    surfaceTargets,
                    depth);

                terrainRenderer->Draw(
                    commands,
                    frameIndex %
                        framesInFlight,
                    width,
                    height,
                    camera);

                const auto& stats =
                    terrainRenderer->
                        StreamingStats();

                performance->
                    RecordViewportStreaming(
                        performanceViewportId,
                        performanceObserver,
                        performanceCacheStats,
                        {
                            .generatedSamplesLastUpdate =
                                stats.generatedSamplesLastUpdate,
                            .refreshedRegionsLastUpdate =
                                stats.refreshedRegionsLastUpdate,
                            .levelsTouchedLastUpdate =
                                stats.levelsTouchedLastUpdate,
                            .uploadedBytesLastFrame =
                                stats.uploadedBytesLastFrame,
                            .drawCallsLastFrame =
                                stats.drawCallsLastFrame,
                            .cumulativeGeneratedSamples =
                                stats.cumulativeGeneratedSamples,
                            .cumulativeUploadedBytes =
                                stats.cumulativeUploadedBytes,
                            .submittedBatches =
                                stats.submittedBatches,
                            .committedBatches =
                                stats.committedBatches,
                            .supersededBatches =
                                stats.supersededBatches,
                            .revisionInvalidations =
                                stats.revisionInvalidations,
                            .staleRevisionBatches =
                                stats.staleRevisionBatches,
                            .coverageTierChanges =
                                stats.coverageTierChanges,
                            .rebaseCount =
                                stats.rebaseCount,
                            .adaptiveCoverageTier =
                                stats.adaptiveCoverageTier,
                            .activeBaseSpacingMeters =
                                stats.activeBaseSpacingMeters,
                            .activeOuterHalfExtentMeters =
                                stats.activeOuterHalfExtentMeters,
                            .updatePending =
                                stats.updatePending
                        },
                        performanceAdapter);
            });
    }

    // Keep a closed globe behind the local terrain at every altitude.
    recordComposeStage("terrain");
    if (hasMacroGlobe &&
        macroGlobeSurface != nullptr &&
        macroGlobeSurface->terrain != nullptr &&
        shape.has_value())
    {
        auto* transitionGlobe =
            EnsureMacroGlobePresentation(
                info.id,
                session,
                terrainRuntime->body,
                *shape,
                *macroGlobeSurface->terrain,
                representationDecision.
                    projectedRadiusPixels,
                [&](const u64 revision)
                {
                    return acquireCelestialGrant(
                        info.id,
                        terrainRuntime->body,
                        celestial_scheduler::
                            WorkKind::FarImpostor,
                        revision,
                        celestial_scheduler::
                            WorkBackend::Gpu,
                        5U,
                        75,
                        true);
                },
                [&](const u64 revision)
                {
                    static_cast<void>(
                        completeCelestialGrant(
                            info.id,
                            terrainRuntime->body,
                            celestial_scheduler::
                                WorkKind::FarImpostor,
                            revision));
                });

        auto* farPresentation =
            &macroGlobePresentations_[
                info.id];

        const auto globeCamera =
            view->Camera();

        const bool clearForFarOnly =
            productionWeight <= 0.0;

        const auto drawRepresentation =
            [this,
             color,
             width,
             height,
             transitionGlobe,
             depth,
             farPresentation,
             surfaceBaseRoughness,
             surfaceNormalMetallic,
             surfaceEmissionClass,
             shape = *shape,
             globeCamera,
             studioDirectLight,
             resolvedOceanForView,
             globeLodBias = info.layers.lodBiasStops,
             macroGlobeLayer = (info.layers.macroGlobe && !info.layers.fullClipmap),
             projectedRadius =
                representationDecision.
                    projectedRadiusPixels](
                rhi::CommandList& commands,
                const celestial_representation::
                    Representation representation,
                const f32 opacity)
            {
                if (opacity <= 0.0F)
                {
                    return;
                }

                if (representation ==
                    celestial_representation::
                        Representation::
                            MacroDisplacedGlobe)
                {
                    if (transitionGlobe == nullptr ||
                        !macroGlobeLayer)
                    {
                        return;
                    }

                    const auto macroLighting =
                        celestial_globe::MacroGlobeLighting{
                            .directionBody =
                                studioDirectLight.directionBody,
                            .irradianceScale =
                                studioDirectLight.irradianceScale,
                            .oceanRefractiveIndex =
                                static_cast<f32>(
                                    resolvedOceanForView.has_value()
                                        ? resolvedOceanForView->optical.refractiveIndex
                                        : 1.333),
                            .oceanRoughness =
                                static_cast<f32>(
                                    resolvedOceanForView.has_value()
                                        ? resolvedOceanForView->optical.orbitalRoughness
                                        : 0.12),
                            .oceanGlintStrength =
                                static_cast<f32>(
                                    resolvedOceanForView.has_value()
                                        ? resolvedOceanForView->optical.glintStrength
                                        : 1.0),
                            .oceanEnabled =
                                resolvedOceanForView.has_value(),
                            .lodBiasStops = globeLodBias
                        };

                    if (opacity >= 0.5F)
                    {
                        macroGlobeRenderer_.DrawSurface(
                            commands,
                            *color,
                            *surfaceBaseRoughness,
                            *surfaceNormalMetallic,
                            *surfaceEmissionClass,
                            width,
                            height,
                            *transitionGlobe,
                            globeCamera,
                            opacity,
                            macroLighting,
                            depth);
                    }
                    else
                    {
                        macroGlobeRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            *transitionGlobe,
                            globeCamera,
                            opacity,
                            macroLighting);
                    }
                    return;
                }

                celestial_far_render::FarBodyDraw
                    draw{
                        .representation =
                            representation,
                        .shape = shape,
                        .camera = globeCamera,
                        .appearance =
                            farPresentation->
                                appearanceSummary,
                        .projectedRadiusPixels =
                            projectedRadius,
                        .opacity = opacity,
                        .lightDirectionBody =
                            studioDirectLight.
                                directionBody,
                        .incidentLightScale =
                            studioDirectLight.
                                irradianceScale,
                        .oceanRefractiveIndex =
                            static_cast<f32>(
                                resolvedOceanForView.has_value()
                                    ? resolvedOceanForView->optical.refractiveIndex
                                    : 1.333),
                        .oceanRoughness =
                            static_cast<f32>(
                                resolvedOceanForView.has_value()
                                    ? resolvedOceanForView->optical.orbitalRoughness
                                    : 0.12),
                        .oceanGlintStrength =
                            static_cast<f32>(
                                resolvedOceanForView.has_value()
                                    ? resolvedOceanForView->optical.glintStrength
                                    : 1.0),
                        .oceanEnabled =
                            resolvedOceanForView.has_value(),
                        .stellar =
                            representation ==
                            celestial_representation::
                                Representation::
                                    StellarPointProxy
                    };

                farBodyRenderer_.Draw(
                    commands,
                    *color,
                    width,
                    height,
                    draw,
                    farPresentation->
                        cachedDisc.get());
                if (opacity >= 0.5F)
                {
                    farBodyRenderer_.DrawSurfaceData(
                        commands,
                        *surfaceBaseRoughness,
                        *surfaceNormalMetallic,
                        *surfaceEmissionClass,
                        width,
                        height,
                        draw);
                }
            };

        const auto richer =
            representationBlend.richer;
        const auto lower =
            representationBlend.lower;
        const f32 lowerOpacity =
            static_cast<f32>(
                std::clamp(
                    representationBlend.
                        lowerWeight,
                    0.0,
                    1.0));

        graph.AddPass(
            prefix +
                ".CelestialFarTransition",
            {
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture = targets.surfaceBaseRoughness,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceNormalMetallic,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.depth,
                    .state = rhi::ResourceState::DepthWrite,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceEmissionClass,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                }
            },
            [color,
             depth,
             surfaceBaseRoughness,
             surfaceNormalMetallic,
             surfaceEmissionClass,
             clearForFarOnly,
             drawBackgroundBodies,
             richer,
             lower,
             lowerOpacity,
             farPresentation,
             drawRepresentation](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                if ((richer ==
                         celestial_representation::
                             Representation::
                                 CachedDiscImpostor ||
                     lower ==
                         celestial_representation::
                             Representation::
                                 CachedDiscImpostor) &&
                    farPresentation->cachedDisc !=
                        nullptr)
                {
                    farPresentation->
                        cachedDisc->
                        EnsureUploaded(
                            commands);
                }

                if (clearForFarOnly)
                {
                    commands.ClearDepthTarget(*depth, 0.0F);
                    const std::array<rhi::Texture*, 4> clearTargets{
                        color, surfaceBaseRoughness,
                        surfaceNormalMetallic, surfaceEmissionClass};
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
                            .alpha = 1.0F
                        });

                    commands.ClearColorTarget(
                        *surfaceBaseRoughness,
                        {0.0F, 0.0F, 0.0F, 1.0F});
                    commands.ClearColorTarget(
                        *surfaceNormalMetallic,
                        {0.0F, 1.0F, 0.0F, 0.0F});
                    commands.ClearColorTarget(
                        *surfaceEmissionClass,
                        {0.0F, 0.0F, 0.0F, 0.0F});
                    commands.SetRenderTargets(clearTargets, depth);
                    drawBackgroundBodies(commands);
                }

                if (richer == celestial_representation::Representation::ProductionSurface)
                {
                    // Depth composes the whole sphere with the local patch;
                    // a global alpha fade cannot represent spatial coverage.
                    drawRepresentation(commands,
                        celestial_representation::Representation::MacroDisplacedGlobe,
                        1.0F);
                    return;
                }

                if (richer !=
                    celestial_representation::
                        Representation::
                            ProductionSurface)
                {
                    drawRepresentation(
                        commands,
                        richer,
                        1.0F);
                }

                if (lower != richer &&
                    lower !=
                        celestial_representation::
                            Representation::
                                ProductionSurface)
                {
                    drawRepresentation(
                        commands,
                        lower,
                        lowerOpacity);
                }
            });
    }
}
} // namespace orbit::studio_ui
