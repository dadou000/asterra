#include <orbit/studio_ui/V007PerformanceCapture.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::string JsonEscape(const std::string_view text)
{
    std::string result;
    result.reserve(text.size() + 8U);

    for (const char value : text)
    {
        switch (value)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += value; break;
        }
    }

    return result;
}

[[nodiscard]] const char* Boolean(const bool value) noexcept
{
    return value ? "true" : "false";
}

void WriteLightingSection(
    std::ostringstream& stream,
    const lighting::LightingRuntimeProfilerSnapshot& profile,
    const lighting::LightingGpuSection section,
    const std::string_view name,
    const bool trailingComma)
{
    stream << "      \"" << name << "\": {\n";
    stream << "        \"budget_ms\": "
           << profile.config.budget.SectionMs(section) << ",\n";
    stream << "        \"measured_valid\": "
           << Boolean(profile.hasMeasuredTimings && profile.measured.HasSection(section)) << ",\n";
    stream << "        \"measured_ms\": "
           << (profile.hasMeasuredTimings && profile.measured.HasSection(section)
                ? profile.measured.SectionMs(section)
                : 0.0F) << "\n";
    stream << "      }" << (trailingComma ? "," : "") << "\n";
}

[[nodiscard]] bool SameBudget(
    const lighting::LightingBudget& left,
    const lighting::LightingBudget& right) noexcept
{
    constexpr f32 epsilon = 1.0e-5F;
    return
        std::abs(left.directLightingMs - right.directLightingMs) <= epsilon &&
        std::abs(left.visibilityMs - right.visibilityMs) <= epsilon &&
        std::abs(left.giMs - right.giMs) <= epsilon &&
        std::abs(left.reflectionMs - right.reflectionMs) <= epsilon &&
        std::abs(left.emissiveMs - right.emissiveMs) <= epsilon &&
        std::abs(left.postProcessMs - right.postProcessMs) <= epsilon;
}
} // namespace

V007PerformanceCapture BuildV007PerformanceCapture(
    const rhi::Device& device,
    const u32 width,
    const u32 height,
    const std::string_view scenario,
    const std::string_view settings,
    const std::string_view commit,
    StudioRadianceCachePerformanceDiagnostics cache)
{
    const auto& capabilities = device.Capabilities();

    V007PerformanceCapture capture{};
    capture.environment.adapter = std::string(device.AdapterName());
    capture.environment.api =
        device.GetBackend() == rhi::Backend::Vulkan
            ? "Vulkan"
            : "Unknown";
    capture.environment.commit = std::string(commit);
    capture.environment.scenario = std::string(scenario);
    capture.environment.settings = std::string(settings);
    capture.environment.width = width;
    capture.environment.height = height;
    capture.environment.accelerationStructures = capabilities.accelerationStructures;
    capture.environment.rayQuery = capabilities.rayQuery;
    capture.environment.rayTracingPipeline = capabilities.rayTracingPipeline;
    capture.environment.meshShaders = capabilities.meshShaders;
    capture.environment.variableRateShading = capabilities.variableRateShading;

    capture.lighting = lighting::StudioLightingRuntimeProfiler();
    capture.radianceCache = cache;
    capture.volume = StudioVolumeRuntimeProfiler();

    if (capture.volume.hasRenderer && capture.volume.renderer.rendered)
    {
        const u64 pixels =
            static_cast<u64>(width) *
            static_cast<u64>(height);
        capture.volumeRenderPixelSteps =
            pixels *
            static_cast<u64>(capture.volume.renderer.raymarchSteps);
        capture.volumeShadowPixelSteps =
            pixels *
            static_cast<u64>(capture.volume.renderer.shadowSteps);
    }

    return capture;
}

