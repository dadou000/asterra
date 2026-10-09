#include "StudioViewportInternals.hpp"

#include <atomic>
#include <exception>
#include <optional>

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::ComposeLightingPasses(StudioLightingPassContext& context)
{
    auto& [
        graph, session, view, info,
        targets, prefix, presentation, snapshot,
        atTime, frameIndex, studioDirectLight, terrainRuntime,
        lightingPlan, lightingTimestamps, cpuTimingRecorder, recordComposeStage,
        sharedRadianceCellsHandle, sharedRadianceLevelsHandle, sharedRadianceLevelCount, shape,
        bodies, frames, resolvedMagnetosphereForView, composeStageStarted,
        nearFieldWaterWeight, resolvedOceanForView, nearFieldSeaLevelMeters
    ] = context;

    const auto* logicalTarget = session.Viewports().Find(info.id);
    const auto width = view->Width();
    const auto height = view->Height();
    auto* color = &view->Color();

    const auto physicalBodyObject =
        logicalTarget->target.has_value()
            ? session.World().
                  Universe().
                  ObjectForBody(
                      logicalTarget->
                          target->body)
            : std::nullopt;

    const auto runtimeMaterialOverride =
        ResolveRuntimeBodyMaterialIfAssigned(
            content_,
            session.World().Objects(),
            physicalBodyObject);

    auto* lightingBaseRoughness =
        &view->SurfaceBaseRoughness();
    auto* lightingNormalMetallic =
        &view->SurfaceNormalMetallic();
    auto* lightingEmissionClass =
        &view->SurfaceEmissionClass();
    auto* lightingDepth =
        &view->Depth();

    const auto lightingView =
        view->Lighting();

    // Starlight reaches the ground through the atmosphere: tint the
    // direct term by the same transmittance LUT the sky and clouds use,
    // evaluated at the surface under the observer (sea level; terrain
    // relief changes it by a few percent). Without an atmosphere the
    // light stays untinted.
    math::Float3 stellarTransmittance{1.0F, 1.0F, 1.0F};
    if (terrainRuntime.has_value())
    {
        if (const auto atmosphereFound =
                atmospherePresentations_.find(info.id);
            atmosphereFound != atmospherePresentations_.end() &&
            atmosphereFound->second.staticLuts != nullptr)
        {
            const auto up =
                math::Normalize(terrainRuntime->observer.meters);
            const auto toStar =
                math::Normalize(math::Double3{
                    static_cast<f64>(studioDirectLight.directionBody.x),
                    static_cast<f64>(studioDirectLight.directionBody.y),
                    static_cast<f64>(studioDirectLight.directionBody.z)});
            const auto transmittance =
                celestial_atmosphere::SunTransmittanceAt(
                    atmosphereFound->second.parameters,
                    *atmosphereFound->second.staticLuts,
                    atmosphereFound->second.parameters.bottomRadiusMeters,
                    math::Dot(up, toStar));
            stellarTransmittance = {
                static_cast<f32>(transmittance.x),
                static_cast<f32>(transmittance.y),
                static_cast<f32>(transmittance.z)};
        }
    }

    const lighting::DirectionalLight
        directLight{
            .directionToLight =
                studioDirectLight.directionBody,
            .colorLinear = stellarTransmittance,
            .irradianceScale =
                studioDirectLight.irradianceScale
        };

    std::optional<scene::ObjectId>
        authoredLightRoot;

    if (logicalTarget->target.has_value() &&
        snapshot.hasWorld)
    {
        authoredLightRoot =
            session.World().
                Universe().
                ObjectForBody(
                    logicalTarget->
                        target->body);
    }

    const auto authoredLights =
        world_model::
            ResolveAuthoredLocalLights(
                session.World().Objects(),
                authoredLightRoot);

    std::vector<lighting::LocalLight>
        localLights;
    localLights.reserve(
        authoredLights.size());

    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    for (const auto& authored :
         authoredLights)
    {
        localLights.push_back({
            .type =
                authored.kind ==
                        world_model::
                            AuthoredLightKind::
                                Spot
                    ? lighting::
                        LocalLightType::
                            Spot
                    : lighting::
                        LocalLightType::
                            Point,
            .positionInFrameMeters =
                authored.positionMeters,
            .direction = {
                static_cast<f32>(
                    authored.direction.x),
                static_cast<f32>(
                    authored.direction.y),
                static_cast<f32>(
                    authored.direction.z)
            },
            .colorLinear = {
                static_cast<f32>(
                    authored.colorLinear.x),
                static_cast<f32>(
                    authored.colorLinear.y),
                static_cast<f32>(
                    authored.colorLinear.z)
            },
            .luminousFluxLumens =
                static_cast<f32>(
                    authored.
                        luminousFluxLumens),
            .rangeMeters =
                static_cast<f32>(
                    authored.rangeMeters),
            .innerConeRadians =
                static_cast<f32>(
                    authored.
                        innerConeDegrees *
                    kDegreesToRadians),
            .outerConeRadians =
                static_cast<f32>(
                    authored.
                        outerConeDegrees *
                    kDegreesToRadians),
            .stableId =
                authored.object.high ^
                authored.object.low
        });
    }

    const lighting::TiledLightGrid
        localLightGrid =
            lighting::BuildTiledLightGrid(
                localLights,
                lightingView,
                width,
                height);

    std::vector<lighting::GpuLocalLight>
        gpuLights;
    gpuLights.reserve(
        localLightGrid.lights.size());

    for (const auto& light :
         localLightGrid.lights)
    {
        gpuLights.push_back(
            lighting::
                EncodeGpuLocalLight(
                    light));
    }

    const u64 lightBufferBytes =
        std::max<u64>(
            sizeof(
                lighting::GpuLocalLight),
            static_cast<u64>(
                gpuLights.size()) *
                sizeof(
                    lighting::
                        GpuLocalLight));

    const u64 offsetBufferBytes =
        std::max<u64>(
            sizeof(u32),
            static_cast<u64>(
                localLightGrid.
                    offsets.size()) *
                sizeof(u32));

    const u64 indexBufferBytes =
        std::max<u64>(
            sizeof(u32),
            static_cast<u64>(
                localLightGrid.
                    lightIndices.size()) *
                sizeof(u32));

    const auto localLightsHandle =
        graph.CreateBuffer(
            prefix + ".LocalLights",
            {
                .sizeBytes =
                    lightBufferBytes,
                .usage =
                    rhi::BufferUsage::
                        Structured,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        ShaderResource
            });

    const auto localOffsetsHandle =
        graph.CreateBuffer(
            prefix + ".LocalLightOffsets",
            {
                .sizeBytes =
                    offsetBufferBytes,
                .usage =
                    rhi::BufferUsage::
                        Structured,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        ShaderResource
            });

    const auto localIndicesHandle =
        graph.CreateBuffer(
            prefix + ".LocalLightIndices",
            {
                .sizeBytes =
                    indexBufferBytes,
                .usage =
                    rhi::BufferUsage::
                        Structured,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        ShaderResource
            });

    const auto uploadBuffer =
        [](rhi::Buffer& buffer,
           const void* source,
           const u64 sourceBytes)
        {
            auto* destination =
                buffer.Map();

            std::memset(
                destination,
                0,
                static_cast<std::size_t>(
                    buffer.SizeBytes()));

            if (source != nullptr &&
                sourceBytes > 0U)
            {
                std::memcpy(
                    destination,
                    source,
                    static_cast<
                        std::size_t>(
                            sourceBytes));
            }

            buffer.Unmap();
        };

    uploadBuffer(
        graph.Buffer(
            localLightsHandle),
        gpuLights.empty()
            ? nullptr
            : gpuLights.data(),
        static_cast<u64>(
            gpuLights.size()) *
            sizeof(
                lighting::
                    GpuLocalLight));

    uploadBuffer(
        graph.Buffer(
            localOffsetsHandle),
        localLightGrid.offsets.empty()
            ? nullptr
            : localLightGrid.
                offsets.data(),
        static_cast<u64>(
            localLightGrid.
                offsets.size()) *
            sizeof(u32));

    uploadBuffer(
        graph.Buffer(
            localIndicesHandle),
        localLightGrid.
                lightIndices.empty()
            ? nullptr
            : localLightGrid.
                lightIndices.data(),
        static_cast<u64>(
            localLightGrid.
                lightIndices.size()) *
            sizeof(u32));

    auto& finalGather =
        finalGatherPresentations_[info.id];

    if (finalGather.radianceResidency == nullptr)
    {
        finalGather.radianceResidency =
            std::make_unique<
                lighting::RadianceClipmapResidency>(
                    lighting::RadianceClipmapConfig{});
        finalGather.radianceResidency->
            ConfigureGpuSnapshotFrameSlots(
                framesInFlight_);
    }

    recordComposeStage("render");
    // The near-field indirect stack (screen-space final gather,
    // radiance-cache fallback, hybrid and exact reflections) only has
    // data within the camera-centred radiance clipmap. When the
    // nearest visible surface of the target body lies beyond that
    // coverage -- a planet seen from orbit -- it can only inject
    // error: a convex body cannot illuminate itself, and screen-space
    // gathers/reflections across an entire disc produce spurious
    // night-side light and budget seams. Direct lighting remains.
    bool nearFieldIndirect = true;

    if (shape.has_value())
    {
        const auto& clipmapConfig =
            finalGather.radianceResidency->Config();
        const f64 clipmapHalfExtentMeters =
            lighting::RadianceCellSizeMeters(
                clipmapConfig,
                clipmapConfig.levelCount > 0U
                    ? clipmapConfig.levelCount - 1U
                    : 0U) *
            static_cast<f64>(
                clipmapConfig.cellsPerAxis) *
            0.5;
        const f64 cameraAltitudeMeters =
            math::Length(
                view->Camera().
                    localPositionMeters) -
            ReferenceRadiusForShape(
                *shape);

        nearFieldIndirect =
            cameraAltitudeMeters <=
            clipmapHalfExtentMeters;
    }
    // Bisecting switch: skip the indirect lighting (final gather and hybrid reflections).
    if (info.layers.bypassIndirectLighting)
    {
        nearFieldIndirect = false;
    }

    u64 radianceSourceRevision =
        session.World().Objects().Revision();
    if (terrainRuntime.has_value())
    {
        const auto& radianceTerrainSource =
            session.TerrainRuntime().TerrainSource(
                *terrainRuntime);

        radianceSourceRevision =
            CombineFingerprint(
                radianceSourceRevision,
                radianceTerrainSource.Revision());
    }

    static_cast<void>(
        finalGather.radianceResidency->ScrollTo(
            lightingView,
            lightingView.cameraPositionInFrameMeters,
            radianceSourceRevision,
            1.0F / 60.0F,
            false));
    recordComposeStage("gi_prepare");

    lighting::VisibilityRegistry
        radianceVisibility;

    if (const auto proxyFound =
            visibilityProxyPresentations_.find(
                info.id);
        proxyFound !=
                visibilityProxyPresentations_.end() &&
            proxyFound->second.provider !=
                nullptr)
    {
        radianceVisibility.Register(
            *proxyFound->second.provider);
    }

    std::unique_ptr<
        lighting::AnalyticBodyVisibilityProvider>
        analyticVisibility;

    std::unique_ptr<
        lighting::TerrainHeightfieldVisibilityProvider>
        terrainVisibility;

    if (bodies != nullptr &&
        frames != nullptr)
    {
        analyticVisibility =
            std::make_unique<
                lighting::
                    AnalyticBodyVisibilityProvider>(
                        *bodies,
                        *frames,
                        atTime);

        radianceVisibility.Register(
            *analyticVisibility);

        if (terrainRuntime.has_value())
        {
            const auto& radianceTerrainSource =
                session.TerrainRuntime().TerrainSource(
                    *terrainRuntime);

            terrainVisibility =
                std::make_unique<
                    lighting::
                        TerrainHeightfieldVisibilityProvider>(
                            terrainRuntime->body,
                            terrainRuntime->planet,
                            radianceTerrainSource,
                            *bodies,
                            *frames,
                            lighting::
                                TerrainVisibilityConfig{},
                            atTime);

            radianceVisibility.Register(
                *terrainVisibility);
        }
    }

    lighting::RadianceEstimateSettings
        radianceEstimateSettings{};
    // Do not invent a direction-independent fill light for a body
    // with no resolved atmospheric sky. The former scalar fallback
    // made night-side clipmap cells emit diffuse light even when
    // terrain correctly occluded the star.
    radianceEstimateSettings.
        ambientIrradianceScale = 0.0F;

    if (const auto atmosphereFound =
            atmospherePresentations_.find(
                info.id);
        atmosphereFound !=
                atmospherePresentations_.end() &&
            atmosphereFound->second.skyView !=
                nullptr)
    {
        const auto skyIrradiance =
            AtmosphereSkyIrradianceSummary(
                atmosphereFound->second.
                    skyView.get());

        if (skyIrradiance.x > 0.0F ||
            skyIrradiance.y > 0.0F ||
            skyIrradiance.z > 0.0F)
        {
            radianceEstimateSettings.
                ambientIrradianceScale =
                    0.0F;
            radianceEstimateSettings.
                skyIrradianceLinear =
                    skyIrradiance;
        }
    }

    std::vector<
        lighting::EmissiveVolumeSource>
        emissiveVolumes;

    if (resolvedMagnetosphereForView.has_value() &&
        shape.has_value())
    {
        const f64 referenceRadius =
            ReferenceRadiusForShape(
                *shape);

        const f64 outerRadius =
            referenceRadius +
            resolvedMagnetosphereForView->
                parameters.
                auroralMaximumAltitudeMeters;

        const f64 shellThickness =
            std::max(
                resolvedMagnetosphereForView->
                    parameters.
                    auroralMaximumAltitudeMeters -
                resolvedMagnetosphereForView->
                    parameters.
                    auroralMinimumAltitudeMeters,
                1.0);

        const auto& p =
            resolvedMagnetosphereForView->
                parameters;

        emissiveVolumes.push_back({
            .centerInFrameMeters =
                {0.0, 0.0, 0.0},
            .radiusMeters =
                static_cast<f32>(
                    outerRadius),
            .emissionLinear = {
                static_cast<f32>(
                    p.auroralColorLinear.x),
                static_cast<f32>(
                    p.auroralColorLinear.y),
                static_cast<f32>(
                    p.auroralColorLinear.z)
            },
            .intensityScale =
                static_cast<f32>(
                    p.auroralIntensity *
                    (0.22 +
                     0.78 * p.activity) *
                    std::clamp(
                        shellThickness /
                            std::max(
                                referenceRadius,
                                1.0),
                        0.002,
                        0.08)),
            .influenceRangeMeters =
                static_cast<f32>(
                    std::max(
                        referenceRadius *
                            0.45,
                        shellThickness *
                            18.0)),
            .stableId =
                resolvedMagnetosphereForView->
                    capability.high ^
                resolvedMagnetosphereForView->
                    capability.low
        });
    }

    const auto authoredVolumeEmission =
        ResolveAuthoredEmissiveVolumes(
            session.World().Objects());

    emissiveVolumes.insert(
        emissiveVolumes.end(),
        authoredVolumeEmission.begin(),
        authoredVolumeEmission.end());

    std::vector<
        lighting::DynamicEmissiveSourceState>
        dynamicEmissiveSources;
    dynamicEmissiveSources.reserve(
        emissiveVolumes.size());

    for (const auto& volume :
         emissiveVolumes)
    {
        u64 revision =
            CombineFingerprint(
                volume.stableId,
                QuantizedLightingFingerprintValue(
                    volume.intensityScale,
                    0.0025F));

        revision =
            CombineFingerprint(
                revision,
                QuantizedLightingFingerprintValue(
                    volume.emissionLinear.x,
                    0.0025F));
        revision =
            CombineFingerprint(
                revision,
                QuantizedLightingFingerprintValue(
                    volume.emissionLinear.y,
                    0.0025F));
        revision =
            CombineFingerprint(
                revision,
                QuantizedLightingFingerprintValue(
                    volume.emissionLinear.z,
                    0.0025F));
        revision =
            CombineFingerprint(
                revision,
                QuantizedLightingFingerprintValue(
                    volume.radiusMeters,
                    0.05F));
        revision =
            CombineFingerprint(
                revision,
                QuantizedLightingFingerprintValue(
                    volume.influenceRangeMeters,
                    0.05F));

        dynamicEmissiveSources.push_back({
            .stableId =
                volume.stableId,
            .contentRevision =
                revision,
            .centerInFrameMeters =
                volume.centerInFrameMeters,
            .sourceRadiusMeters =
                std::max<f64>(
                    volume.radiusMeters,
                    0.0F),
            .influenceRangeMeters =
                std::max<f64>(
                    volume.influenceRangeMeters,
                    0.0F)
        });
    }

    u64 emissionAuthorityFingerprint =
        0x4f52424954454d31ULL;

    for (const auto& sourceState :
         dynamicEmissiveSources)
    {
        emissionAuthorityFingerprint =
            CombineFingerprint(
                emissionAuthorityFingerprint,
                sourceState.stableId);
        emissionAuthorityFingerprint =
            CombineFingerprint(
                emissionAuthorityFingerprint,
                sourceState.contentRevision);
    }

    if (runtimeMaterialOverride.has_value())
    {
        const auto runtimeEmission =
            runtimeMaterialOverride->emissionRadiance;

        emissionAuthorityFingerprint =
            CombineFingerprint(
                emissionAuthorityFingerprint,
                QuantizedLightingFingerprintValue(
                    runtimeEmission.x,
                    0.0025F));
        emissionAuthorityFingerprint =
            CombineFingerprint(
                emissionAuthorityFingerprint,
                QuantizedLightingFingerprintValue(
                    runtimeEmission.y,
                    0.0025F));
        emissionAuthorityFingerprint =
            CombineFingerprint(
                emissionAuthorityFingerprint,
                QuantizedLightingFingerprintValue(
                    runtimeEmission.z,
                    0.0025F));
    }

    const auto emissiveInvalidations =
        finalGather.
            emissiveInvalidationTracker.
            Update(
                dynamicEmissiveSources);

    if (!emissiveInvalidations.empty())
    {
        lighting::ApplyEmissiveInvalidations(
            *finalGather.radianceResidency,
            emissiveInvalidations,
            radianceSourceRevision,
            8.0F);
    }

    u64 lightingFingerprint =
        0x4f52424954474931ULL;

    const auto addLightingValue =
        [&](const f32 value,
            const f32 quantum)
        {
            lightingFingerprint =
                CombineFingerprint(
                    lightingFingerprint,
                    QuantizedLightingFingerprintValue(
                        value,
                        quantum));
        };

    addLightingValue(
        directLight.directionToLight.x,
        0.005F);
    addLightingValue(
        directLight.directionToLight.y,
        0.005F);
    addLightingValue(
        directLight.directionToLight.z,
        0.005F);
    addLightingValue(
        directLight.irradianceScale,
        0.01F);
    addLightingValue(
        directLight.colorLinear.x,
        0.01F);
    addLightingValue(
        directLight.colorLinear.y,
        0.01F);
    addLightingValue(
        directLight.colorLinear.z,
        0.01F);

    addLightingValue(
        radianceEstimateSettings.
            skyIrradianceLinear.x,
        0.002F);
    addLightingValue(
        radianceEstimateSettings.
            skyIrradianceLinear.y,
        0.002F);
    addLightingValue(
        radianceEstimateSettings.
            skyIrradianceLinear.z,
        0.002F);

    for (const auto& light :
         localLightGrid.lights)
    {
        lightingFingerprint =
            CombineFingerprint(
                lightingFingerprint,
                light.stableId);

        addLightingValue(
            light.positionCameraRelativeMeters.x,
            0.05F);
        addLightingValue(
            light.positionCameraRelativeMeters.y,
            0.05F);
        addLightingValue(
            light.positionCameraRelativeMeters.z,
            0.05F);
        addLightingValue(
            light.luminousFluxLumens,
            5.0F);
    }

    const auto radianceRefreshReason =
        lighting::EvaluateRadianceRefresh(
            finalGather.lightingFingerprint,
            lightingFingerprint,
            finalGather.previousView.frame,
            lightingView.frame,
            finalGather.previousView.body,
            lightingView.body);

    const bool radianceCacheRefreshRequested =
        radianceRefreshReason !=
            lighting::RadianceRefreshReason::None;

    if (radianceCacheRefreshRequested)
    {
        finalGather.radianceResidency->
            RequestGlobalRefresh();
    }

    finalGather.lightingFingerprint =
        lightingFingerprint;

    if (auto transitionFound =
            transitionDiagnostics_.find(info.id);
        transitionFound !=
            transitionDiagnostics_.end())
    {
        auto& diagnostic =
            transitionFound->second;

        const std::array<
            lighting::RepresentationLightingAuthority,
            2U>
            authorities{{
                {
                    .representation =
                        diagnostic.representation,
                    .weight =
                        diagnostic.richerWeight,
                    .directLightingFingerprint =
                        lightingFingerprint,
                    .emissionAuthorityFingerprint =
                        emissionAuthorityFingerprint,
                    .radianceFrame =
                        lightingView.frame,
                    .radianceBody =
                        lightingView.body
                },
                {
                    .representation =
                        diagnostic.lowerFidelityNeighbor,
                    .weight =
                        diagnostic.lowerWeight,
                    .directLightingFingerprint =
                        lightingFingerprint,
                    .emissionAuthorityFingerprint =
                        emissionAuthorityFingerprint,
                    .radianceFrame =
                        lightingView.frame,
                    .radianceBody =
                        lightingView.body
                }
            }};

        const auto continuity =
            lighting::EvaluateLightingContinuity(
                authorities);

        diagnostic.directLightingFingerprint =
            lightingFingerprint;
        diagnostic.emissionAuthorityFingerprint =
            emissionAuthorityFingerprint;
        diagnostic.directLightingCoherent =
            continuity.directLightingCoherent;
        diagnostic.emissionAuthorityCoherent =
            continuity.emissionAuthorityCoherent;
        diagnostic.broadIndirectCoherent =
            continuity.radianceIdentityCoherent;
        diagnostic.continuityPassed =
            continuity.Passed();
        diagnostic.radianceCacheRefreshRequested =
            radianceCacheRefreshRequested;
    }

    recordComposeStage("gi_prepare");
    const auto updateListStarted =
        std::chrono::steady_clock::now();
    lighting::RadianceResidencyStats radianceStats;
    // The estimates run on the frame thread (terrain sampling dominates), so
    // the per-frame count is capped to what fits a time budget rather than a
    // fixed cell count: a loaded frame still hits its rate, and the cache
    // simply converges over more frames.
    constexpr f64 kRadianceEstimateBudgetMs = 1.2;
    const u32 radianceUpdateBudget =
        std::min(
            lightingPlan.radianceCacheUpdates,
            std::max<u32>(
                8U,
                static_cast<u32>(
                    kRadianceEstimateBudgetMs /
                    std::max(
                        finalGather.radianceEstimateMsPerCell,
                        0.005))));
    const auto radianceUpdates =
        finalGather.radianceResidency->BuildUpdateList(
            lightingView.cameraPositionInFrameMeters,
            radianceUpdateBudget,
            &radianceStats);
    if (cpuTimingRecorder)
    {
        cpuTimingRecorder(
            "gi_update_count",
            static_cast<double>(radianceUpdates.size()));
        const auto now = std::chrono::steady_clock::now();
        cpuTimingRecorder(
            "gi_update_list",
            std::chrono::duration<double, std::milli>(
                now - updateListStarted).count());
        composeStageStarted = now;
    }

    emissiveGiDiagnostics_.insert_or_assign(
        info.id,
        StudioEmissiveGiDiagnostics{
            .trackedSources =
                static_cast<u32>(
                    finalGather.
                        emissiveInvalidationTracker.
                        SourceCount()),
            .invalidationEventsThisFrame =
                static_cast<u32>(
                    emissiveInvalidations.size()),
            .dirtyRadianceCells =
                radianceStats.dirtyCells,
            .scheduledRadianceUpdates =
                static_cast<u32>(
                    radianceUpdates.size())
        });

    const auto estimateStarted = std::chrono::steady_clock::now();

    // Cells are independent and their inputs (terrain source, proxy BVH,
    // frame graph, lights) are read-only here, so they are estimated in
    // parallel; the results are committed in order on this thread.
    using RadianceEstimate = decltype(
        lighting::EstimateRadianceCellWithSky(
            radianceUpdates.front().key,
            finalGather.radianceResidency->Config(),
            lightingView,
            directLight,
            localLightGrid.lights,
            &radianceVisibility,
            radianceEstimateSettings,
            emissiveVolumes));
    std::vector<std::optional<RadianceEstimate>> estimates(
        radianceUpdates.size());
    std::vector<std::exception_ptr> estimateErrors(
        radianceUpdates.size());

    const auto estimateOne =
        [&](const std::size_t index)
        {
            try
            {
                // The sky is estimated as its own channel (full strength,
                // occluded by terrain and proxies) rather than folded into
                // the one-bounce L1; direct lighting applies it as fill.
                estimates[index] =
                    lighting::EstimateRadianceCellWithSky(
                        radianceUpdates[index].key,
                        finalGather.radianceResidency->Config(),
                        lightingView,
                        directLight,
                        localLightGrid.lights,
                        &radianceVisibility,
                        radianceEstimateSettings,
                        emissiveVolumes);
            }
            catch (...)
            {
                estimateErrors[index] = std::current_exception();
            }
        };

    if (radianceUpdates.size() >= 4U)
    {
        if (radianceEstimatePool_ == nullptr)
        {
            radianceEstimatePool_ =
                std::make_unique<jobs::JobSystem>(
                    jobs::PoolWorkerCount(
                        "ORBIT_RADIANCE_ESTIMATE_WORKERS", 3U, 2U),
                    "RadianceEstimate");
        }

        // Dynamic distribution: workers and this thread pull indices.
        std::atomic<std::size_t> nextIndex{0U};
        const auto drain =
            [&]
            {
                for (;;)
                {
                    const std::size_t index =
                        nextIndex.fetch_add(1U, std::memory_order_relaxed);
                    if (index >= radianceUpdates.size())
                    {
                        return;
                    }
                    estimateOne(index);
                }
            };

        jobs::JobGroup group;
        const std::size_t helpers = std::min<std::size_t>(
            radianceUpdates.size() - 1U, 8U);
        for (std::size_t helper = 0U; helper < helpers; ++helper)
        {
            radianceEstimatePool_->Submit(group, drain);
        }
        drain();
        radianceEstimatePool_->Wait(group);
    }
    else
    {
        for (std::size_t index = 0U; index < radianceUpdates.size(); ++index)
        {
            estimateOne(index);
        }
    }

    for (std::size_t index = 0U; index < radianceUpdates.size(); ++index)
    {
        if (estimateErrors[index] != nullptr)
        {
            std::rethrow_exception(estimateErrors[index]);
        }

        static_cast<void>(
            finalGather.radianceResidency->CommitUpdate(
                radianceUpdates[index].key,
                estimates[index]->indirect,
                estimates[index]->sky,
                radianceEstimateSettings.diffuseTransportScale,
                1U,
                radianceSourceRevision));
    }
    if (!radianceUpdates.empty())
    {
        const f64 elapsedMs =
            std::chrono::duration<f64, std::milli>(
                std::chrono::steady_clock::now() - estimateStarted).count();
        const f64 perCell =
            elapsedMs / static_cast<f64>(radianceUpdates.size());
        finalGather.radianceEstimateMsPerCell =
            0.8 * finalGather.radianceEstimateMsPerCell + 0.2 * perCell;
    }
    recordComposeStage("gi_estimate");

    const auto snapshotBuildStarted =
        std::chrono::steady_clock::now();
    const auto& radianceSnapshot =
        finalGather.radianceResidency->BuildGpuSnapshotRef(
            lightingView);
    const auto snapshotBuildFinished =
        std::chrono::steady_clock::now();

    const u64 radianceCellBytes =
        std::max<u64>(
            sizeof(lighting::GpuRadianceCell),
            static_cast<u64>(
                radianceSnapshot.cells.size()) *
                sizeof(lighting::GpuRadianceCell));

    const u64 radianceLevelBytes =
        std::max<u64>(
            sizeof(lighting::GpuRadianceLevelInfo),
            static_cast<u64>(
                radianceSnapshot.levels.size()) *
                sizeof(lighting::GpuRadianceLevelInfo));

    if (finalGather.radianceCellsBuffers.size() !=
        framesInFlight_)
    {
        finalGather.radianceCellsBuffers.resize(framesInFlight_);
    }
    if (finalGather.radianceLevelsBuffers.size() !=
        framesInFlight_)
    {
        finalGather.radianceLevelsBuffers.resize(framesInFlight_);
    }
    const u32 radianceFrameSlot = frameIndex % framesInFlight_;
    auto& radianceCellsBuffer =
        finalGather.radianceCellsBuffers[radianceFrameSlot];
    auto& radianceLevelsBuffer =
        finalGather.radianceLevelsBuffers[radianceFrameSlot];
    if (radianceCellsBuffer == nullptr ||
        radianceCellsBuffer->SizeBytes() != radianceCellBytes)
    {
        radianceCellsBuffer = device_->CreateBuffer({
            .sizeBytes = radianceCellBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });
        finalGather.radianceResidency->
            ConfigureGpuSnapshotFrameSlots(framesInFlight_);
    }
    if (radianceLevelsBuffer == nullptr ||
        radianceLevelsBuffer->SizeBytes() != radianceLevelBytes)
    {
        radianceLevelsBuffer = device_->CreateBuffer({
            .sizeBytes = radianceLevelBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });
    }

    const auto radianceCellsHandle = graph.ImportBuffer(
        prefix + ".RadianceCacheCells",
        *radianceCellsBuffer,
        rhi::ResourceState::ShaderResource);
    const auto radianceLevelsHandle = graph.ImportBuffer(
        prefix + ".RadianceCacheLevels",
        *radianceLevelsBuffer,
        rhi::ResourceState::ShaderResource);

    auto* radianceCellDestination =
        radianceCellsBuffer->Map();
    for (const auto cellIndex :
         finalGather.radianceResidency->
             GpuSnapshotDirtyCellIndices(radianceFrameSlot))
    {
        std::memcpy(
            radianceCellDestination +
                static_cast<std::size_t>(cellIndex) *
                    sizeof(lighting::GpuRadianceCell),
            radianceSnapshot.cells.data() + cellIndex,
            sizeof(lighting::GpuRadianceCell));
    }
    radianceCellsBuffer->Unmap();
    finalGather.radianceResidency->
        ClearGpuSnapshotDirtyCellIndices(radianceFrameSlot);

    auto* radianceLevelDestination =
        radianceLevelsBuffer->Map();
    std::memcpy(
        radianceLevelDestination,
        radianceSnapshot.levels.data(),
        static_cast<std::size_t>(
            radianceSnapshot.levels.size()) *
            sizeof(lighting::GpuRadianceLevelInfo));
    radianceLevelsBuffer->Unmap();
    const auto snapshotUploadFinished =
        std::chrono::steady_clock::now();
    if (cpuTimingRecorder)
    {
        cpuTimingRecorder(
            "gi_snapshot_build",
            std::chrono::duration<double, std::milli>(
                snapshotBuildFinished - snapshotBuildStarted).count());
        cpuTimingRecorder(
            "gi_snapshot_upload",
            std::chrono::duration<double, std::milli>(
                snapshotUploadFinished - snapshotBuildFinished).count());
    }
    recordComposeStage("gi_snapshot");

    const u32 radianceLevelCount =
        static_cast<u32>(
            radianceSnapshot.levels.size());

    sharedRadianceCellsHandle =
        radianceCellsHandle;
    sharedRadianceLevelsHandle =
        radianceLevelsHandle;
    sharedRadianceLevelCount =
        radianceLevelCount;

    if (runtimeMaterialOverride.has_value() &&
        presentation !=
            StudioViewportPresentation::BodyPreview)
    {
        const auto emission =
            runtimeMaterialOverride->
                emissionRadiance;

        if (emission.x > 0.0F ||
            emission.y > 0.0F ||
            emission.z > 0.0F)
        {
            graph.AddPass(
                prefix +
                    ".RuntimeMaterialEmissionOverride",
                {
                    {
                        .texture =
                            targets.
                                surfaceEmissionClass,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 lightingEmissionClass,
                 width,
                 height,
                 emission](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    materialEmissionSurfaceOverride_.
                        Apply(
                            commands,
                            *lightingEmissionClass,
                            width,
                            height,
                            emission);
                });
        }
    }

    rhi::Buffer* particleLightGridForDirect =
        volumeParticleRenderer_.ParticleLightGridReady()
            ? &volumeParticleRenderer_.ParticleLightGrid()
            : nullptr;

    // Cloud shadows: sun transmittance through the cloud layer at each
    // visible surface, at half resolution, read by the direct lighting.
    rhi::Texture* cloudShadowTexture = nullptr;
    std::optional<render_graph::TextureUse> cloudShadowUse;
    if (const auto cloudFound = cloudPresentations_.find(info.id);
        info.layers.clouds &&
        !info.layers.bypassCloudShadow &&
        cloudFound != cloudPresentations_.end() &&
        cloudFound->second.gpu != nullptr &&
        cloudFound->second.field != nullptr &&
        !cloudFound->second.field->layers.empty() &&
        logicalTarget->target.has_value() &&
        cloudFound->second.body == logicalTarget->target->body &&
        (info.layers.fullClipmap || !info.layers.macroGlobe) &&
        studioDirectLight.direct.has_value() &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        if (const auto shadowAtmosphere =
                atmospherePresentations_.find(info.id);
            shadowAtmosphere != atmospherePresentations_.end() &&
            shadowAtmosphere->second.body == logicalTarget->target->body)
        {
            const u32 shadowWidth = std::max(1U, (width + 1U) / 2U);
            const u32 shadowHeight = std::max(1U, (height + 1U) / 2U);
            auto& shadowTarget = cloudShadowTargets_[info.id];
            if (shadowTarget == nullptr ||
                shadowTarget->Width() != shadowWidth ||
                shadowTarget->Height() != shadowHeight)
            {
                shadowTarget = device_->CreateTexture({
                    .width = shadowWidth,
                    .height = shadowHeight,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource});
            }
            cloudShadowTexture = shadowTarget.get();
            const auto shadowHandle = graph.ImportTexture(
                prefix + ".CloudShadowTarget",
                *cloudShadowTexture,
                rhi::ResourceState::ShaderResource);

            auto* shadowGpu = cloudFound->second.gpu.get();
            const auto shadowLayer =
                cloudFound->second.field->layers.front().parameters;
            const f64 shadowReferenceRadius =
                shadowAtmosphere->second.parameters.bottomRadiusMeters;
            const auto shadowCamera = view->Camera();
            math::Float3 labShadowSun{};
            bool labShadowSunOverridden = false;
            const auto shadowLab = ResolveCloudLab(
                cloudLabAnchors_[info.id],
                info.layers.cloudLab,
                shadowCamera,
                shadowReferenceRadius,
                labShadowSun,
                labShadowSunOverridden);
            const celestial_atmosphere::AtmosphereRenderView shadowView{
                .cameraPositionMeters = shadowCamera.localPositionMeters,
                .forward = shadowCamera.forward,
                .up = shadowCamera.up,
                .verticalFovRadians = shadowCamera.verticalFovRadians,
                .nearPlaneMeters = shadowCamera.nearPlaneMeters,
                .farPlaneMeters = shadowCamera.farPlaneMeters,
                .sunDirection = studioDirectLight.directionBody,
                .irradianceScale = studioDirectLight.irradianceScale};

            graph.AddPass(
                prefix + ".CloudShadow",
                {
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = shadowHandle,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 lightingDepth,
                 shadowGpu,
                 shadowLayer,
                 shadowReferenceRadius,
                 shadowView,
                 shadowLab,
                 shadowTexture = cloudShadowTexture,
                 shadowWidth,
                 shadowHeight](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    cloudRenderer_.DrawShadow(
                        commands,
                        *lightingDepth,
                        *shadowGpu,
                        *shadowTexture,
                        shadowWidth,
                        shadowHeight,
                        shadowReferenceRadius,
                        shadowLayer,
                        shadowView,
                        shadowLab);
                });

            cloudShadowUse = render_graph::TextureUse{
                .texture = shadowHandle,
                .state = rhi::ResourceState::ShaderResource,
                .access = render_graph::Access::Read};
        }
    }

    // Authored Visibility Proxies as visible, lit geometry: drawn into
    // the surface buffer after the terrain so direct sun, the proxy sun
    // shadow, the final gather and the radiance cache all treat them as
    // ordinary rigid surfaces.
    if (!info.layers.bypassProxySurfaces &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        if (const auto surfaceFound =
                visibilityProxyPresentations_.find(info.id);
            surfaceFound != visibilityProxyPresentations_.end() &&
            surfaceFound->second.surfaces.Ready())
        {
            graph.AddPass(
                prefix + ".ProxySurfaces",
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
                [this,
                 geometry = &surfaceFound->second.surfaces,
                 color,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 width,
                 height,
                 lightingView](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    proxySurfaceRenderer_.Draw(
                        commands,
                        *geometry,
                        *color,
                        *lightingBaseRoughness,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        *lightingDepth,
                        width,
                        height,
                        lightingView);
                });
        }
    }

    // Imported Static Meshes (glTF/GLB) as lit, textured rigid surfaces
    // in the same surface buffer, after the terrain and proxies.
    if (!info.layers.bypassMeshSurfaces &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        if (const auto meshFound =
                staticMeshPresentations_.find(info.id);
            meshFound != staticMeshPresentations_.end() &&
            // Also while nothing is resident yet: the pass is what
            // pumps the library (uploads) that makes models resident.
            (!meshFound->second.instances.empty() ||
             meshFound->second.requested > 0U))
        {
            graph.AddPass(
                prefix + ".MeshSurfaces",
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
                [this,
                 instances = meshFound->second.instances,
                 sdfExtra = meshFound->second.sdfExtra,
                 color,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 width,
                 height,
                 lightingView](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    meshSurfaceRenderer_.Draw(
                        commands,
                        *meshLibrary_,
                        instances,
                        *color,
                        *lightingBaseRoughness,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        *lightingDepth,
                        width,
                        height,
                        lightingView,
                        sdfExtra.get());
                });

            // Light the mesh distance field's surface voxels (sun,
            // sky and multi-bounce) for the world-space GI fallback.
            {
                mesh_render::SdfLightingInput sdfLight;
                sdfLight.toSun = studioDirectLight.directionBody;
                sdfLight.sunIrradiance =
                    studioDirectLight.direct.has_value()
                        ? studioDirectLight.irradianceScale
                        : 0.0F;
                if (const auto skyFound =
                        atmospherePresentations_.find(info.id);
                    skyFound != atmospherePresentations_.end() &&
                    skyFound->second.skyView != nullptr)
                {
                    sdfLight.skyIrradiance =
                        AtmosphereSkyIrradianceSummary(
                            skyFound->second.skyView.get());
                }
                const auto& cameraFrame =
                    view->Lighting().cameraPositionInFrameMeters;
                const f64 radial = math::Length(cameraFrame);
                if (radial > 1.0)
                {
                    sdfLight.up = {
                        static_cast<f32>(cameraFrame.x / radial),
                        static_cast<f32>(cameraFrame.y / radial),
                        static_cast<f32>(cameraFrame.z / radial)};
                }
                sdfLight.frame =
                    antiAliasingPresentations_[info.id].frameCounter;

                graph.AddPass(
                    prefix + ".MeshSdfLighting",
                    {},
                    [this, sdfLight](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        meshSdfScene_.Light(commands, sdfLight);
                    });
            }
        }
    }

    // Sun visibility against authored Visibility Proxies: one hardware
    // ray per visible pixel toward the star, read by direct lighting.
    // Terrain and sky are not proxies, so only authored structures cast.
    rhi::Texture* proxySunShadowTexture = nullptr;
    std::optional<render_graph::TextureUse> proxySunShadowUse;

    // Imported Static Meshes cast sun shadows through a sun-space
    // depth map (works without ray-query hardware); it is folded into
    // the same sun-visibility texture the proxy pass writes.
    std::optional<mesh_render::MeshShadowFrame> meshShadowFrame;
    // The sun shadow map of this frame, for the glass caustics.
    rhi::Texture* glassSunShadowMap = nullptr;
    std::optional<render_graph::TextureHandle> glassSunShadowHandle;
    std::vector<mesh_render::MeshInstance> meshShadowInstances;
    if (!info.layers.bypassProxySunShadow &&
        !info.layers.bypassMeshSurfaces &&
        studioDirectLight.direct.has_value() &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        if (const auto meshShadowFound =
                staticMeshPresentations_.find(info.id);
            meshShadowFound != staticMeshPresentations_.end() &&
            (!meshShadowFound->second.instances.empty() ||
             !meshShadowFound->second.glass.empty()))
        {
            meshShadowInstances = meshShadowFound->second.instances;
            // Glass blocks the sun's direct beam like any body; the light it
            // lets through returns as caustics (GlassCaustics).
            for (const auto& glass : meshShadowFound->second.glass)
            {
                if (glass.model != nullptr && glass.castShadows)
                {
                    meshShadowInstances.push_back(
                        {.model = glass.model, .rows = glass.rows});
                }
            }
            meshShadowFrame = mesh_render::BuildMeshShadowFrame(
                meshShadowInstances,
                studioDirectLight.directionBody,
                view->Lighting().cameraPositionInFrameMeters);

            // The sun's real angular size (emitter radius over its
            // distance) sets how fast shadows soften with distance.
            if (meshShadowFrame.has_value() &&
                studioDirectLight.direct.has_value() &&
                bodies != nullptr &&
                studioDirectLight.direct->sourceDistanceMeters > 0.0)
            {
                if (const auto* emitter = bodies->FindBody(
                        studioDirectLight.direct->emitter);
                    emitter != nullptr)
                {
                    const f64 sine = std::clamp(
                        ReferenceRadiusForShape(emitter->shape) /
                            studioDirectLight.direct->sourceDistanceMeters,
                        0.0, 0.5);
                    meshShadowFrame->sunTanHalfAngle =
                        static_cast<f32>(std::tan(std::asin(sine)));
                }
            }
            if (meshShadowFrame.has_value())
            {
                // Scale for art direction / debugging (0 = hard shadows).
                meshShadowFrame->sunTanHalfAngle *=
                    info.layers.meshShadowSoftness;
            }
        }
    }

    if (!info.layers.bypassProxySunShadow &&
        studioDirectLight.direct.has_value() &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        const auto proxyFound =
            visibilityProxyPresentations_.find(info.id);
        const bool proxiesReady =
            proxySunShadowRenderer_.Supported() &&
            proxyFound != visibilityProxyPresentations_.end() &&
            proxyFound->second.hardware != nullptr &&
            proxyFound->second.hardware->Ready();

        if (proxiesReady || meshShadowFrame.has_value())
        {
            auto& shadowTarget = proxySunShadowTargets_[info.id];
            if (shadowTarget == nullptr ||
                shadowTarget->Width() != width ||
                shadowTarget->Height() != height)
            {
                shadowTarget = device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource,
                    .allowUnorderedAccess = true});
            }
            proxySunShadowTexture = shadowTarget.get();
            const auto proxyShadowHandle = graph.ImportTexture(
                prefix + ".ProxySunShadowTarget",
                *proxySunShadowTexture,
                rhi::ResourceState::ShaderResource);

            // Sky fill for proxy surfaces uses the same atmosphere
            // summary the radiance cache does; zero without a sky.
            math::Float3 proxySkyIrradiance{};
            if (const auto skyFound =
                    atmospherePresentations_.find(info.id);
                skyFound != atmospherePresentations_.end() &&
                skyFound->second.skyView != nullptr)
            {
                proxySkyIrradiance =
                    AtmosphereSkyIrradianceSummary(
                        skyFound->second.skyView.get());
            }

            if (proxiesReady)
            graph.AddPass(
                prefix + ".ProxySunShadow",
                {
                    {
                        .texture = targets.surfaceNormalMetallic,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.surfaceEmissionClass,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = proxyShadowHandle,
                        .state = rhi::ResourceState::UnorderedAccess,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 proxyHardware = proxiesReady
                     ? proxyFound->second.hardware.get()
                     : nullptr,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 proxySkyIrradiance,
                 shadowTexture = proxySunShadowTexture,
                 width,
                 height,
                 lightingView,
                 sunDirection = studioDirectLight.directionBody](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    proxySunShadowRenderer_.Draw(
                        commands,
                        *proxyHardware,
                        *shadowTexture,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        *lightingDepth,
                        width,
                        height,
                        lightingView,
                        sunDirection,
                        lighting::ProxySunShadowSettings{
                            .skyIrradiance = proxySkyIrradiance});
                });

            if (meshShadowFrame.has_value())
            {
                auto& mapTargets = meshShadowTargets_[info.id];
                const u32 mapSize = meshShadowFrame->mapSize;
                if (mapTargets.color == nullptr ||
                    mapTargets.color->Width() != mapSize)
                {
                    mapTargets.color = device_->CreateTexture({
                        .width = mapSize,
                        .height = mapSize,
                        .format = rhi::TextureFormat::R32_Float,
                        .initialState =
                            rhi::ResourceState::ShaderResource});
                    mapTargets.depth = device_->CreateTexture({
                        .width = mapSize,
                        .height = mapSize,
                        .format = rhi::TextureFormat::D32_Float,
                        .initialState =
                            rhi::ResourceState::DepthWrite});
                    mapTargets.skyColor = device_->CreateTexture({
                        .width = mesh_render::kMeshSkyAtlasWidth,
                        .height = mesh_render::kMeshSkyAtlasHeight,
                        .format = rhi::TextureFormat::R32_Float,
                        .initialState =
                            rhi::ResourceState::ShaderResource});
                    mapTargets.skyDepth = device_->CreateTexture({
                        .width = mesh_render::kMeshSkyAtlasWidth,
                        .height = mesh_render::kMeshSkyAtlasHeight,
                        .format = rhi::TextureFormat::D32_Float,
                        .initialState =
                            rhi::ResourceState::DepthWrite});
                    mapTargets.skySignature = 0U;
                    mapTargets.sunSignature = 0U;
                }

                // Signatures of the inputs the two maps are rendered from.
                u64 skySignature = 1469598103934665603ULL;
                u64 sunSignature = 1469598103934665603ULL;
                const auto mix =
                    [](u64& hash, const auto& value)
                    {
                        const auto* bytes =
                            reinterpret_cast<const unsigned char*>(&value);
                        for (std::size_t i = 0U; i < sizeof(value); ++i)
                        {
                            hash = (hash ^ bytes[i]) * 1099511628211ULL;
                        }
                    };
                for (const auto& instance : meshShadowInstances)
                {
                    for (u64* hash : {&skySignature, &sunSignature})
                    {
                        mix(*hash, instance.model);
                        mix(*hash, instance.rows);
                    }
                }
                const auto skyUp =
                    mesh_render::MeshLocalUp(
                        *meshShadowFrame,
                        view->Lighting().cameraPositionInFrameMeters);
                for (u64* hash : {&skySignature, &sunSignature})
                {
                    mix(*hash, meshShadowFrame->center);
                    mix(*hash, meshShadowFrame->radius);
                    mix(*hash, meshShadowFrame->right);
                    mix(*hash, meshShadowFrame->up);
                    mix(*hash, meshShadowFrame->mapSize);
                }
                mix(skySignature, skyUp);
                mix(sunSignature, meshShadowFrame->toSun);
                // Streaming textures can change alpha-cut casters without
                // touching any hashed input; refresh periodically.
                {
                    const u64 refreshEpoch =
                        antiAliasingPresentations_[info.id].frameCounter / 64U;
                    mix(skySignature, refreshEpoch);
                    mix(sunSignature, refreshEpoch);
                }
                const bool skyMapStale =
                    mapTargets.skySignature != skySignature;
                const bool sunMapStale =
                    mapTargets.sunSignature != sunSignature;
                mapTargets.skySignature = skySignature;
                mapTargets.sunSignature = sunSignature;

                const auto skyColorHandle = graph.ImportTexture(
                    prefix + ".MeshSkyAtlas",
                    *mapTargets.skyColor,
                    rhi::ResourceState::ShaderResource);
                const auto skyDepthHandle = graph.ImportTexture(
                    prefix + ".MeshSkyDepth",
                    *mapTargets.skyDepth,
                    rhi::ResourceState::DepthWrite);
                const auto meshLocalUp = mesh_render::MeshLocalUp(
                    *meshShadowFrame,
                    view->Lighting().cameraPositionInFrameMeters);

                if (skyMapStale)
                graph.AddPass(
                    prefix + ".MeshSkyMap",
                    {
                        {
                            .texture = skyColorHandle,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = skyDepthHandle,
                            .state = rhi::ResourceState::DepthWrite,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     instances = meshShadowInstances,
                     frame = *meshShadowFrame,
                     localUp = meshLocalUp,
                     colorAtlas = mapTargets.skyColor.get(),
                     depthAtlas = mapTargets.skyDepth.get()](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        meshShadowMapRenderer_.DrawSky(
                            commands,
                            *meshLibrary_,
                            instances,
                            frame,
                            localUp,
                            *colorAtlas,
                            *depthAtlas);
                    });

                const auto mapColorHandle = graph.ImportTexture(
                    prefix + ".MeshShadowMap",
                    *mapTargets.color,
                    rhi::ResourceState::ShaderResource);
                glassSunShadowMap = mapTargets.color.get();
                glassSunShadowHandle = mapColorHandle;
                const auto mapDepthHandle = graph.ImportTexture(
                    prefix + ".MeshShadowDepth",
                    *mapTargets.depth,
                    rhi::ResourceState::DepthWrite);

                if (sunMapStale)
                graph.AddPass(
                    prefix + ".MeshShadowMap",
                    {
                        {
                            .texture = mapColorHandle,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = mapDepthHandle,
                            .state = rhi::ResourceState::DepthWrite,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     instances = meshShadowInstances,
                     frame = *meshShadowFrame,
                     colorMap = mapTargets.color.get(),
                     depthMap = mapTargets.depth.get()](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        meshShadowMapRenderer_.Draw(
                            commands,
                            *meshLibrary_,
                            instances,
                            frame,
                            *colorMap,
                            *depthMap);
                    });

                graph.AddPass(
                    prefix + ".MeshSunShadow",
                    {
                        {
                            .texture = targets.surfaceNormalMetallic,
                            .state = rhi::ResourceState::ShaderResource,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = targets.surfaceEmissionClass,
                            .state = rhi::ResourceState::ShaderResource,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = targets.depth,
                            .state = rhi::ResourceState::DepthRead,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = mapColorHandle,
                            .state = rhi::ResourceState::ShaderResource,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = skyColorHandle,
                            .state = rhi::ResourceState::ShaderResource,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = proxyShadowHandle,
                            .state = rhi::ResourceState::UnorderedAccess,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     lightingNormalMetallic,
                     lightingEmissionClass,
                     lightingDepth,
                     frame = *meshShadowFrame,
                     colorMap = mapTargets.color.get(),
                     skyAtlas = mapTargets.skyColor.get(),
                     skyFill = mesh_render::MeshSkyFill{
                         .irradiance = proxySkyIrradiance,
                         .localUp = meshLocalUp},
                     shadowTexture = proxySunShadowTexture,
                     initialize = !proxiesReady,
                     temporalIndex =
                         info.layers.antiAliasing >= 2U
                             ? static_cast<u32>(
                                   antiAliasingPresentations_[info.id]
                                       .frameCounter)
                             : 0U,
                     width,
                     height,
                     lightingView](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        meshSunShadowRenderer_.Draw(
                            commands,
                            *shadowTexture,
                            *lightingNormalMetallic,
                            *lightingEmissionClass,
                            *lightingDepth,
                            *colorMap,
                            *skyAtlas,
                            width,
                            height,
                            lightingView,
                            frame,
                            skyFill,
                            initialize,
                            temporalIndex);
                    });
            }

            proxySunShadowUse = render_graph::TextureUse{
                .texture = proxyShadowHandle,
                .state = rhi::ResourceState::ShaderResource,
                .access = render_graph::Access::Read};
        }
    }

    const auto withCloudShadow =
        [&cloudShadowUse, &proxySunShadowUse](
            std::vector<render_graph::TextureUse> uses)
    {
        if (cloudShadowUse.has_value())
        {
            uses.push_back(*cloudShadowUse);
        }
        if (proxySunShadowUse.has_value())
        {
            uses.push_back(*proxySunShadowUse);
        }
        return uses;
    };

    // The sky-only radiance cache fill is read by direct lighting, so
    // the cache buffers join its inputs when it is active.
    const bool skyCacheFill =
        nearFieldIndirect &&
        radianceLevelCount > 0U &&
        !info.layers.bypassSkyCache;
    constexpr f32 kSkyCacheStrength = 1.0F;

    std::vector<render_graph::BufferUse> directBufferUses{
        {
            .buffer = localLightsHandle,
            .state = rhi::ResourceState::ShaderResource,
            .access = render_graph::Access::Read
        },
        {
            .buffer = localOffsetsHandle,
            .state = rhi::ResourceState::ShaderResource,
            .access = render_graph::Access::Read
        },
        {
            .buffer = localIndicesHandle,
            .state = rhi::ResourceState::ShaderResource,
            .access = render_graph::Access::Read
        }
    };
    if (skyCacheFill)
    {
        directBufferUses.push_back({
            .buffer = radianceCellsHandle,
            .state = rhi::ResourceState::ShaderResource,
            .access = render_graph::Access::Read});
        directBufferUses.push_back({
            .buffer = radianceLevelsHandle,
            .state = rhi::ResourceState::ShaderResource,
            .access = render_graph::Access::Read});
    }

    graph.AddPass(
        prefix + ".SharedDirectLighting",
        withCloudShadow({
            {
                .texture =
                    targets.surfaceBaseRoughness,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    targets.surfaceNormalMetallic,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    targets.surfaceEmissionClass,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture = targets.depth,
                .state =
                    rhi::ResourceState::
                        DepthRead,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture = targets.color,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        }),
        directBufferUses,
        [this,
         lightingBaseRoughness,
         lightingNormalMetallic,
         lightingEmissionClass,
         lightingDepth,
         color,
         width,
         height,
         lightingView,
         directLight,
         localLightGrid,
         localLightsHandle,
         localOffsetsHandle,
         localIndicesHandle,
         particleLightGridForDirect,
         cloudShadowTexture,
         proxySunShadowTexture,
         skyCacheFill,
         radianceCellsHandle,
         radianceLevelsHandle,
         radianceLevelCount,
         lightingTimestamps,
         frameIndex,
         nearFieldIndirect](
            rhi::CommandList& commands,
            const render_graph::Resources&
                resources)
        {
            if (lightingTimestamps != nullptr)
            {
                lightingTimestamps->BeginSection(
                    commands,
                    frameIndex,
                    lighting::LightingGpuSection::Direct);
            }

            directLightingRenderer_.Draw(
                commands,
                *lightingBaseRoughness,
                *lightingNormalMetallic,
                *lightingEmissionClass,
                *lightingDepth,
                resources.Buffer(
                    localLightsHandle),
                resources.Buffer(
                    localOffsetsHandle),
                resources.Buffer(
                    localIndicesHandle),
                *color,
                width,
                height,
                lightingView,
                directLight,
                localLightGrid,
                particleLightGridForDirect,
                cloudShadowTexture,
                proxySunShadowTexture,
                skyCacheFill
                    ? &resources.Buffer(radianceCellsHandle)
                    : nullptr,
                skyCacheFill
                    ? &resources.Buffer(radianceLevelsHandle)
                    : nullptr,
                skyCacheFill ? radianceLevelCount : 0U,
                kSkyCacheStrength,
                // The sky/ground fill floor tracks the stellar
                // irradiance actually reaching this body, so distant
                // planets are not washed flat by a fixed 3.5% floor.
                lighting::DirectLightingSettings{
                    // Direct sun and resolved atmospheric sky are
                    // the only non-emissive terms. A uniform ground
                    // fill leaks daylight onto the clipmap's night
                    // side, so ambient comes from the atmosphere/GI
                    // sky summary rather than a constant floor.
                    .ambientIrradianceScale =
                        0.0F
                });

            if (lightingTimestamps != nullptr)
            {
                lightingTimestamps->EndSection(
                    commands,
                    frameIndex,
                    lighting::LightingGpuSection::Direct);
            }
        });

    if (nearFieldIndirect)
    {
        if (finalGather.width != width ||
            finalGather.height != height ||
            finalGather.indirectA == nullptr ||
            finalGather.indirectB == nullptr ||
            finalGather.metaA == nullptr ||
            finalGather.metaB == nullptr ||
            finalGather.scratch == nullptr)
        {
            const auto createGatherTexture =
                [this, width, height]()
                {
                    return device_->CreateTexture({
                        .width = width,
                        .height = height,
                        .format =
                            rhi::TextureFormat::
                                RGBA16_Float,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource,
                        .allowUnorderedAccess =
                            true
                    });
                };

            finalGather.width = width;
            finalGather.height = height;
            finalGather.writeA = true;
            finalGather.hasHistory = false;
            finalGather.previousView = {};

            finalGather.indirectA =
                createGatherTexture();
            finalGather.indirectB =
                createGatherTexture();
            finalGather.metaA =
                createGatherTexture();
            finalGather.metaB =
                createGatherTexture();
            finalGather.scratch =
                createGatherTexture();
        }

        auto* currentIndirect =
            finalGather.writeA
                ? finalGather.indirectA.get()
                : finalGather.indirectB.get();

        auto* previousIndirect =
            finalGather.writeA
                ? finalGather.indirectB.get()
                : finalGather.indirectA.get();

        auto* currentMeta =
            finalGather.writeA
                ? finalGather.metaA.get()
                : finalGather.metaB.get();

        auto* previousMeta =
            finalGather.writeA
                ? finalGather.metaB.get()
                : finalGather.metaA.get();

        auto* gatherScratch =
            finalGather.scratch.get();

        const bool historyCompatible =
            finalGather.hasHistory &&
            lighting::
                CanReuseFinalGatherHistory(
                    finalGather.previousView,
                    lightingView);

        const auto currentIndirectHandle =
            graph.ImportTexture(
                prefix +
                    ".FinalGather.CurrentIndirect",
                *currentIndirect,
                rhi::ResourceState::
                    ShaderResource);

        const auto previousIndirectHandle =
            graph.ImportTexture(
                prefix +
                    ".FinalGather.PreviousIndirect",
                *previousIndirect,
                rhi::ResourceState::
                    ShaderResource);

        const auto currentMetaHandle =
            graph.ImportTexture(
                prefix +
                    ".FinalGather.CurrentMeta",
                *currentMeta,
                rhi::ResourceState::
                    ShaderResource);

        const auto previousMetaHandle =
            graph.ImportTexture(
                prefix +
                    ".FinalGather.PreviousMeta",
                *previousMeta,
                rhi::ResourceState::
                    ShaderResource);

        const auto gatherScratchHandle =
            graph.ImportTexture(
                prefix +
                    ".FinalGather.Scratch",
                *gatherScratch,
                rhi::ResourceState::
                    ShaderResource);

        lighting::
            ScreenSpaceFinalGatherSettings
                gatherSettings;

        gatherSettings.intensity = info.layers.giIntensity;
        gatherSettings.stepsPerRay =
            std::clamp(
                static_cast<u32>(
                    std::lround(
                        2.0F +
                        8.0F *
                            std::clamp(
                                lightingPlan.giScale,
                                0.0F,
                                1.0F))),
                2U,
                10U);

        // Pass-need probe (see FinalGatherPresentation): read the slot the
        // gather wrote framesInFlight frames ago, update the holds, rearm it.
        if (finalGather.needStatsBuffers.size() != framesInFlight_)
        {
            finalGather.needStatsBuffers.clear();
            finalGather.needStatsWritten.assign(framesInFlight_, 0U);
            for (u32 slot = 0U; slot < framesInFlight_; ++slot)
            {
                auto buffer = device_->CreateBuffer({
                    .sizeBytes = 16U,
                    .usage = rhi::BufferUsage::Structured,
                    .memory = rhi::MemoryUsage::HostVisible,
                    .initialState = rhi::ResourceState::ShaderResource});
                std::memset(buffer->Map(), 0, 16U);
                buffer->Unmap();
                finalGather.needStatsBuffers.push_back(std::move(buffer));
            }
        }
        const u32 needSlot = frameIndex % framesInFlight_;
        {
            constexpr u32 kNeedHoldFrames = 90U;
            constexpr f64 kSmoothFraction = 0.002;
            constexpr f64 kUncoveredFraction = 0.01;
            auto& needBuffer = *finalGather.needStatsBuffers[needSlot];
            auto* counters = reinterpret_cast<u32*>(needBuffer.Map());
            if (finalGather.needStatsWritten[needSlot] != 0U)
            {
                const f64 surfaces = static_cast<f64>(counters[0]);
                if (surfaces > 256.0)
                {
                    if (static_cast<f64>(counters[1]) >
                        kSmoothFraction * surfaces)
                    {
                        finalGather.reflectionsHold = kNeedHoldFrames;
                    }
                    if (static_cast<f64>(counters[2]) >
                        kUncoveredFraction * surfaces)
                    {
                        finalGather.cacheFallbackHold = kNeedHoldFrames;
                    }
                    if (static_cast<f64>(counters[3]) >
                        kSmoothFraction * surfaces)
                    {
                        finalGather.exactReflectionHold = kNeedHoldFrames;
                    }
                }
                else
                {
                    // Nothing to judge from (sky only, or a tiny view).
                    finalGather.reflectionsHold = kNeedHoldFrames;
                    finalGather.cacheFallbackHold = kNeedHoldFrames;
                    finalGather.exactReflectionHold = kNeedHoldFrames;
                }
            }
            std::memset(counters, 0, 16U);
            needBuffer.Unmap();
            finalGather.needStatsWritten[needSlot] = 1U;
            if (finalGather.reflectionsHold > 0U)
            {
                --finalGather.reflectionsHold;
            }
            if (finalGather.cacheFallbackHold > 0U)
            {
                --finalGather.cacheFallbackHold;
            }
            if (finalGather.exactReflectionHold > 0U)
            {
                --finalGather.exactReflectionHold;
            }
        }
        auto* needStatsBuffer = finalGather.needStatsBuffers[needSlot].get();
        const auto needStatsHandle = graph.ImportBuffer(
            prefix + ".NeedStats",
            *needStatsBuffer,
            rhi::ResourceState::ShaderResource);

        graph.AddPass(
            prefix + ".ScreenSpaceFinalGather",
            {
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        targets.
                            surfaceBaseRoughness,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        targets.
                            surfaceNormalMetallic,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        targets.
                            surfaceEmissionClass,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = targets.depth,
                    .state =
                        rhi::ResourceState::
                            DepthRead,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        previousIndirectHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        previousMetaHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        currentIndirectHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture =
                        currentMetaHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            {
                {
                    .buffer = needStatsHandle,
                    .state = rhi::ResourceState::UnorderedAccess,
                    .access = render_graph::Access::Write
                }
            },
            [this,
             needStatsBuffer,
             color,
             lightingBaseRoughness,
             lightingNormalMetallic,
             lightingEmissionClass,
             lightingDepth,
             previousIndirect,
             previousMeta,
             currentIndirect,
             currentMeta,
             width,
             height,
             lightingView,
             historyCompatible,
             gatherSettings,
             particleLightGridForDirect,
             lightingTimestamps,
             frameIndex,
             useSdfGi = !info.layers.bypassSdfGi](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                // World-space fallback: the merged mesh distance field.
                lighting::SdfGatherInput sdfGather;
                if (const auto& volume = meshSdfScene_.Volume();
                    useSdfGi && volume.ready &&
                    volume.radiance != nullptr &&
                    volume.distanceCorners != nullptr)
                {
                    sdfGather.distance = volume.distanceCorners;
                    sdfGather.albedo = volume.albedo;
                    sdfGather.normal = volume.normal;
                    sdfGather.radiance = volume.radiance;
                    sdfGather.originInFrameMeters =
                        volume.originInFrameMeters;
                    sdfGather.voxelSize = volume.voxelSize;
                    sdfGather.dimensions = volume.dimensions;
                }

                if (lightingTimestamps != nullptr)
                {
                    lightingTimestamps->
                        BeginSection(
                            commands,
                            frameIndex,
                            lighting::
                                LightingGpuSection::
                                    Gi);
                }

                finalGatherRenderer_.Gather(
                    commands,
                    *color,
                    *lightingBaseRoughness,
                    *lightingNormalMetallic,
                    *lightingEmissionClass,
                    *lightingDepth,
                    *previousIndirect,
                    *previousMeta,
                    *currentIndirect,
                    *currentMeta,
                    width,
                    height,
                    lightingView,
                    historyCompatible,
                    particleLightGridForDirect,
                    gatherSettings,
                    sdfGather.distance != nullptr ? &sdfGather
                                                  : nullptr,
                    needStatsBuffer);
            });

        // The fallback only fills pixels the gather could not cover; skip it
        // while (almost) none are.
        if (radianceLevelCount > 0U &&
            !info.layers.bypassRadianceCache &&
            finalGather.cacheFallbackHold > 0U)
        {
            graph.AddPass(
                prefix + ".RadianceCacheFallback",
                {
                    {
                        .texture =
                            currentIndirectHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .texture =
                            targets.
                                surfaceBaseRoughness,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceNormalMetallic,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceEmissionClass,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.depth,
                        .state =
                            rhi::ResourceState::
                                DepthRead,
                        .access =
                            render_graph::Access::
                                Read
                    }
                },
                {
                    {
                        .buffer =
                            radianceCellsHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            radianceLevelsHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    }
                },
                [this,
                 currentIndirect,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 radianceCellsHandle,
                 radianceLevelsHandle,
                 radianceLevelCount,
                 width,
                 height,
                 lightingView](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    radianceCacheSampler_.
                        ResolveFallback(
                            commands,
                            *currentIndirect,
                            *lightingBaseRoughness,
                            *lightingNormalMetallic,
                            *lightingEmissionClass,
                            *lightingDepth,
                            resources.Buffer(
                                radianceCellsHandle),
                            resources.Buffer(
                                radianceLevelsHandle),
                            radianceLevelCount,
                            width,
                            height,
                            lightingView);
                });
        }

        graph.AddPass(
            prefix + ".FinalGatherCombine",
            {
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        currentIndirectHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        gatherScratchHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             color,
             currentIndirect,
             gatherScratch,
             width,
             height,
             coverageView = info.layers.indirectCoverageView,
             giOnlyView = info.layers.giOnlyView](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                finalGatherRenderer_.Combine(
                    commands,
                    *color,
                    *currentIndirect,
                    *gatherScratch,
                    width,
                    height,
                    1.0F,
                    coverageView,
                    giOnlyView);
            });

        graph.AddPass(
            prefix + ".FinalGatherCopyBack",
            {
                {
                    .texture =
                        gatherScratchHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
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
             gatherScratch,
             color,
             width,
             height,
             lightingTimestamps,
             frameIndex](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                debugComposite_.Draw(
                    commands,
                    *gatherScratch,
                    *color,
                    width,
                    height);

                if (lightingTimestamps != nullptr)
                {
                    lightingTimestamps->
                        EndSection(
                            commands,
                            frameIndex,
                            lighting::
                                LightingGpuSection::
                                    Gi);
                }
            });

        // Reflections only act on smooth or metallic pixels; skip the whole
        // chain (hybrid resolve, exact compact/trace/resolve, copy-back)
        // while none are on screen.
        if (radianceLevelCount > 0U &&
            !info.layers.bypassHybridReflections &&
            finalGather.reflectionsHold > 0U)
        {
            lighting::HardwareRayQueryVisibilityBatch*
                exactReflectionHardware = nullptr;

            if (const auto proxyFound =
                    visibilityProxyPresentations_.find(
                        info.id);
                proxyFound !=
                        visibilityProxyPresentations_.end() &&
                    proxyFound->second.hardware != nullptr &&
                    proxyFound->second.hardware->Ready() &&
                    finalGather.exactReflectionHold > 0U)
            {
                exactReflectionHardware =
                    proxyFound->second.hardware.get();
            }

            const u32 maximumExactReflectionQueries =
                std::min(
                    lightingPlan.exactVisibilityQueries,
                    lightingPlan.reflectionQueries);
            const auto exactSceneOrigin =
                exactReflectionHardware != nullptr
                    ? exactReflectionHardware->
                          GpuOriginInFrameMeters()
                    : lightingView.
                          gpuOriginInFrameMeters;

            const math::Float3
                currentToExactSceneOrigin{
                    static_cast<f32>(
                        lightingView.
                            gpuOriginInFrameMeters.x -
                        exactSceneOrigin.x),
                    static_cast<f32>(
                        lightingView.
                            gpuOriginInFrameMeters.y -
                        exactSceneOrigin.y),
                    static_cast<f32>(
                        lightingView.
                            gpuOriginInFrameMeters.z -
                        exactSceneOrigin.z)
                };

            const math::Float3
                exactSceneToCurrentOrigin{
                    -currentToExactSceneOrigin.x,
                    -currentToExactSceneOrigin.y,
                    -currentToExactSceneOrigin.z
                };

            render_graph::BufferHandle
                exactReflectionQueriesHandle{};
            render_graph::BufferHandle
                exactReflectionResultsHandle{};
            render_graph::BufferHandle
                exactReflectionPixelMapHandle{};
            render_graph::BufferHandle
                exactReflectionCounterHandle{};

            if (exactReflectionHardware != nullptr &&
                maximumExactReflectionQueries > 0U)
            {
                exactReflectionQueriesHandle =
                    graph.CreateBuffer(
                        prefix +
                            ".ExactReflectionQueries",
                        {
                            .sizeBytes =
                                static_cast<u64>(
                                    maximumExactReflectionQueries) *
                                sizeof(
                                    lighting::
                                        GpuVisibilityQuery),
                            .usage =
                                rhi::BufferUsage::
                                    Structured,
                            .memory =
                                rhi::MemoryUsage::
                                    HostVisible,
                            .initialState =
                                rhi::ResourceState::
                                    ShaderResource
                        });

                exactReflectionResultsHandle =
                    graph.CreateBuffer(
                        prefix +
                            ".ExactReflectionResults",
                        {
                            .sizeBytes =
                                static_cast<u64>(
                                    maximumExactReflectionQueries) *
                                sizeof(
                                    lighting::
                                        GpuVisibilityResult),
                            .usage =
                                rhi::BufferUsage::
                                    Structured,
                            .memory =
                                rhi::MemoryUsage::
                                    HostVisible,
                            .initialState =
                                rhi::ResourceState::
                                    ShaderResource
                        });

                exactReflectionPixelMapHandle =
                    graph.CreateBuffer(
                        prefix +
                            ".ExactReflectionPixelMap",
                        {
                            .sizeBytes =
                                static_cast<u64>(
                                    maximumExactReflectionQueries) *
                                sizeof(u32),
                            .usage =
                                rhi::BufferUsage::
                                    Structured,
                            .memory =
                                rhi::MemoryUsage::
                                    HostVisible,
                            .initialState =
                                rhi::ResourceState::
                                    ShaderResource
                        });

                exactReflectionCounterHandle =
                    graph.CreateBuffer(
                        prefix +
                            ".ExactReflectionCounter",
                        {
                            .sizeBytes = sizeof(u32),
                            .usage =
                                rhi::BufferUsage::
                                    Structured,
                            .memory =
                                rhi::MemoryUsage::
                                    HostVisible,
                            .initialState =
                                rhi::ResourceState::
                                    ShaderResource
                        });

                std::vector<
                    lighting::GpuVisibilityQuery>
                    safeQueries(
                        maximumExactReflectionQueries);

                for (auto& safeQuery : safeQueries)
                {
                    safeQuery.originMinimumDistance.w =
                        0.03F;
                    safeQuery.directionMaximumDistance =
                        {0.0F, 0.0F, 1.0F, 0.03F};
                    safeQuery.requirements = {
                        std::numeric_limits<f32>::
                            infinity(),
                        0.0F,
                        0.0F,
                        std::bit_cast<f32>(3U)
                    };
                }

                uploadBuffer(
                    graph.Buffer(
                        exactReflectionQueriesHandle),
                    safeQueries.data(),
                    static_cast<u64>(
                        safeQueries.size()) *
                        sizeof(
                            lighting::
                                GpuVisibilityQuery));

                std::vector<u32>
                    emptyPixelMap(
                        maximumExactReflectionQueries,
                        0xFFFF'FFFFU);

                uploadBuffer(
                    graph.Buffer(
                        exactReflectionPixelMapHandle),
                    emptyPixelMap.data(),
                    static_cast<u64>(
                        emptyPixelMap.size()) *
                        sizeof(u32));

                const u32 zero = 0U;
                uploadBuffer(
                    graph.Buffer(
                        exactReflectionCounterHandle),
                    &zero,
                    sizeof(zero));

                std::vector<
                    lighting::GpuVisibilityResult>
                    emptyResults(
                        maximumExactReflectionQueries);

                uploadBuffer(
                    graph.Buffer(
                        exactReflectionResultsHandle),
                    emptyResults.data(),
                    static_cast<u64>(
                        emptyResults.size()) *
                        sizeof(
                            lighting::
                                GpuVisibilityResult));
            }

            graph.AddPass(
                prefix + ".HybridReflections",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceBaseRoughness,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceNormalMetallic,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceEmissionClass,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::
                                DepthRead,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            gatherScratchHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                {
                    {
                        .buffer =
                            radianceCellsHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            radianceLevelsHandle,
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
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 gatherScratch,
                 radianceCellsHandle,
                 radianceLevelsHandle,
                 radianceLevelCount,
                 width,
                 height,
                 lightingView,
                 lightingPlan,
                 lightingTimestamps,
                 frameIndex,
                 useSdfGi = !info.layers.bypassSdfGi](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    lighting::SdfGatherInput sdfReflection;
                    if (const auto& volume = meshSdfScene_.Volume();
                        useSdfGi && volume.ready &&
                        volume.radiance != nullptr &&
                        volume.distanceCorners != nullptr)
                    {
                        sdfReflection.distance = volume.distanceCorners;
                        sdfReflection.albedo = volume.albedo;
                        sdfReflection.normal = volume.normal;
                        sdfReflection.radiance = volume.radiance;
                        sdfReflection.originInFrameMeters =
                            volume.originInFrameMeters;
                        sdfReflection.voxelSize = volume.voxelSize;
                        sdfReflection.dimensions = volume.dimensions;
                    }

                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->
                            BeginSection(
                                commands,
                                frameIndex,
                                lighting::
                                    LightingGpuSection::
                                        Reflections);
                    }

                    hybridReflectionRenderer_.
                        Resolve(
                            commands,
                            *color,
                            *lightingBaseRoughness,
                            *lightingNormalMetallic,
                            *lightingEmissionClass,
                            *lightingDepth,
                            resources.Buffer(
                                radianceCellsHandle),
                            resources.Buffer(
                                radianceLevelsHandle),
                            radianceLevelCount,
                            *gatherScratch,
                            width,
                            height,
                            lightingView,
                            lightingPlan.
                                reflectionScale,
                            {},
                            sdfReflection.distance != nullptr
                                ? &sdfReflection
                                : nullptr);
                });

            if (exactReflectionHardware != nullptr &&
                maximumExactReflectionQueries > 0U)
            {
                graph.AddPass(
                    prefix +
                        ".ExactReflectionCompact",
                    {
                        {
                            .texture =
                                targets.
                                    surfaceBaseRoughness,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.
                                    surfaceNormalMetallic,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.depth,
                            .state =
                                rhi::ResourceState::
                                    DepthRead,
                            .access =
                                render_graph::Access::
                                    Read
                        }
                    },
                    {
                        {
                            .buffer =
                                exactReflectionQueriesHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .buffer =
                                exactReflectionPixelMapHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .buffer =
                                exactReflectionCounterHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     lightingBaseRoughness,
                     lightingNormalMetallic,
                     lightingDepth,
                     exactReflectionQueriesHandle,
                     exactReflectionPixelMapHandle,
                     exactReflectionCounterHandle,
                     maximumExactReflectionQueries,
                     width,
                     height,
                     lightingView,
                     currentToExactSceneOrigin,
                     lightingPlan](
                        rhi::CommandList& commands,
                        const render_graph::Resources&
                            resources)
                    {
                        const u32 screenSteps =
                            std::clamp(
                                static_cast<u32>(
                                    std::lround(
                                        4.0F +
                                        12.0F *
                                            lightingPlan.
                                                reflectionScale)),
                                4U,
                                16U);

                        exactReflectionQueryRenderer_.
                            BuildQueries(
                                commands,
                                *lightingBaseRoughness,
                                *lightingNormalMetallic,
                                *lightingDepth,
                                resources.Buffer(
                                    exactReflectionQueriesHandle),
                                resources.Buffer(
                                    exactReflectionPixelMapHandle),
                                resources.Buffer(
                                    exactReflectionCounterHandle),
                                maximumExactReflectionQueries,
                                width,
                                height,
                                lightingView,
                                currentToExactSceneOrigin,
                                0.08F,
                                40.0F,
                                0.12F,
                                screenSteps);
                    });

                graph.AddPass(
                    prefix +
                        ".ExactReflectionTrace",
                    {},
                    {
                        {
                            .buffer =
                                exactReflectionQueriesHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .buffer =
                                exactReflectionResultsHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [exactReflectionHardware,
                     exactReflectionQueriesHandle,
                     exactReflectionResultsHandle,
                     maximumExactReflectionQueries](
                        rhi::CommandList& commands,
                        const render_graph::Resources&
                            resources)
                    {
                        exactReflectionHardware->
                            Dispatch(
                                commands,
                                resources.Buffer(
                                    exactReflectionQueriesHandle),
                                resources.Buffer(
                                    exactReflectionResultsHandle),
                                maximumExactReflectionQueries);
                    });

                graph.AddPass(
                    prefix +
                        ".ExactReflectionResolve",
                    {
                        {
                            .texture =
                                gatherScratchHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture =
                                targets.
                                    surfaceBaseRoughness,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.
                                    surfaceNormalMetallic,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.depth,
                            .state =
                                rhi::ResourceState::
                                    DepthRead,
                            .access =
                                render_graph::Access::
                                    Read
                        }
                    },
                    {
                        {
                            .buffer =
                                exactReflectionResultsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .buffer =
                                exactReflectionPixelMapHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .buffer =
                                radianceCellsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .buffer =
                                radianceLevelsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        }
                    },
                    [this,
                     gatherScratch,
                     lightingBaseRoughness,
                     lightingNormalMetallic,
                     lightingDepth,
                     exactReflectionResultsHandle,
                     exactReflectionPixelMapHandle,
                     radianceCellsHandle,
                     radianceLevelsHandle,
                     radianceLevelCount,
                     maximumExactReflectionQueries,
                     width,
                     height,
                     lightingView,
                     exactSceneToCurrentOrigin](
                        rhi::CommandList& commands,
                        const render_graph::Resources&
                            resources)
                    {
                        exactReflectionQueryRenderer_.
                            ResolveResults(
                                commands,
                                *gatherScratch,
                                *lightingBaseRoughness,
                                *lightingNormalMetallic,
                                *lightingDepth,
                                resources.Buffer(
                                    exactReflectionResultsHandle),
                                resources.Buffer(
                                    exactReflectionPixelMapHandle),
                                resources.Buffer(
                                    radianceCellsHandle),
                                resources.Buffer(
                                    radianceLevelsHandle),
                                radianceLevelCount,
                                maximumExactReflectionQueries,
                                width,
                                height,
                                lightingView,
                                exactSceneToCurrentOrigin);
                    });
            }

            graph.AddPass(
                prefix + ".HybridReflectionsCopyBack",
                {
                    {
                        .texture =
                            gatherScratchHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
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
                 gatherScratch,
                 color,
                 width,
                 height,
                 lightingTimestamps,
                 frameIndex](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    debugComposite_.Draw(
                        commands,
                        *gatherScratch,
                        *color,
                        width,
                        height);

                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->
                            EndSection(
                                commands,
                                frameIndex,
                                lighting::
                                    LightingGpuSection::
                                        Reflections);
                    }
                });
        }

        graph.AddPass(
            prefix + ".FinalGatherRestoreHistory",
            {
                {
                    .texture =
                        currentIndirectHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        currentMetaHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                }
            },
            [](
                rhi::CommandList&,
                const render_graph::Resources&)
            {
            });

        finalGather.previousView =
            lightingView;
        finalGather.hasHistory = true;
        finalGather.writeA =
            !finalGather.writeA;
    }
    else
    {
        // Resume from a clean history when the view returns to the
        // near field instead of reprojecting stale indirect light.
        finalGather.hasHistory = false;
        finalGather.previousView = {};
    }

    // Near-field standing water is its own object: a flat surface at
    // sea level, alpha-blended over the lit scene, after lighting, GI
    // and reflections and before the atmosphere. The terrain pass drew
    // the true bed; this pass tests against its depth (the shoreline)
    // and measures the water column with it.
    if (presentation ==
            StudioViewportPresentation::ProductionTerrain &&
        nearFieldWaterWeight > 0.0 &&
        !info.layers.bypassNearFieldWater &&
        resolvedOceanForView.has_value() &&
        terrainRuntime.has_value())
    {
        if (const auto waterTerrain =
                terrainPresentations_.find(info.id);
            waterTerrain != terrainPresentations_.end() &&
            waterTerrain->second.renderer != nullptr)
        {
            terrain_render::TerrainWaterOptics water{};
            const auto& optical = resolvedOceanForView->optical;
            water.absorptionPerMeter = {
                static_cast<f32>(optical.absorptionPerMeter.x),
                static_cast<f32>(optical.absorptionPerMeter.y),
                static_cast<f32>(optical.absorptionPerMeter.z)};
            water.refractiveIndex =
                static_cast<f32>(optical.refractiveIndex);
            water.deepColor = {
                static_cast<f32>(optical.deepWaterColor.x),
                static_cast<f32>(optical.deepWaterColor.y),
                static_cast<f32>(optical.deepWaterColor.z)};
            water.deepColorDepthMeters =
                static_cast<f32>(optical.deepColorDepthMeters);
            water.seaLevelMeters = nearFieldSeaLevelMeters;
            water.sunDirectionBody = studioDirectLight.directionBody;
            water.sunIrradiance = studioDirectLight.irradianceScale;
            water.skyIrradiance =
                radianceEstimateSettings.skyIrradianceLinear;
            water.opacity = static_cast<f32>(nearFieldWaterWeight);

            auto* waterRenderer = waterTerrain->second.renderer.get();
            waterRenderer->SetWaterOptics(water);

            const auto waterCamera =
                TerrainCameraFromBodyCamera(
                    view->Camera(),
                    waterRenderer->CameraFrame());

            // The sea is a sphere: from outside it, only view rays pointing
            // more steeply down than the horizon (sin dip = sqrt(1 - (R/r)^2))
            // can hit it. Skip the pass when no screen ray can.
            bool seaMayBeVisible = true;
            {
                const auto& waterEye = view->Camera();
                const math::Double3 eyePosition =
                    waterEye.localPositionMeters;
                const f64 seaRadius =
                    terrainRuntime->planet.radiusMeters +
                    nearFieldSeaLevelMeters;
                const f64 eyeRadius = math::Length(eyePosition);
                if (seaRadius > 1.0 && eyeRadius > seaRadius * 1.000001)
                {
                    const f64 ratio = seaRadius / eyeRadius;
                    const f64 sinDip =
                        std::sqrt(std::max(0.0, 1.0 - ratio * ratio));
                    const math::Double3 localUp =
                        eyePosition * (1.0 / eyeRadius);
                    const math::Double3 eyeForward = math::Normalize(
                        math::Double3{
                            static_cast<f64>(waterEye.forward.x),
                            static_cast<f64>(waterEye.forward.y),
                            static_cast<f64>(waterEye.forward.z)});
                    const math::Double3 eyeUpHint{
                        static_cast<f64>(waterEye.up.x),
                        static_cast<f64>(waterEye.up.y),
                        static_cast<f64>(waterEye.up.z)};
                    const math::Double3 eyeRight = math::Normalize(
                        math::Cross(eyeForward, eyeUpHint));
                    const math::Double3 eyeUp =
                        math::Cross(eyeRight, eyeForward);
                    const f64 tanHalf =
                        std::tan(0.5 * static_cast<f64>(
                            waterEye.verticalFovRadians));
                    const f64 aspect =
                        static_cast<f64>(width) /
                        static_cast<f64>(std::max(height, 1U));
                    // Margin of ~5 degrees covers the sampling grid spacing.
                    constexpr f64 kMargin = 0.09;
                    seaMayBeVisible = false;
                    for (int iy = 0; iy <= 24 && !seaMayBeVisible; ++iy)
                    {
                        const f64 ny = 1.0 - 2.0 * iy / 24.0;
                        for (int ix = 0; ix <= 40; ++ix)
                        {
                            const f64 nx = 2.0 * ix / 40.0 - 1.0;
                            const math::Double3 direction =
                                math::Normalize(
                                    eyeForward +
                                    eyeRight * (nx * aspect * tanHalf) +
                                    eyeUp * (ny * tanHalf));
                            if (math::Dot(direction, localUp) <
                                -sinDip + kMargin)
                            {
                                seaMayBeVisible = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (seaMayBeVisible)
            graph.AddPass(
                prefix + ".NearFieldWater",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    }
                },
                [color,
                 lightingDepth,
                 waterRenderer,
                 waterCamera,
                 width,
                 height,
                 frameIndex](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    const std::array<rhi::Texture*, 1> waterTargets{
                        color};
                    commands.SetRenderTargetsReadOnlyDepth(
                        waterTargets,
                        *lightingDepth);
                    waterRenderer->DrawWater(
                        commands,
                        frameIndex,
                        width,
                        height,
                        waterCamera,
                        *lightingDepth);
                });
        }
    }

    // Emissive primitives light their surroundings analytically (sphere-light
    // irradiance, SDF soft shadows) instead of through noisy GI rays.
    if (!info.layers.bypassMeshSurfaces &&
        !info.layers.bypassIndirectLighting &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        if (const auto emissiveFound = staticMeshPresentations_.find(info.id);
            emissiveFound != staticMeshPresentations_.end() &&
            !emissiveFound->second.emitters.empty())
        {
            auto& emissivePresentation = emissiveFound->second;
            if (emissivePresentation.emissiveLighting == nullptr ||
                emissivePresentation.emissiveLighting->Width() != width ||
                emissivePresentation.emissiveLighting->Height() != height)
            {
                emissivePresentation.emissiveLighting =
                    device_->CreateTexture({
                        .width = width,
                        .height = height,
                        .format = rhi::TextureFormat::RGBA16_Float,
                        .initialState = rhi::ResourceState::ShaderResource,
                        .allowUnorderedAccess = true});
            }
            auto* const lightingTarget =
                emissivePresentation.emissiveLighting.get();
            const auto lightingHandle = graph.ImportTexture(
                prefix + ".EmissiveLighting",
                *lightingTarget,
                rhi::ResourceState::ShaderResource);

            graph.AddPass(
                prefix + ".EmissiveLights",
                {
                    {
                        .texture = targets.surfaceBaseRoughness,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.surfaceNormalMetallic,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.surfaceEmissionClass,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = lightingHandle,
                        .state = rhi::ResourceState::UnorderedAccess,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 lights = emissiveFound->second.emitters,
                 lightingTarget,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 width,
                 height,
                 lightingView,
                 useSdf = !info.layers.bypassSdfGi](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    lighting::SdfGatherInput sdfInput;
                    if (const auto& volume = meshSdfScene_.Volume();
                        useSdf && volume.ready &&
                        volume.radiance != nullptr &&
                        volume.distanceCorners != nullptr)
                    {
                        sdfInput.distance = volume.distanceCorners;
                        sdfInput.albedo = volume.albedo;
                        sdfInput.normal = volume.normal;
                        sdfInput.radiance = volume.radiance;
                        sdfInput.originInFrameMeters =
                            volume.originInFrameMeters;
                        sdfInput.voxelSize = volume.voxelSize;
                        sdfInput.dimensions = volume.dimensions;
                    }
                    emissiveLightRenderer_.Light(
                        commands,
                        lights,
                        *lightingTarget,
                        *lightingBaseRoughness,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        *lightingDepth,
                        width,
                        height,
                        lightingView,
                        sdfInput.distance != nullptr ? &sdfInput : nullptr);
                });

            graph.AddPass(
                prefix + ".EmissiveLightsComposite",
                {
                    {
                        .texture = lightingHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this, lightingTarget, color, width, height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    emissiveLightRenderer_.Composite(
                        commands, *lightingTarget, *color, width, height);
                });
        }
    }

    recordComposeStage("gi");
    ComposeAtmospherePasses(context);

    // Glass Primitives: sun caustics onto the receivers behind them, a copy of
    // the lit scene, then the refracting bodies themselves over it. After the
    // atmosphere so the sky and aerial perspective are part of what is seen
    // through them.
    if (!info.layers.bypassMeshSurfaces &&
        logicalTarget->mode != studio_session::ViewportMode::Debug)
    {
        if (const auto glassFound = staticMeshPresentations_.find(info.id);
            glassFound != staticMeshPresentations_.end() &&
            !glassFound->second.glass.empty())
        {
            auto& glassPresentation = glassFound->second;
            if (glassPresentation.glassBackdrop == nullptr ||
                glassPresentation.glassBackdrop->Width() != width ||
                glassPresentation.glassBackdrop->Height() != height)
            {
                glassPresentation.glassBackdrop = device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource});
            }
            auto* const backdrop = glassPresentation.glassBackdrop.get();
            const auto backdropHandle = graph.ImportTexture(
                prefix + ".GlassBackdrop",
                *backdrop,
                rhi::ResourceState::ShaderResource);

            mesh_render::GlassLighting glassLighting;
            glassLighting.toSun = {
                directLight.directionToLight.x,
                directLight.directionToLight.y,
                directLight.directionToLight.z};
            if (studioDirectLight.direct.has_value())
            {
                glassLighting.sunIrradiance = {
                    directLight.colorLinear.x * directLight.irradianceScale,
                    directLight.colorLinear.y * directLight.irradianceScale,
                    directLight.colorLinear.z * directLight.irradianceScale};
                // Smooth cut-off as the sun passes the local horizon.
                const auto& eye = view->Lighting().cameraPositionInFrameMeters;
                const f64 eyeLength = math::Length(eye);
                if (eyeLength > 1.0)
                {
                    const f64 sinElevation =
                        (eye.x * directLight.directionToLight.x +
                         eye.y * directLight.directionToLight.y +
                         eye.z * directLight.directionToLight.z) /
                        eyeLength;
                    const f64 t =
                        std::clamp((sinElevation + 0.02) / 0.07, 0.0, 1.0);
                    glassLighting.sunVisibility =
                        static_cast<f32>(t * t * (3.0 - 2.0 * t));
                }
            }
            // Rays that leave the screen and find nothing in the mesh
            // distance field either see the open sky.
            constexpr f32 kInversePi = 0.31830988F;
            glassLighting.environment = {
                radianceEstimateSettings.skyIrradianceLinear.x * kInversePi,
                radianceEstimateSettings.skyIrradianceLinear.y * kInversePi,
                radianceEstimateSettings.skyIrradianceLinear.z * kInversePi};
            {
                const auto& eye = view->Lighting().cameraPositionInFrameMeters;
                const f64 eyeLength = math::Length(eye);
                if (eyeLength > 1.0)
                {
                    glassLighting.localUp = {
                        static_cast<f32>(eye.x / eyeLength),
                        static_cast<f32>(eye.y / eyeLength),
                        static_cast<f32>(eye.z / eyeLength)};
                }
            }

            std::vector<render_graph::TextureUse> causticUses{
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.surfaceBaseRoughness,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.surfaceNormalMetallic,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.surfaceEmissionClass,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    }
            };
            if (glassSunShadowHandle.has_value())
            {
                causticUses.push_back({
                    .texture = *glassSunShadowHandle,
                    .state = rhi::ResourceState::ShaderResource,
                    .access = render_graph::Access::Read});
            }
            graph.AddPass(
                prefix + ".GlassCaustics",
                causticUses,
                [this,
                 instances = glassFound->second.glass,
                 color,
                 lightingDepth,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 width,
                 height,
                 lightingView,
                 glassLighting,
                 sunShadowMap = glassSunShadowMap,
                 shadowFrame = meshShadowFrame](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    glassSurfaceRenderer_.DrawCaustics(
                        commands,
                        instances,
                        *color,
                        *lightingDepth,
                        *lightingBaseRoughness,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        width,
                        height,
                        lightingView,
                        glassLighting,
                        sunShadowMap,
                        shadowFrame.has_value() ? &*shadowFrame : nullptr);
                });

            graph.AddPass(
                prefix + ".GlassBackdropCopy",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = backdropHandle,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this, color, backdrop, width, height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    glassSurfaceRenderer_.CopyBackdrop(
                        commands, *color, *backdrop, width, height);
                });

            graph.AddPass(
                prefix + ".Glass",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = backdropHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    }
                },
                [this,
                 instances = glassFound->second.glass,
                 color,
                 backdrop,
                 lightingDepth,
                 width,
                 height,
                 lightingView,
                 glassLighting,
                 useSdf = !info.layers.bypassSdfGi](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    lighting::SdfGatherInput sdfInput;
                    if (const auto& volume = meshSdfScene_.Volume();
                        useSdf && volume.ready &&
                        volume.radiance != nullptr &&
                        volume.distanceCorners != nullptr)
                    {
                        sdfInput.distance = volume.distanceCorners;
                        sdfInput.albedo = volume.albedo;
                        sdfInput.normal = volume.normal;
                        sdfInput.radiance = volume.radiance;
                        sdfInput.originInFrameMeters =
                            volume.originInFrameMeters;
                        sdfInput.voxelSize = volume.voxelSize;
                        sdfInput.dimensions = volume.dimensions;
                    }
                    glassSurfaceRenderer_.DrawGlass(
                        commands,
                        instances,
                        *color,
                        *backdrop,
                        *lightingDepth,
                        width,
                        height,
                        lightingView,
                        glassLighting,
                        sdfInput.distance != nullptr ? &sdfInput : nullptr);
                });
        }
    }

    // RenderView imports depth as DepthWrite on the next frame.
    // Shared direct lighting samples it read-only, so close this frame
    // by returning the actual Vulkan image to that persistent state.
    graph.AddPass(
        prefix + ".RestoreDepthWrite",
        {
            {
                .texture = targets.depth,
                .state =
                    rhi::ResourceState::
                        DepthWrite,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [](
            rhi::CommandList&,
            const render_graph::Resources&)
        {
        });

}
} // namespace orbit::studio_ui
