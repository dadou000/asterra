#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingRuntimeProfiler.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/studio_ui/StudioRuntimeProfiler.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <optional>
#include <string>
#include <string_view>

#ifndef ORBIT_GIT_COMMIT
#define ORBIT_GIT_COMMIT "unknown"
#endif

namespace orbit::studio_ui
{
struct StudioRadianceCachePerformanceDiagnostics
{
    bool available{false};
    u64 residentCells{0U};
    u64 dirtyCells{0U};
    u64 totalCells{0U};
    u64 gpuBytes{0U};
    u32 scheduledUpdates{0U};
};

struct V007PerformanceCaptureEnvironment
{
    std::string adapter;
    std::string api{"Vulkan"};
    std::string commit;
    std::string scenario;
    std::string settings;
    u32 width{0U};
    u32 height{0U};

    bool accelerationStructures{false};
    bool rayQuery{false};
    bool rayTracingPipeline{false};
    bool meshShaders{false};
    bool variableRateShading{false};
};

struct V007PerformanceCapture
{
    V007PerformanceCaptureEnvironment environment{};
    lighting::LightingRuntimeProfilerSnapshot lighting{};
    StudioRadianceCachePerformanceDiagnostics radianceCache{};
    StudioVolumeRuntimeProfilerSnapshot volume{};

    // M44 deliberately distinguishes measured GPU timing from work-cost
    // descriptors. Volume simulation currently exposes timestamped GPU time;
    // render cost is captured as explicit raymarch/shadow work until a
    // renderer timestamp is available rather than fabricating milliseconds.
    u64 volumeRenderPixelSteps{0U};
    u64 volumeShadowPixelSteps{0U};
};

struct V007PerformancePairValidation
{
    bool passed{false};
    std::string diagnostic;
};

[[nodiscard]] V007PerformanceCapture BuildV007PerformanceCapture(
    const rhi::Device& device,
    u32 width,
    u32 height,
    std::string_view scenario,
    std::string_view settings,
    std::string_view commit = ORBIT_GIT_COMMIT,
    StudioRadianceCachePerformanceDiagnostics cache = {});

[[nodiscard]] std::string SerializeV007PerformanceCapture(
    const V007PerformanceCapture& capture);

[[nodiscard]] bool SaveV007PerformanceCapture(
    std::string_view path,
    const V007PerformanceCapture& capture,
    std::string* error = nullptr);

[[nodiscard]] V007PerformancePairValidation ValidateV007RtAutoPair(
    const V007PerformanceCapture& nonRt,
    const V007PerformanceCapture& rt);

[[nodiscard]] V007PerformancePairValidation ValidateV007VolumeAutoPair(
    const V007PerformanceCapture& nearCapture,
    const V007PerformanceCapture& farCapture);

[[nodiscard]] std::string_view V007RepresentationName(
    volume_representation::ResolvedRepresentation representation) noexcept;
} // namespace orbit::studio_ui