std::string SerializeV007PerformanceCapture(
    const V007PerformanceCapture& capture)
{
    const auto& environment = capture.environment;
    const auto& lighting = capture.lighting;
    const auto& cache = capture.radianceCache;
    const auto& volume = capture.volume;

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6);
    stream << "{\n";
    stream << "  \"schema\": \"orbit.v0.0.7.performance.v1\",\n";
    stream << "  \"environment\": {\n";
    stream << "    \"adapter\": \"" << JsonEscape(environment.adapter) << "\",\n";
    stream << "    \"api\": \"" << JsonEscape(environment.api) << "\",\n";
    stream << "    \"commit\": \"" << JsonEscape(environment.commit) << "\",\n";
    stream << "    \"scenario\": \"" << JsonEscape(environment.scenario) << "\",\n";
    stream << "    \"settings\": \"" << JsonEscape(environment.settings) << "\",\n";
    stream << "    \"width\": " << environment.width << ",\n";
    stream << "    \"height\": " << environment.height << ",\n";
    stream << "    \"capabilities\": {\n";
    stream << "      \"acceleration_structures\": " << Boolean(environment.accelerationStructures) << ",\n";
    stream << "      \"ray_query\": " << Boolean(environment.rayQuery) << ",\n";
    stream << "      \"ray_tracing_pipeline\": " << Boolean(environment.rayTracingPipeline) << ",\n";
    stream << "      \"mesh_shaders\": " << Boolean(environment.meshShaders) << ",\n";
    stream << "      \"variable_rate_shading\": " << Boolean(environment.variableRateShading) << "\n";
    stream << "    }\n";
    stream << "  },\n";

    stream << "  \"lighting\": {\n";
    stream << "    \"has_plan\": " << Boolean(lighting.hasScheduledPlan) << ",\n";
    stream << "    \"has_measured_timings\": " << Boolean(lighting.hasMeasuredTimings) << ",\n";
    stream << "    \"total_budget_ms\": " << lighting.config.budget.TotalMs() << ",\n";
    stream << "    \"hardware_ray_query_policy\": " << Boolean(lighting.config.hardwareRayQueryEnabled) << ",\n";
    stream << "    \"hardware_ray_query_available\": " << Boolean(lighting.scheduled.hardwareRayQueryAvailable) << ",\n";
    stream << "    \"hardware_ray_query_selected\": " << Boolean(lighting.scheduled.preferHardwareRayQuery) << ",\n";
    stream << "    \"requested_exact_queries\": " << lighting.requested.exactVisibilityQueries << ",\n";
    stream << "    \"scheduled_exact_queries\": " << lighting.scheduled.exactVisibilityQueries << ",\n";
    stream << "    \"requested_radiance_updates\": " << lighting.requested.radianceCacheUpdates << ",\n";
    stream << "    \"scheduled_radiance_updates\": " << lighting.scheduled.radianceCacheUpdates << ",\n";
    stream << "    \"requested_reflection_queries\": " << lighting.requested.reflectionQueries << ",\n";
    stream << "    \"scheduled_reflection_queries\": " << lighting.scheduled.reflectionQueries << ",\n";
    stream << "    \"requested_emissive_updates\": " << lighting.requested.emissiveUpdates << ",\n";
    stream << "    \"scheduled_emissive_updates\": " << lighting.scheduled.emissiveUpdates << ",\n";
    stream << "    \"sections\": {\n";
    WriteLightingSection(stream, lighting, lighting::LightingGpuSection::Direct, "direct", true);
    WriteLightingSection(stream, lighting, lighting::LightingGpuSection::Visibility, "visibility", true);
    WriteLightingSection(stream, lighting, lighting::LightingGpuSection::Gi, "gi", true);
    WriteLightingSection(stream, lighting, lighting::LightingGpuSection::Reflections, "reflections", true);
    WriteLightingSection(stream, lighting, lighting::LightingGpuSection::Emissive, "emissive", true);
    WriteLightingSection(stream, lighting, lighting::LightingGpuSection::PostProcess, "post_process", false);
    stream << "    }\n";
    stream << "  },\n";

    stream << "  \"radiance_cache\": {\n";
    stream << "    \"available\": " << Boolean(cache.available) << ",\n";
    stream << "    \"resident_cells\": " << cache.residentCells << ",\n";
    stream << "    \"dirty_cells\": " << cache.dirtyCells << ",\n";
    stream << "    \"total_cells\": " << cache.totalCells << ",\n";
    stream << "    \"gpu_bytes\": " << cache.gpuBytes << ",\n";
    stream << "    \"scheduled_updates\": " << cache.scheduledUpdates << "\n";
    stream << "  },\n";

    stream << "  \"volume\": {\n";
    stream << "    \"selected\": " << Boolean(volume.hasSelection) << ",\n";
    stream << "    \"has_fields\": " << Boolean(volume.hasFields) << ",\n";
    stream << "    \"field_bytes\": " << (volume.hasFields ? volume.fields.totalBytes : 0U) << ",\n";
    stream << "    \"resident_tiles\": " << (volume.hasFields ? volume.fields.residentTiles : 0U) << ",\n";
    stream << "    \"solver_timing_valid\": " << Boolean(volume.hasSolver && volume.solver.gpuTimingValid) << ",\n";
    stream << "    \"solver_gpu_ms\": " << (volume.hasSolver && volume.solver.gpuTimingValid ? volume.solver.gpuMilliseconds : 0.0F) << ",\n";
    stream << "    \"solver_budget_ms\": " << (volume.hasSolver ? volume.solver.gpuBudgetMilliseconds : 0.0F) << ",\n";
    stream << "    \"solver_requested_iterations\": " << (volume.hasSolver ? volume.solver.requestedIterations : 0U) << ",\n";
    stream << "    \"solver_executed_iterations\": " << (volume.hasSolver ? volume.solver.iterationsThisFrame : 0U) << ",\n";
    stream << "    \"rendered\": " << Boolean(volume.hasRenderer && volume.renderer.rendered) << ",\n";
    stream << "    \"representation\": \""
           << (volume.hasRenderer ? V007RepresentationName(volume.renderer.representation) : "Unavailable")
           << "\",\n";
    stream << "    \"dense_field_required\": " << Boolean(volume.hasRenderer && volume.renderer.denseFieldRequired) << ",\n";
    stream << "    \"raymarch_steps\": " << (volume.hasRenderer ? volume.renderer.raymarchSteps : 0U) << ",\n";
    stream << "    \"shadow_steps\": " << (volume.hasRenderer ? volume.renderer.shadowSteps : 0U) << ",\n";
    stream << "    \"projected_pixels\": " << (volume.hasRenderer ? volume.renderer.projectedDiameterPixels : 0.0F) << ",\n";
    stream << "    \"distance_to_bounds_m\": " << (volume.hasRenderer ? volume.renderer.distanceToBoundsMeters : 0.0) << ",\n";
    stream << "    \"render_pixel_steps\": " << capture.volumeRenderPixelSteps << ",\n";
    stream << "    \"shadow_pixel_steps\": " << capture.volumeShadowPixelSteps << ",\n";
    stream << "    \"render_history_bytes\": " << (volume.hasRenderer ? volume.renderer.historyBytes : 0U) << ",\n";
    stream << "    \"render_scratch_bytes\": " << (volume.hasRenderer ? volume.renderer.scratchBytes : 0U) << "\n";
    stream << "  }\n";
    stream << "}\n";
    return stream.str();
}

