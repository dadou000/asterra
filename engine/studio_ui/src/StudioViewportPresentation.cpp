#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

celestial_globe::GpuMacroGlobeProduct*
StudioViewportRenderer::EnsureMacroGlobePresentation(
    const std::string_view viewportId,
    studio_session::StudioSession& session,
    const universe::BodyId body,
    const universe::BodyShape& shape,
    const terrain::TerrainSource& terrainSource,
    const f64 projectedRadiusPixels,
    const std::function<bool(u64)>& acquireGrant,
    const std::function<void(u64)>& completeGrant)
{
    if (device_ == nullptr)
    {
        throw std::logic_error(
            "Macro-globe presentation requires a render device.");
    }

    auto& presentation =
        macroGlobePresentations_[
            std::string(viewportId)];

    // A fixed 33-sample cube face spreads one vertex (and one appearance
    // sample: coastline, ice and cloud masks) over ~50 px once a planet
    // fills the view, producing staircase coastlines. Pick the tier whose
    // vertex spacing stays within a few pixels, with hysteresis so the
    // (parallel) rebuild does not thrash around a threshold.
    {
        constexpr std::array<u32, 5> kTiers{33U, 65U, 129U, 257U, 513U};
        constexpr f64 kUpgradeSpacingPixels = 4.0;
        constexpr f64 kDowngradeSpacingPixels = 2.0;

        const auto spacingPixels =
            [projectedRadiusPixels](const u32 resolution)
            {
                return std::max(projectedRadiusPixels, 0.0) *
                    (0.5 * std::numbers::pi_v<f64>) /
                    static_cast<f64>(resolution - 1U);
            };

        std::size_t tier = 0U;
        while (tier + 1U < kTiers.size() &&
               kTiers[tier] < presentation.faceResolution)
        {
            ++tier;
        }

        while (tier + 1U < kTiers.size() &&
               spacingPixels(kTiers[tier]) > kUpgradeSpacingPixels)
        {
            ++tier;
        }

        while (tier > 0U &&
               spacingPixels(kTiers[tier - 1U]) < kDowngradeSpacingPixels)
        {
            --tier;
        }

        presentation.faceResolution = kTiers[tier];
    }

    const celestial_globe::MacroGlobeConfig
        globeConfig{
            .faceResolution = presentation.faceResolution,
            .footprintScale = 1.5
        };

    const auto* terrainCapability =
        session.World().
            Surfaces().
            Registry().
            FindTerrainSurface(body);

    const u64 geometryFingerprint =
        terrainCapability != nullptr &&
            terrainCapability->terrain.get() == &terrainSource
        ? celestial_globe::
              MacroGlobeFingerprint(
                  terrainCapability->terrain,
                  shape,
                  globeConfig)
        : celestial_globe::
              MacroGlobeFingerprint(
                  terrainSource,
                  shape,
                  globeConfig);

    const u64 sourceRevision =
        terrainSource.Revision();

    const auto sphericalPlanet =
        session.World().
            Surfaces().
            Registry().
            SphericalPlanetDefinition(
                body);

    if (!sphericalPlanet.has_value())
    {
        throw std::logic_error(
            "Planetary appearance currently requires the spherical terrain body contract.");
    }

    celestial_appearance::
        AppearanceConfig
        appearanceConfig{
            .faceResolution =
                globeConfig.faceResolution,
            .footprintScale =
                globeConfig.footprintScale
        };

    const u64 appearanceFingerprint =
        celestial_appearance::
            PlanetaryAppearanceFingerprint(
                terrainSource,
                sphericalPlanet->
                    radiusMeters,
                appearanceConfig);

    const auto bodyObject =
        session.World().
            Universe().
            ObjectForBody(body);

    const auto resolvedOcean =
        bodyObject.has_value()
            ? world_model::
                  ResolveOceanBody(
                      session.World().
                          Objects(),
                      *bodyObject)
            : std::nullopt;

    appearanceConfig.standingWaterEnabled =
        resolvedOcean.has_value();

    const u64 activeOceanFingerprint =
        resolvedOcean.has_value()
            ? celestial_ocean::
                  OceanOpticalFingerprint(
                      resolvedOcean->optical)
            : 0U;

    const auto cloudFound =
        cloudPresentations_.find(
            viewportId);

    const celestial_clouds::CloudFieldProduct*
        activeCloudField =
            cloudFound !=
                    cloudPresentations_.end() &&
                cloudFound->second.field !=
                    nullptr
                ? cloudFound->second.field.get()
                : nullptr;

    const u64 activeCloudFingerprint =
        activeCloudField != nullptr
            ? activeCloudField->fingerprint
            : 0U;

    u64 derivedRevision =
        CombineFingerprint(
            geometryFingerprint,
            appearanceFingerprint);
    derivedRevision =
        CombineFingerprint(
            derivedRevision,
            sourceRevision);
    derivedRevision =
        CombineFingerprint(
            derivedRevision,
            activeOceanFingerprint);
    derivedRevision =
        CombineFingerprint(
            derivedRevision,
            activeCloudFingerprint);

    const bool recreate =
        presentation.product == nullptr ||
        presentation.appearanceProduct ==
            nullptr ||
        presentation.cachedDisc ==
            nullptr ||
        presentation.body != body ||
        presentation.sourceRevision !=
            sourceRevision ||
        presentation.fingerprint !=
            geometryFingerprint ||
        presentation.baseAppearanceFingerprint !=
            appearanceFingerprint ||
        presentation.oceanFingerprint !=
            activeOceanFingerprint ||
        presentation.cloudFingerprint !=
            activeCloudFingerprint;

    if (recreate &&
        acquireGrant(derivedRevision))
    {
        const auto mesh =
            celestial_globe::
                BuildMacroGlobe(
                    terrainSource,
                    shape,
                    globeConfig);

        auto appearance =
            celestial_appearance::
                BuildPlanetaryAppearance(
                    terrainSource,
                    sphericalPlanet->
                        radiusMeters,
                    appearanceConfig);

        if (resolvedOcean.has_value())
        {
            celestial_ocean::
                ApplyOrbitalOceanAppearance(
                    appearance,
                    resolvedOcean->optical);
        }

        if (activeCloudField != nullptr)
        {
            celestial_clouds::
                CompositeOrbitalCloudAppearance(
                    *activeCloudField,
                    appearance);
        }

        presentation.appearanceSummary =
            celestial_far_render::
                SummarizeAppearance(
                    appearance);

        const auto cachedDisc =
            celestial_far_render::
                BuildCachedDisc(
                    appearance,
                    {.resolution = 64U});

        presentation.cachedDisc =
            std::make_unique<
                celestial_far_render::
                    GpuCachedDiscProduct>(
                        *device_,
                        cachedDisc);

        presentation.cachedDiscFingerprint =
            cachedDisc.fingerprint;

        presentation.appearanceProduct =
            std::make_unique<
                celestial_appearance::
                    GpuPlanetaryAppearanceProduct>(
                        *device_,
                        appearance);

        presentation.product =
            std::make_unique<
                celestial_globe::
                    GpuMacroGlobeProduct>(
                        *device_,
                        mesh,
                        &appearance);

        presentation.body =
            body;
        presentation.sourceRevision =
            sourceRevision;
        presentation.fingerprint =
            geometryFingerprint;
        presentation.baseAppearanceFingerprint =
            appearanceFingerprint;
        presentation.appearanceFingerprint =
            appearance.fingerprint;
        presentation.oceanFingerprint =
            activeOceanFingerprint;
        presentation.cloudFingerprint =
            activeCloudFingerprint;
        presentation.appearanceTexels =
            static_cast<u32>(
                appearance.texels.size());

        completeGrant(
            derivedRevision);
    }

    if (resolvedOcean.has_value())
    {
        oceanDiagnostics_.insert_or_assign(
            std::string(viewportId),
            StudioOceanDiagnostics{
                .body = body,
                .opticalFingerprint =
                    activeOceanFingerprint,
                .refractiveIndex =
                    resolvedOcean->
                        optical.refractiveIndex,
                .orbitalRoughness =
                    resolvedOcean->
                        optical.orbitalRoughness,
                .glintStrength =
                    resolvedOcean->
                        optical.glintStrength,
                .oceanFraction =
                    presentation.
                        appearanceSummary.
                        oceanFraction
            });
    }
    else
    {
        if (const auto found = oceanDiagnostics_.find(viewportId);
            found != oceanDiagnostics_.end())
        {
            oceanDiagnostics_.erase(found);
        }
    }

    return presentation.product.get();
}

