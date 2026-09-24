#include <orbit/studio_ui/V007ValidationScenarios.hpp>

#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>
#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
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
            << "V0.0.7 deterministic validation failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

[[nodiscard]] bool Near(
    const float a,
    const float b,
    const float epsilon = 1.0e-4F)
{
    return std::abs(a - b) <= epsilon;
}
} // namespace

int main()
{
    using namespace orbit;

    // The catalog is the authoritative named M43 matrix. Visual tolerance
    // capture remains a separate GPU/hardware gate; this binary pins only
    // deterministic numerical invariants.
    Check(studio_ui::kV007ValidationScenarios.size() == 13U);
    for (const auto& scenario : studio_ui::kV007ValidationScenarios)
    {
        Check(!scenario.name.empty());
        Check(!scenario.invariant.empty());
        Check(scenario.gpuVisualToleranceRequired);
    }

    // LED Room / small-emitter authority: visible emission remains physical
    // radiance and GI scaling/gating cannot change the visible source.
    const lighting::PhysicalMaterialEmission led{
        .colorLinear = {1.0F, 0.25F, 0.05F},
        .luminanceNits = 1200.0F,
        .contributesToGi = true,
        .giScale = 0.5F
    };
    const auto ledEvaluated = lighting::EvaluateMaterialEmission(led);
    Check(ledEvaluated.visibleRadiance.x > 0.0F);
    Check(ledEvaluated.giRadiance.x > 0.0F);
    Check(ledEvaluated.giRadiance.x < ledEvaluated.visibleRadiance.x);

    auto visibleOnlyLed = led;
    visibleOnlyLed.contributesToGi = false;
    const auto visibleOnly = lighting::EvaluateMaterialEmission(visibleOnlyLed);
    Check(Near(visibleOnly.visibleRadiance.x, ledEvaluated.visibleRadiance.x));
    Check(Near(visibleOnly.giRadiance.x, 0.0F));

    // Cloud Glare: extreme P99/peak values create overload/headroom pressure,
    // but the normal photopic target cannot chase above its calibrated ceiling.
    post_process::LuminanceHistogramStatistics glare{};
    glare.valid = true;
    glare.sampleCount = 1024U;
    glare.weightedSampleCount = 1024U;
    glare.medianLog2 = 0.0F;
    glare.p95Log2 = 2.0F;
    glare.p99Log2 = 11.0F;
    glare.peakLog2 = 15.0F;

    post_process::HumanEyeAdaptationConfig eyeConfig{};
    post_process::HumanEyeAdaptationState eye{};
    eye = post_process::UpdateHumanEyeAdaptation(
        eye,
        glare,
        1.0F / 60.0F,
        eyeConfig);
    Check(eye.photopicTargetLog2 <= eyeConfig.photopicCeilingLog2 + 1.0e-4F);
    Check(eye.photopicCeilingExcessStops >= 0.0F);
    Check(eye.overloadTarget > 0.0F);

    const float overloadAtGlare = eye.overload;
    post_process::LuminanceHistogramStatistics ordinary = glare;
    ordinary.p99Log2 = 1.0F;
    ordinary.peakLog2 = 2.0F;
    ordinary.p95Log2 = 0.5F;
    for (int i = 0; i < 120; ++i)
    {
        eye = post_process::UpdateHumanEyeAdaptation(
            eye,
            ordinary,
            1.0F / 60.0F,
            eyeConfig);
    }
    Check(eye.overload < overloadAtGlare);

    // Dark Interior -> Daylight: dark adaptation is independent state and is
    // reset much faster on daylight return than it accumulated in darkness.
    post_process::HumanEyeAdaptationState darkEye{};
    post_process::LuminanceHistogramStatistics dark{};
    dark.valid = true;
    dark.sampleCount = 1024U;
    dark.weightedSampleCount = 1024U;
    dark.medianLog2 = -10.0F;
    dark.p95Log2 = -8.0F;
    dark.p99Log2 = -7.0F;
    dark.peakLog2 = -6.0F;

    for (int i = 0; i < 600; ++i)
    {
        darkEye = post_process::UpdateHumanEyeAdaptation(
            darkEye,
            dark,
            1.0F / 60.0F,
            eyeConfig);
    }
    Check(darkEye.darkAdaptation > 0.0F);
    const float darkState = darkEye.darkAdaptation;

    post_process::LuminanceHistogramStatistics daylight = ordinary;
    daylight.medianLog2 = 2.0F;
    daylight.p95Log2 = 3.0F;
    daylight.p99Log2 = 4.0F;
    daylight.peakLog2 = 5.0F;
    for (int i = 0; i < 60; ++i)
    {
        darkEye = post_process::UpdateHumanEyeAdaptation(
            darkEye,
            daylight,
            1.0F / 60.0F,
            eyeConfig);
    }
    Check(darkEye.darkAdaptation < darkState);

    // RT A/B: capability/backend selection is independent from requested work
    // and total budget. Hardware availability may change only the preferred
    // visibility backend at this point in the deterministic gate.
    lighting::LightingSchedulerConfig schedulerConfig{};
    schedulerConfig.hardwareRayQueryEnabled = true;
    lighting::LightingScheduler scheduler(schedulerConfig);
    const lighting::LightingRequestedWork requested{
        .exactVisibilityQueries = 400U,
        .radianceCacheUpdates = 200U,
        .reflectionQueries = 100U,
        .emissiveUpdates = 80U
    };
    const auto rtOff = scheduler.BuildPlan(requested, false);
    const auto rtOn = scheduler.BuildPlan(requested, true);
    Check(rtOff.TotalBudgetMs() == rtOn.TotalBudgetMs());
    Check(rtOff.exactVisibilityQueries == rtOn.exactVisibilityQueries);
    Check(rtOff.radianceCacheUpdates == rtOn.radianceCacheUpdates);
    Check(rtOff.reflectionQueries == rtOn.reflectionQueries);
    Check(rtOff.emissiveUpdates == rtOn.emissiveUpdates);
    Check(!rtOff.preferHardwareRayQuery);
    Check(rtOn.preferHardwareRayQuery);

    // Smoke obstacle/advection reference: true 3D transport moves density and
    // temperature vertically with the same production reference solver used by
    // the volume-solver tests.
    volume_solver::SurfaceVolumeReferenceConfig local3D{
        .width = 3U,
        .height = 3U,
        .layers = 3U,
        .cellSizeX = 1.0F,
        .cellSizeY = 1.0F,
        .cellSizeZ = 1.0F,
        .deltaSeconds = 0.2F
    };
    std::vector<volume_solver::LocalVolumeReferenceCell> localInput(27U);
    std::vector<volume_solver::LocalVolumeReferenceCell> localOutput(27U);
    const auto localIndex = [](const u32 x, const u32 y, const u32 z)
    {
        return (static_cast<std::size_t>(z) * 3U + y) * 3U + x;
    };
    const auto center = localIndex(1U, 1U, 1U);
    const auto above = localIndex(1U, 2U, 1U);
    localInput[center].density = 1.0F;
    localInput[center].temperature = 8.0F;
    for (auto& cell : localInput)
    {
        cell.velocity = {0.0F, 1.0F, 0.0F};
    }
    volume_solver::StepLocalVolumeReference(localInput, localOutput, local3D);
    Check(localOutput[center].density < localInput[center].density);
    Check(localOutput[above].density > 0.0F);
    Check(localOutput[above].temperature > 0.0F);

    // Surface dust/wind reference: external wind changes the transported
    // velocity while the scalar field remains finite and non-negative.
    volume_solver::SurfaceVolumeReferenceConfig surface{
        .width = 5U,
        .height = 1U,
        .layers = 1U,
        .cellSizeX = 1.0F,
        .cellSizeY = 1.0F,
        .cellSizeZ = 1.0F,
        .deltaSeconds = 0.25F
    };
    std::vector<volume_solver::SurfaceVolumeCell> dustInput(5U);
    std::vector<volume_solver::SurfaceVolumeCell> dustCalm(5U);
    std::vector<volume_solver::SurfaceVolumeCell> dustWind(5U);
    dustInput[1].scalar = 1.0F;
    volume_solver::StepSurfaceVolumeReference(dustInput, dustCalm, surface);
    volume_solver::StepSurfaceVolumeReference(
        dustInput,
        dustWind,
        surface,
        {2.0F, 0.0F, 0.0F});
    Check(dustWind[3].velocity.x > dustCalm[3].velocity.x);
    for (const auto& cell : dustWind)
    {
        Check(std::isfinite(cell.scalar));
        Check(cell.scalar >= 0.0F);
    }

    // Emissive fire GI: homogeneous integration keeps emitted radiance in HDR
    // and adds it independently of scattering.
    const auto fire = volume_render::IntegrateHomogeneousVolume(
        0.7F,
        4.0F,
        1.0F,
        0.6F,
        {1.0F, 0.7F, 0.4F},
        {0.0F, 0.0F, 0.0F},
        {1.0F, 0.15F, 0.02F},
        3.0F,
        4.0F);
    Check(fire.emittedRadiance.x > 0.0F);
    Check(fire.totalRadiance.x >= fire.emittedRadiance.x);
    Check(fire.transmittance >= 0.0F && fire.transmittance <= 1.0F);

    // Live -> baked equivalence: identical authored inputs produce identical
    // native cache fingerprints and sampled density/emission.
    world_model::ResolvedVolumeDomain domain;
    domain.object = {.high = 0x43U, .low = 0x01U};
    domain.preset = "Fire";
    domain.centerMeters = {0.0, 0.0, 0.0};
    domain.halfExtentsMeters = {4.0, 4.0, 4.0};
    domain.fieldMask =
        static_cast<u64>(world_model::VolumeField::Density) |
        static_cast<u64>(world_model::VolumeField::Emission);

    std::vector<world_model::ResolvedVolumeInput> inputs;
    inputs.push_back({
        .object = {.high = 0x43U, .low = 0x02U},
        .role = world_model::VolumeInputRole::Source,
        .enabled = true,
        .order = 0,
        .kind = static_cast<i64>(world_model::VolumeSourceKind::Brush),
        .shape = world_model::VolumeSourceShape::Sphere,
        .positionMeters = {0.0,0.0,0.0},
        .radiusMeters = 2.0,
        .halfExtentsMeters = {2.0,2.0,2.0},
        .scalarValue = 0.8,
        .fieldMask = domain.fieldMask,
        .fingerprint = 0x4302U
    });

    const volume_representation::VolumeCacheBakeSettings bake{
        .resolution = 8U,
        .fieldMask = domain.fieldMask
    };
    const auto cacheA = volume_representation::BakeVolumeCache(domain, inputs, bake);
    const auto cacheB = volume_representation::BakeVolumeCache(domain, inputs, bake);
    Check(cacheA.payloadFingerprint == cacheB.payloadFingerprint);
    Check(Near(
        volume_representation::SampleVolumeCacheDensity(cacheA, 0.5, 0.5, 0.5),
        volume_representation::SampleVolumeCacheDensity(cacheB, 0.5, 0.5, 0.5)));
    Check(Near(
        volume_representation::SampleVolumeCacheEmission(cacheA, 0.5, 0.5, 0.5),
        volume_representation::SampleVolumeCacheEmission(cacheB, 0.5, 0.5, 0.5)));

    // Roaming-domain / near->far LOD: representation decisions always carry
    // normalized blend weights; authored stable addressing is deterministic.
    volume_representation::RepresentationInput nearInput{
        .volume = {.high = 0x43U, .low = 0x03U},
        .volumeCenterInFrameMeters = {0.0,0.0,0.0},
        .halfExtentsMeters = {10.0,10.0,10.0},
        .observerInFrameMeters = {0.0,0.0,20.0},
        .stableFrame = 7U,
        .stableBody = 9U,
        .viewportHeightPixels = 1080U,
        .verticalFovRadians = 1.0F,
        .authoredMode = world_model::VolumeRepresentationMode::Auto,
        .liveDistanceMeters = 30.0,
        .passiveDistanceMeters = 180.0,
        .liveProjectedPixels = 96.0F,
        .passiveProjectedPixels = 12.0F,
        .hysteresisFraction = 0.12F,
        .bakedAvailable = true
    };
    auto farInput = nearInput;
    farInput.observerInFrameMeters = {0.0,0.0,2000.0};

    const auto nearDecision = volume_representation::ResolveRepresentation(nearInput);
    const auto farDecision = volume_representation::ResolveRepresentation(
        farInput,
        nearDecision.representation);

    const auto weightSum = [](const volume_representation::RepresentationDecision& d)
    {
        return d.liveWeight + d.coarseWeight + d.passiveWeight + d.bakedWeight;
    };
    Check(Near(weightSum(nearDecision), 1.0F, 1.0e-3F));
    Check(Near(weightSum(farDecision), 1.0F, 1.0e-3F));
    Check(nearDecision.stableAddressFingerprint == farDecision.stableAddressFingerprint);

    return 0;
}
