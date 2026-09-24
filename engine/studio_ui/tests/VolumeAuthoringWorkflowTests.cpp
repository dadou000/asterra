#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeOutputCoupling.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>
#include <orbit/volume_render/UniversalVolumeRenderer.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <span>
#include <type_traits>
#include <vector>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "M39 volumetric Studio workflow gate failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace orbit;

    // The gate deliberately compiles the actual unified Studio authoring seam,
    // rather than a duplicate test-only facade. The panel must remain wired to
    // the same session, field storage, live solver and viewport renderer used
    // by the active editor.
    static_assert(std::is_constructible_v<
        studio_ui::VolumeAuthoringUi,
        studio_session::StudioSession&,
        volume_fields::VolumeFieldStorageService&,
        volume_solver::SurfaceVolumeSolverService&,
        studio_ui::StudioViewportRenderer&>);

    static_assert(requires(
        studio_ui::VolumeAuthoringUi& authoring,
        editor_ui::EditorUi& ui)
    {
        authoring.Register(ui);
    });

    const scene::ObjectId volumeId{
        .high = 0x4d3339564f4c554dULL,
        .low = 0x45574f524b464c4fULL
    };

    world_model::ResolvedVolumeDomain domain{};
    domain.object = volumeId;
    domain.preset = "Fire";
    domain.centerMeters = {8.0, 2.0, -4.0};
    domain.halfExtentsMeters = {6.0, 4.0, 6.0};
    domain.solverPolicy =
        world_model::VolumeSolverPolicy::Local3D;
    domain.representationMode =
        world_model::VolumeRepresentationMode::Baked;
    domain.fieldMask =
        static_cast<u64>(world_model::VolumeField::Density) |
        static_cast<u64>(world_model::VolumeField::Emission);
    domain.renderEnabled = true;
    domain.extinctionScale = 0.72F;
    domain.singleScatteringAlbedo = 0.84F;
    domain.emissionScale = 3.5F;
    domain.giEmissionScale = 1.25F;
    domain.outputParticlesEnabled = true;
    domain.outputSurfaceDepositsEnabled = true;
    domain.outputFieldThreshold = 0.05F;
    domain.outputParticleRatePerSecond = 48.0F;
    domain.outputParticleBudgetPerStep = 64U;
    domain.outputSurfaceDepositRatePerSecond = 16.0F;
    domain.outputSurfaceDepositBudgetPerStep = 32U;

    world_model::ResolvedVolumeInput source{};
    source.object = {
        .high = 0x4d3339534f555243ULL,
        .low = 0x4500000000000001ULL
    };
    source.role = world_model::VolumeInputRole::Source;
    source.enabled = true;
    source.kind = static_cast<i64>(
        world_model::VolumeSourceKind::Brush);
    source.shape = world_model::VolumeSourceShape::Sphere;
    source.positionMeters = domain.centerMeters;
    source.radiusMeters = 4.0;
    source.scalarValue = 1.0;
    source.fieldMask = domain.fieldMask;
    source.fingerprint = 0x390001U;

    const std::vector inputs{source};

    const volume_representation::VolumeCacheBakeSettings bakeSettings{
        .resolution = 16U,
        .fieldMask = domain.fieldMask
    };

    auto cache =
        volume_representation::BakeVolumeCache(
            domain,
            std::span<const world_model::ResolvedVolumeInput>(inputs),
            bakeSettings);

    Check(cache.descriptor.volume == volumeId);
    Check(cache.descriptor.resolutionX == 16U);
    Check(cache.descriptor.resolutionY == 16U);
    Check(cache.descriptor.resolutionZ == 16U);
    Check(!cache.density.empty());
    Check(!cache.emission.empty());
    Check(cache.payloadFingerprint != 0U);

    std::string freshness;
    Check(volume_representation::IsVolumeCacheCurrent(
        cache,
        domain,
        std::span<const world_model::ResolvedVolumeInput>(inputs),
        bakeSettings,
        &freshness));

    volume_representation::RepresentationInput representationInput{};
    representationInput.volume = volumeId;
    representationInput.volumeCenterInFrameMeters = domain.centerMeters;
    representationInput.halfExtentsMeters = domain.halfExtentsMeters;
    representationInput.observerInFrameMeters = {8.0, 2.0, 18.0};
    representationInput.stableFrame = 7U;
    representationInput.stableBody = 11U;
    representationInput.viewportHeightPixels = 1080U;
    representationInput.verticalFovRadians = 1.0F;
    representationInput.authoredMode = domain.representationMode;
    representationInput.bakedAvailable = true;

    const auto representation =
        volume_representation::ResolveRepresentation(
            representationInput,
            volume_representation::ResolvedRepresentation::Live);

    Check(representation.forced);
    Check(!representation.bakedFallback);
    Check(representation.representation ==
        volume_representation::ResolvedRepresentation::Baked);
    Check(representation.bakedWeight == 1.0F);
    Check(!representation.denseFieldRequired);

    // The same authored emission contract used by the Studio panel must reach
    // the shared lighting/GI authority without a volume-specific light model.
    const auto emissive =
        volume_render::BuildEmissiveVolumeSource(
            domain,
            0.75F);
    Check(emissive.has_value());
    Check(emissive->intensityScale > 0.0F);

    volume_representation::VolumeOutputCouplingService output;
    auto& outputSettings = output.Settings(volumeId);
    outputSettings.particlesEnabled = true;
    outputSettings.surfaceDepositsEnabled = true;
    outputSettings.fieldThreshold = 0.01F;
    outputSettings.particleRatePerSecond = 32.0F;
    outputSettings.particleBudgetPerStep = 32U;
    outputSettings.surfaceDepositRatePerSecond = 16.0F;
    outputSettings.surfaceDepositBudgetPerStep = 16U;

    const auto& batch =
        output.AdvanceBaked(
            domain,
            cache,
            1.0);

    Check(batch.volume == volumeId);
    Check(batch.diagnostics.requestedParticles > 0U);
    Check(batch.diagnostics.requestedSurfaceDeposits > 0U);
    Check(batch.diagnostics.emittedParticles <=
        outputSettings.particleBudgetPerStep);
    Check(batch.diagnostics.emittedSurfaceDeposits <=
        outputSettings.surfaceDepositBudgetPerStep);

    // One final compile-time contract pins the runtime policy fields consumed
    // by the M39 Studio panel and UniversalVolumeRenderer together.
    static_assert(requires(
        volume_render::VolumeRenderRuntimeSettings settings)
    {
        settings.followTarget;
        settings.liveDistanceMeters;
        settings.passiveDistanceMeters;
        settings.coarseResolution;
        settings.passiveResolution;
        settings.coarseRaymarchSteps;
        settings.passiveRaymarchSteps;
    });

    return 0;
}