std::optional<StudioMacroGlobeDiagnostics>
StudioViewportRenderer::MacroGlobeDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        macroGlobePresentations_.find(
            viewportId);

    if (found ==
        macroGlobePresentations_.end())
    {
        return std::nullopt;
    }

    const auto& presentation =
        found->second;

    return StudioMacroGlobeDiagnostics{
        .body = presentation.body,
        .terrainRevision =
            presentation.sourceRevision,
        .geometryFingerprint =
            presentation.fingerprint,
        .appearanceFingerprint =
            presentation.appearanceFingerprint,
        .appearanceTexels =
            presentation.appearanceTexels
    };
}

std::optional<
    StudioSurfaceGlobeTransitionDiagnostics>
StudioViewportRenderer::
SurfaceGlobeTransitionDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        transitionDiagnostics_.find(
            viewportId);

    return found ==
            transitionDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

StudioClipmapPlanStats
StudioViewportRenderer::ClipmapPlanStats(
    const std::string_view viewportId) const noexcept
{
    const auto found = clipmapPlanStats_.find(viewportId);
    return found == clipmapPlanStats_.end() ? StudioClipmapPlanStats{}
                                            : found->second;
}

std::optional<
    StudioLuminanceHistogramDiagnostics>
StudioViewportRenderer::
LuminanceHistogramDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    return found ==
            luminanceHistogramPresentations_.end()
        ? std::nullopt
        : std::optional(
              found->second.diagnostics);
}

