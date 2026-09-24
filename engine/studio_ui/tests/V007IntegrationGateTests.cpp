#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/HybridReflectionRenderer.hpp>
#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/lighting/RadianceEstimator.hpp>
#include <orbit/post_process/ColorLut.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>
#include <orbit/post_process/OutputTransform.hpp>
#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>
#include <orbit/studio_ui/V007ValidationScenarios.hpp>
#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
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
            << "V0.0.7 final integration gate failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

[[nodiscard]] bool Near(
    const float left,
    const float right,
    const float epsilon = 1.0e-4F) noexcept
{
    return std::abs(left - right) <= epsilon;
}
} // namespace

int main()
{
    using namespace orbit;

    // Compile-time architecture seams. M46 is deliberately not a second
    // renderer or validation implementation: it pins the public production
    // services that the earlier milestone gates exercise in depth.
    static_assert(std::is_class_v<lighting::DirectLightingRenderer>);
    static_assert(std::is_class_v<lighting::HybridReflectionRenderer>);
    static_assert(std::is_class_v<volume_render::UniversalVolumeRenderer>);
    static_assert(std::is_class_v<studio_ui::DisplayDiagnosticsUi>);

    static_assert(requires(
        lighting::LightingScheduler& scheduler,
        lighting::LightingRequestedWork requested)
    {
        scheduler.BuildPlan(requested, false);
        scheduler.BuildPlan(requested, true);
        scheduler.SmoothedTimings();
    });

    static_assert(requires(
        volume_solver::SurfaceVolumeReferenceConfig config,
        const std::vector<volume_solver::SurfaceVolumeCell>& input,
        std::vector<volume_solver::SurfaceVolumeCell>& output)
    {
        volume_solver::StepSurfaceVolumeReference(
            input,
            output,
            config);
    });

    // The named M43 matrix is the shared validation authority used by the
    // deterministic, visual-tolerance and performance capture workflows.
    Check(studio_ui::kV007ValidationScenarios.size() == 13U);

    // RT capability may select an exact-query backend, but it must not create a
    // second lighting model or silently increase work/budget.
    lighting::LightingSchedulerConfig schedulerConfig{};
    schedulerConfig.hardwareRayQueryEnabled = true;
    lighting::LightingScheduler scheduler(schedulerConfig);
    const lighting::LightingRequestedWork requested{
        .exactVisibilityQueries = 512U,
        .radianceCacheUpdates = 192U,
        .reflectionQueries = 96U,
        .emissiveUpdates = 64U
    };
    const auto softwarePlan = scheduler.BuildPlan(requested, false);
    const auto hardwarePlan = scheduler.BuildPlan(requested, true);
    Check(Near(softwarePlan.TotalBudgetMs(), hardwarePlan.TotalBudgetMs()));
    Check(softwarePlan.exactVisibilityQueries == hardwarePlan.exactVisibilityQueries);
    Check(softwarePlan.radianceCacheUpdates == hardwarePlan.radianceCacheUpdates);
    Check(softwarePlan.reflectionQueries == hardwarePlan.reflectionQueries);
    Check(softwarePlan.emissiveUpdates == hardwarePlan.emissiveUpdates);
    Check(!softwarePlan.preferHardwareRayQuery);
    Check(hardwarePlan.preferHardwareRayQuery);

    // Physical emission remains scene radiance. GI participation is a transport
    // policy and cannot change visible emission or require presentation bloom.
    const lighting::PhysicalMaterialEmission emissive{
        .colorLinear = {0.15F, 0.55F, 1.0F},
        .luminanceNits = 6000.0F,
        .contributesToGi = true,
        .giScale = 1.4F
    };
    const auto evaluated = lighting::EvaluateMaterialEmission(emissive);
    Check(evaluated.visibleRadiance.x > 0.0F);
    Check(evaluated.visibleRadiance.y > evaluated.visibleRadiance.x);
    Check(evaluated.giRadiance.z > 0.0F);

    auto visibleOnly = emissive;
    visibleOnly.contributesToGi = false;
    const auto visibleOnlyEvaluated = lighting::EvaluateMaterialEmission(visibleOnly);
    Check(Near(visibleOnlyEvaluated.visibleRadiance.x, evaluated.visibleRadiance.x));
    Check(Near(visibleOnlyEvaluated.visibleRadiance.y, evaluated.visibleRadiance.y));
    Check(Near(visibleOnlyEvaluated.visibleRadiance.z, evaluated.visibleRadiance.z));
    Check(Near(visibleOnlyEvaluated.giRadiance.x, 0.0F));
    Check(Near(visibleOnlyEvaluated.giRadiance.y, 0.0F));
    Check(Near(visibleOnlyEvaluated.giRadiance.z, 0.0F));

    // Presentation-only LUT/output operations are deterministic transforms and
    // remain downstream from physical lighting authority.
    const auto identity = post_process::BuildIdentityColorLut(8U);
    Check(identity.size == 8U);
    Check(post_process::IsDisplayLutCompatible(identity));
    Check(Near(post_process::BlendColorLutChannel(0.35F, 0.8F, 0.0F), 0.35F));
    Check(Near(post_process::BlendColorLutChannel(0.35F, 0.8F, 1.0F), 0.8F));

    post_process::OutputTransformSettings output{};
    output.mode = post_process::OutputMode::Hdr10;
    output.referenceWhiteNits = 203.0F;
    output.requestedPeakNits = 1200.0F;
    const auto hdr = post_process::ResolveOutputTransform(
        output,
        {.hdr10Supported = true, .reportedPeakNits = 1500.0F});
    const auto sdrFallback = post_process::ResolveOutputTransform(
        output,
        {.hdr10Supported = false, .reportedPeakNits = 0.0F});
    Check(hdr.resolvedMode == post_process::OutputMode::Hdr10);
    Check(sdrFallback.resolvedMode == post_process::OutputMode::Sdr);
    Check(sdrFallback.fellBackToSdr);
    // Output capability resolution cannot feed back into lighting work.
    const auto afterOutputPolicy = scheduler.BuildPlan(requested, true);
    Check(afterOutputPolicy.exactVisibilityQueries == hardwarePlan.exactVisibilityQueries);
    Check(afterOutputPolicy.radianceCacheUpdates == hardwarePlan.radianceCacheUpdates);

    // Eye state has independent overload and dark adaptation channels. Extreme
    // highlights are ceiling-limited instead of forcing unbounded exposure.
    post_process::LuminanceHistogramStatistics glare{};
    glare.valid = true;
    glare.sampleCount = 4096U;
    glare.weightedSampleCount = 4096U;
    glare.medianLog2 = 0.0F;
    glare.p95Log2 = 2.0F;
    glare.p99Log2 = 12.0F;
    glare.peakLog2 = 16.0F;
    post_process::HumanEyeAdaptationConfig eyeConfig{};
    auto eye = post_process::UpdateHumanEyeAdaptation(
        post_process::HumanEyeAdaptationState{},
        glare,
        1.0F / 60.0F,
        eyeConfig);
    Check(eye.photopicTargetLog2 <= eyeConfig.photopicCeilingLog2 + 1.0e-4F);
    Check(eye.overloadTarget > 0.0F);
    Check(eye.darkAdaptation >= 0.0F);

    // Universal-volume emission and near/far policy remain on the same field /
    // rendering architecture. Far importance must not require denser work.
    const auto fire = volume_render::IntegrateHomogeneousVolume(
        0.65F,
        5.0F,
        0.9F,
        0.7F,
        {1.0F, 0.7F, 0.35F},
        {0.25F, 0.25F, 0.25F},
        {1.0F, 0.12F, 0.02F},
        2.5F,
        3.5F);
    Check(fire.transmittance >= 0.0F && fire.transmittance <= 1.0F);
    Check(fire.emittedRadiance.x > 0.0F);
    Check(fire.totalRadiance.x >= fire.emittedRadiance.x);

    volume_representation::RepresentationInput nearInput{
        .volume = {.high = 0x46U, .low = 0x07U},
        .volumeCenterInFrameMeters = {0.0, 0.0, 0.0},
        .halfExtentsMeters = {12.0, 8.0, 12.0},
        .observerInFrameMeters = {0.0, 0.0, 24.0},
        .stableFrame = 46U,
        .stableBody = 7U,
        .viewportHeightPixels = 1080U,
        .verticalFovRadians = 1.0F,
        .authoredMode = world_model::VolumeRepresentationMode::Auto,
        .liveDistanceMeters = 40.0,
        .passiveDistanceMeters = 300.0,
        .liveProjectedPixels = 96.0F,
        .passiveProjectedPixels = 12.0F,
        .hysteresisFraction = 0.12F,
        .bakedAvailable = true
    };
    auto farInput = nearInput;
    farInput.observerInFrameMeters = {0.0, 0.0, 5000.0};

    const auto nearDecision = volume_representation::ResolveRepresentation(nearInput);
    const auto farDecision = volume_representation::ResolveRepresentation(
        farInput,
        nearDecision.representation);
    Check(nearDecision.stableAddressFingerprint == farDecision.stableAddressFingerprint);
    Check(nearDecision.denseFieldRequired || !farDecision.denseFieldRequired);
    Check(farDecision.liveWeight <= nearDecision.liveWeight + 1.0e-4F);

    return 0;
}
