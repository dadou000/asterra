#include <orbit/studio_ui/V007PerformanceCapture.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace
{
void Check(
    const bool condition,
    const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "V0.0.7 performance capture test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

orbit::studio_ui::V007PerformanceCapture BaseCapture()
{
    using namespace orbit;

    studio_ui::V007PerformanceCapture capture{};
    capture.environment.adapter = "Reference GPU";
    capture.environment.api = "Vulkan";
    capture.environment.commit = "0123456789abcdef";
    capture.environment.scenario = "M44 Reference";
    capture.environment.settings = "Auto";
    capture.environment.width = 1920U;
    capture.environment.height = 1080U;

    capture.lighting.hasScheduledPlan = true;
    capture.lighting.hasMeasuredTimings = true;
    capture.lighting.config = {};
    capture.lighting.requested = {
        .exactVisibilityQueries = 400U,
        .radianceCacheUpdates = 200U,
        .reflectionQueries = 100U,
        .emissiveUpdates = 80U
    };
    capture.lighting.scheduled = {
        .budget = capture.lighting.config.budget,
        .requested = capture.lighting.requested,
        .exactVisibilityQueries = 400U,
        .radianceCacheUpdates = 200U,
        .reflectionQueries = 100U,
        .emissiveUpdates = 80U,
        .visibilityScale = 1.0F,
        .giScale = 1.0F,
        .reflectionScale = 1.0F,
        .emissiveScale = 1.0F
    };

    capture.radianceCache = {
        .available = true,
        .residentCells = 120000U,
        .dirtyCells = 400U,
        .totalCells = 196608U,
        .gpuBytes = 12582912U,
        .scheduledUpdates = 200U
    };

    capture.volume.hasSelection = true;
    capture.volume.hasFields = true;
    capture.volume.hasSolver = true;
    capture.volume.hasRenderer = true;
    capture.volume.fields.totalBytes = 64U * 1024U * 1024U;
    capture.volume.fields.residentTiles = 512U;
    capture.volume.solver.gpuTimingValid = true;
    capture.volume.solver.gpuMilliseconds = 1.25F;
    capture.volume.solver.gpuBudgetMilliseconds = 2.0F;
    capture.volume.solver.requestedIterations = 2U;
    capture.volume.solver.iterationsThisFrame = 2U;
    capture.volume.renderer.rendered = true;
    capture.volume.renderer.representation =
        volume_representation::ResolvedRepresentation::Live;
    capture.volume.renderer.raymarchSteps = 64U;
    capture.volume.renderer.shadowSteps = 8U;
    capture.volume.renderer.denseFieldRequired = true;
    capture.volume.renderer.projectedDiameterPixels = 240.0F;
    capture.volumeRenderPixelSteps =
        static_cast<u64>(capture.environment.width) *
        capture.environment.height *
        capture.volume.renderer.raymarchSteps;
    capture.volumeShadowPixelSteps =
        static_cast<u64>(capture.environment.width) *
        capture.environment.height *
        capture.volume.renderer.shadowSteps;
    return capture;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::studio_ui;

    auto nonRt = BaseCapture();
    nonRt.environment.rayQuery = false;
    nonRt.lighting.scheduled.hardwareRayQueryAvailable = false;
    nonRt.lighting.scheduled.preferHardwareRayQuery = false;

    auto rt = nonRt;
    rt.environment.adapter = "Reference RT GPU";
    rt.environment.rayQuery = true;
    rt.environment.accelerationStructures = true;
    rt.lighting.scheduled.hardwareRayQueryAvailable = true;
    rt.lighting.scheduled.preferHardwareRayQuery = true;

    const auto rtValidation = ValidateV007RtAutoPair(nonRt, rt);
    Check(rtValidation.passed);

    auto explodedBudget = rt;
    explodedBudget.lighting.config.budget.giMs += 1.0F;
    Check(!ValidateV007RtAutoPair(nonRt, explodedBudget).passed);

    auto far = nonRt;
    far.volume.renderer.representation =
        volume_representation::ResolvedRepresentation::Passive;
    far.volume.renderer.denseFieldRequired = false;
    far.volume.renderer.raymarchSteps = 8U;
    far.volume.renderer.shadowSteps = 2U;
    far.volume.renderer.projectedDiameterPixels = 7.0F;
    far.volume.solver.iterationsThisFrame = 0U;
    far.volume.fields.totalBytes = 8U * 1024U * 1024U;
    far.volumeRenderPixelSteps =
        static_cast<u64>(far.environment.width) *
        far.environment.height *
        far.volume.renderer.raymarchSteps;
    far.volumeShadowPixelSteps =
        static_cast<u64>(far.environment.width) *
        far.environment.height *
        far.volume.renderer.shadowSteps;

    const auto volumeValidation =
        ValidateV007VolumeAutoPair(nonRt, far);
    Check(volumeValidation.passed);

    auto badFar = far;
    badFar.volumeRenderPixelSteps = nonRt.volumeRenderPixelSteps + 1U;
    Check(!ValidateV007VolumeAutoPair(nonRt, badFar).passed);

    const std::string json = SerializeV007PerformanceCapture(rt);
    Check(json.find("orbit.v0.0.7.performance.v1") != std::string::npos);
    Check(json.find("Reference RT GPU") != std::string::npos);
    Check(json.find("\"api\": \"Vulkan\"") != std::string::npos);
    Check(json.find("\"commit\": \"0123456789abcdef\"") != std::string::npos);
    Check(json.find("\"ray_query\": true") != std::string::npos);
    Check(json.find("\"solver_gpu_ms\": 1.250000") != std::string::npos);
    Check(json.find("\"gpu_bytes\": 12582912") != std::string::npos);
    Check(json.find("\"representation\": \"Live\"") != std::string::npos);

    return 0;
}