rhi::Texture*
StudioViewportRenderer::LuminanceMeteringMask(
    const std::string_view viewportId) noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    return found ==
                luminanceHistogramPresentations_.end()
            ? nullptr
            : found->second.meteringMask.get();
}

void StudioViewportRenderer::SetLuminanceHistogramConfig(
    const std::string_view viewportId,
    post_process::LuminanceHistogramConfig config)
{
    if (!std::isfinite(config.minimumLog2) ||
        !std::isfinite(config.maximumLog2))
    {
        throw std::invalid_argument(
            "Luminance histogram log range must be finite.");
    }

    if (config.maximumLog2 <=
        config.minimumLog2 + 0.01F)
    {
        config.maximumLog2 =
            config.minimumLog2 + 0.01F;
    }

    config.centerWeightStrength =
        std::clamp(
            config.centerWeightStrength,
            0.0F,
            1.0F);
    config.centerWeightRadius =
        std::max(
            config.centerWeightRadius,
            0.05F);

    luminanceHistogramPresentations_[
        std::string(viewportId)].
        diagnostics.config =
            config;
}

void StudioViewportRenderer::SetHumanEyeAdaptationConfig(
    const std::string_view viewportId,
    post_process::HumanEyeAdaptationConfig config)
{
    auto& diagnostics =
        luminanceHistogramPresentations_[
            std::string(viewportId)].
            diagnostics;

    diagnostics.eyeConfig =
        config;
}

void StudioViewportRenderer::ResetHumanEyeAdaptation(
    const std::string_view viewportId) noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    if (found ==
        luminanceHistogramPresentations_.end())
    {
        return;
    }

    post_process::ResetHumanEyeAdaptation(
        found->second.diagnostics.eyeState);
    found->second.hasEyeUpdateTime = false;
}

void StudioViewportRenderer::SetHumanEyeAdaptationLocked(
    const std::string_view viewportId,
    const bool locked)
{
    luminanceHistogramPresentations_[std::string(viewportId)]
        .diagnostics.eyeAdaptationLocked = locked;
}

void StudioViewportRenderer::SetHighlightEffectsConfig(
    const std::string_view viewportId,
    post_process::HighlightEffectsConfig config)
{
    config.bloomThreshold =
        std::max(config.bloomThreshold, 0.0F);
    config.bloomKnee =
        std::max(config.bloomKnee, 1.0e-5F);
    config.bloomStrength =
        std::max(config.bloomStrength, 0.0F);
    config.bloomRadiusPixels =
        std::max(config.bloomRadiusPixels, 0.5F);

    config.glareThreshold =
        std::max(config.glareThreshold, 0.0F);
    config.glareStrength =
        std::max(config.glareStrength, 0.0F);
    config.glareRadiusPixels =
        std::max(config.glareRadiusPixels, 1.0F);

    config.flareThreshold =
        std::max(config.flareThreshold, 0.0F);
    config.flareStrength =
        std::max(config.flareStrength, 0.0F);
    config.flareCompactness =
        std::max(config.flareCompactness, 1.0F);
    config.flareGhostScale =
        std::max(config.flareGhostScale, 0.0F);

    luminanceHistogramPresentations_[
        std::string(viewportId)].
        diagnostics.highlightConfig =
            config;
}