bool SaveV007PerformanceCapture(
    const std::string_view path,
    const V007PerformanceCapture& capture,
    std::string* const error)
{
    try
    {
        const std::filesystem::path output(path);
        if (output.has_parent_path())
        {
            std::filesystem::create_directories(output.parent_path());
        }

        const auto temporary = output.string() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                if (error != nullptr) *error = "Unable to open performance capture for writing.";
                return false;
            }
            const std::string text = SerializeV007PerformanceCapture(capture);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream)
            {
                if (error != nullptr) *error = "Performance capture write failed.";
                return false;
            }
        }

        std::error_code ec;
        std::filesystem::remove(output, ec);
        ec.clear();
        std::filesystem::rename(temporary, output, ec);
        if (ec)
        {
            if (error != nullptr) *error = ec.message();
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

V007PerformancePairValidation ValidateV007RtAutoPair(
    const V007PerformanceCapture& nonRt,
    const V007PerformanceCapture& rt)
{
    if (!nonRt.lighting.hasScheduledPlan || !rt.lighting.hasScheduledPlan)
    {
        return {false, "Both captures require a published lighting work plan."};
    }
    if (!SameBudget(nonRt.lighting.config.budget, rt.lighting.config.budget))
    {
        return {false, "RT capability changed the configured lighting budget."};
    }
    if (nonRt.lighting.scheduled.exactVisibilityQueries != rt.lighting.scheduled.exactVisibilityQueries ||
        nonRt.lighting.scheduled.radianceCacheUpdates != rt.lighting.scheduled.radianceCacheUpdates ||
        nonRt.lighting.scheduled.reflectionQueries != rt.lighting.scheduled.reflectionQueries ||
        nonRt.lighting.scheduled.emissiveUpdates != rt.lighting.scheduled.emissiveUpdates)
    {
        return {false, "RT capability changed scheduled work counts instead of only the visibility backend."};
    }
    if (nonRt.lighting.scheduled.preferHardwareRayQuery)
    {
        return {false, "Non-RT capture unexpectedly selected hardware ray query."};
    }
    if (!rt.environment.rayQuery || !rt.lighting.scheduled.preferHardwareRayQuery)
    {
        return {false, "RT-capable capture did not select the hardware ray-query backend."};
    }
    return {true, "RT uses the shared work plan and budget while selecting the hardware exact-query backend."};
}

V007PerformancePairValidation ValidateV007VolumeAutoPair(
    const V007PerformanceCapture& nearCapture,
    const V007PerformanceCapture& farCapture)
{
    if (!nearCapture.volume.hasRenderer || !farCapture.volume.hasRenderer)
    {
        return {false, "Both captures require selected-volume renderer diagnostics."};
    }
    if (nearCapture.volume.renderer.representation !=
            volume_representation::ResolvedRepresentation::Live)
    {
        return {false, "Near capture is not using the Live representation."};
    }
    if (farCapture.volume.renderer.representation ==
            volume_representation::ResolvedRepresentation::Live)
    {
        return {false, "Far capture failed to demote from Live."};
    }
    if (farCapture.volumeRenderPixelSteps > nearCapture.volumeRenderPixelSteps)
    {
        return {false, "Far representation increased volume render work."};
    }
    if (nearCapture.volume.hasSolver && farCapture.volume.hasSolver &&
        farCapture.volume.solver.iterationsThisFrame > nearCapture.volume.solver.iterationsThisFrame)
    {
        return {false, "Far representation increased live solver iterations."};
    }
    if (nearCapture.volume.hasFields && farCapture.volume.hasFields &&
        farCapture.volume.fields.totalBytes > nearCapture.volume.fields.totalBytes)
    {
        return {false, "Far representation increased resident volume field memory."};
    }
    return {true, "Volume Auto demoted representation without increasing render, simulation, or resident-field work."};
}

std::string_view V007RepresentationName(
    const volume_representation::ResolvedRepresentation representation) noexcept
{
    return volume_representation::ResolvedRepresentationName(representation);
}
} // namespace orbit::studio_ui