void StudioViewportRenderer::SetToneMappingConfig(
    const std::string_view viewportId,
    post_process::ToneMappingConfig config)
{
    config.referenceWhiteNits =
        std::max(
            config.referenceWhiteNits,
            1.0e-3F);
    config.peakNits =
        std::max(
            config.peakNits,
            config.referenceWhiteNits);
    config.shoulderStart =
        std::max(
            config.shoulderStart,
            0.0F);
    config.shoulderStrength =
        std::max(
            config.shoulderStrength,
            1.0e-3F);

    luminanceHistogramPresentations_[
        std::string(viewportId)].
        diagnostics.toneMapping =
            config;
}

void StudioViewportRenderer::SetLuminanceMeteringOverlay(
    const std::string_view viewportId,
    const bool enabled)
{
    luminanceHistogramPresentations_[
        std::string(viewportId)].
        showMeteringOverlay =
            enabled;
}

bool StudioViewportRenderer::LuminanceMeteringOverlay(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    return
        found !=
            luminanceHistogramPresentations_.end() &&
        found->second.showMeteringOverlay;
}

std::optional<
    StudioVisibilityProxyDiagnostics>
StudioViewportRenderer::
VisibilityProxyDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        visibilityProxyDiagnostics_.find(
            viewportId);

    return found ==
            visibilityProxyDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioEmissiveGiDiagnostics>
StudioViewportRenderer::
EmissiveGiDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        emissiveGiDiagnostics_.find(
            viewportId);

    return found ==
            emissiveGiDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioCelestialLightingDiagnostics>
StudioViewportRenderer::
CelestialLightingDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        lightingDiagnostics_.find(
            viewportId);

    return found ==
            lightingDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioAtmosphereDiagnostics>
StudioViewportRenderer::AtmosphereDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        atmosphereDiagnostics_.find(
            viewportId);

    return found ==
            atmosphereDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioCloudDiagnostics>
StudioViewportRenderer::CloudDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        cloudDiagnostics_.find(
            viewportId);

    return found ==
            cloudDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioOceanDiagnostics>
StudioViewportRenderer::OceanDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        oceanDiagnostics_.find(
            viewportId);

    return found ==
            oceanDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioRingDiagnostics>
StudioViewportRenderer::RingDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        ringDiagnostics_.find(
            viewportId);

    return found ==
            ringDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioMagnetosphereDiagnostics>
StudioViewportRenderer::MagnetosphereDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        magnetosphereDiagnostics_.find(
            viewportId);

    return found ==
            magnetosphereDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioCompactObjectDiagnostics>
StudioViewportRenderer::CompactObjectDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        compactObjectDiagnostics_.find(
            viewportId);

    return found ==
            compactObjectDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

celestial_scheduler::SchedulerFrameStats
StudioViewportRenderer::CelestialSchedulerStats() const noexcept
{
    return celestialScheduler_.Stats();
}

celestial_scheduler::SchedulerBudget
StudioViewportRenderer::CelestialSchedulerBudget() const noexcept
{
    return celestialScheduler_.Budget();
}

void StudioViewportRenderer::SetCelestialQualityPolicy(
    celestial_representation::QualityPolicy policy) noexcept
{
    policy.productionSurfaceErrorPixels =
        std::max(
            policy.productionSurfaceErrorPixels,
            1.0e-4);
    policy.macroDisplacementErrorPixels =
        std::max(
            policy.macroDisplacementErrorPixels,
            1.0e-4);
    policy.smoothGlobeMinimumRadiusPixels =
        std::max(
            policy.smoothGlobeMinimumRadiusPixels,
            1.0e-4);
    policy.discImpostorMinimumRadiusPixels =
        std::max(
            policy.discImpostorMinimumRadiusPixels,
            1.0e-4);
    policy.qualityScale =
        std::clamp(
            policy.qualityScale,
            0.1,
            8.0);
    policy.hysteresisFraction =
        std::clamp(
            policy.hysteresisFraction,
            0.0,
            0.49);

    celestialQualityPolicy_ =
        policy;
}

celestial_representation::QualityPolicy
StudioViewportRenderer::CelestialQualityPolicy() const noexcept
{
    return celestialQualityPolicy_;
}

std::optional<
    StudioStellarDiagnostics>
StudioViewportRenderer::StellarDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        stellarDiagnostics_.find(
            viewportId);

    return found ==
            stellarDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioGiantDiagnostics>
StudioViewportRenderer::GiantDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        giantDiagnostics_.find(
            viewportId);

    return found ==
            giantDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioSmallBodyDiagnostics>
StudioViewportRenderer::SmallBodyDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        smallBodyDiagnostics_.find(
            viewportId);

    return found ==
            smallBodyDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

} // namespace orbit::studio_ui
