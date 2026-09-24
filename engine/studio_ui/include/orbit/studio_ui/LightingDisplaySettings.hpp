#pragma once

#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/post_process/ColorLut.hpp>
#include <orbit/post_process/HighlightEffects.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>
#include <orbit/post_process/LuminanceHistogram.hpp>
#include <orbit/post_process/OutputTransform.hpp>
#include <orbit/post_process/ToneMapping.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
inline constexpr u32 kLightingDisplaySettingsSchemaVersion = 1U;

struct StudioDisplayDefaults
{
    post_process::LuminanceHistogramConfig histogram{};
    post_process::HumanEyeAdaptationConfig eye{};
    post_process::HighlightEffectsConfig highlights{};
    post_process::ToneMappingConfig toneMapping{};
    post_process::ColorLutSettings colorLut{};
    post_process::OutputTransformSettings output{};
    std::string colorLutAsset;
};

struct LightingDisplaySettings
{
    u32 schemaVersion{kLightingDisplaySettingsSchemaVersion};
    lighting::LightingSchedulerConfig lighting{};
    StudioDisplayDefaults display{};
};

struct PublishedStudioDisplayDefaults
{
    StudioDisplayDefaults settings{};
    u64 revision{0U};
};

namespace detail
{
[[nodiscard]] inline std::filesystem::path
LightingDisplaySettingsPath(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / "Config" /
        "LightingDisplay.orbitcfg";
}

[[nodiscard]] inline std::map<std::string, std::string>
ReadKeyValues(const std::filesystem::path& path)
{
    std::map<std::string, std::string> values;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return values;
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        const auto split = line.find('=');
        if (split == std::string::npos)
        {
            continue;
        }

        values.insert_or_assign(
            line.substr(0U, split),
            line.substr(split + 1U));
    }
    return values;
}

template <typename T>
[[nodiscard]] inline T ReadNumber(
    const std::map<std::string, std::string>& values,
    const std::string_view key,
    const T fallback) noexcept
{
    const auto found = values.find(std::string(key));
    if (found == values.end())
    {
        return fallback;
    }

    T value{};
    const auto* first = found->second.data();
    const auto* last = first + found->second.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last
        ? value
        : fallback;
}

[[nodiscard]] inline f32 ReadFloat(
    const std::map<std::string, std::string>& values,
    const std::string_view key,
    const f32 fallback) noexcept
{
    const auto found = values.find(std::string(key));
    if (found == values.end())
    {
        return fallback;
    }

    try
    {
        return std::stof(found->second);
    }
    catch (...)
    {
        return fallback;
    }
}

[[nodiscard]] inline bool ReadBool(
    const std::map<std::string, std::string>& values,
    const std::string_view key,
    const bool fallback) noexcept
{
    const auto found = values.find(std::string(key));
    if (found == values.end())
    {
        return fallback;
    }
    return found->second == "1" ||
        found->second == "true"
        ? true
        : found->second == "0" ||
              found->second == "false"
            ? false
            : fallback;
}

inline void WriteBool(
    std::ostream& output,
    const std::string_view key,
    const bool value)
{
    output << key << '=' << (value ? 1 : 0) << '\n';
}

template <typename T>
inline void WriteNumber(
    std::ostream& output,
    const std::string_view key,
    const T value)
{
    output << key << '=' << value << '\n';
}

[[nodiscard]] inline PublishedStudioDisplayDefaults&
PublishedDefaultsStorage() noexcept
{
    static PublishedStudioDisplayDefaults defaults{};
    return defaults;
}
} // namespace detail

[[nodiscard]] inline LightingDisplaySettings
LoadLightingDisplaySettings(
    const std::filesystem::path& projectRoot)
{
    LightingDisplaySettings result{};
    const auto values = detail::ReadKeyValues(
        detail::LightingDisplaySettingsPath(projectRoot));

    if (values.empty())
    {
        return result;
    }

    result.schemaVersion = detail::ReadNumber<u32>(
        values, "schema_version", result.schemaVersion);

    auto& lighting = result.lighting;
    lighting.hardwareRayQueryEnabled = detail::ReadBool(
        values, "lighting.hardware_ray_query_enabled",
        lighting.hardwareRayQueryEnabled);
    lighting.emissiveGiQualityScale = detail::ReadFloat(
        values, "lighting.emissive_gi_quality",
        lighting.emissiveGiQualityScale);
    lighting.budget.directLightingMs = detail::ReadFloat(
        values, "lighting.budget.direct_ms",
        lighting.budget.directLightingMs);
    lighting.budget.visibilityMs = detail::ReadFloat(
        values, "lighting.budget.visibility_ms",
        lighting.budget.visibilityMs);
    lighting.budget.giMs = detail::ReadFloat(
        values, "lighting.budget.gi_ms",
        lighting.budget.giMs);
    lighting.budget.reflectionMs = detail::ReadFloat(
        values, "lighting.budget.reflection_ms",
        lighting.budget.reflectionMs);
    lighting.budget.emissiveMs = detail::ReadFloat(
        values, "lighting.budget.emissive_ms",
        lighting.budget.emissiveMs);
    lighting.budget.postProcessMs = detail::ReadFloat(
        values, "lighting.budget.post_ms",
        lighting.budget.postProcessMs);
    lighting.minimumVisibilityScale = detail::ReadFloat(
        values, "lighting.minimum.visibility",
        lighting.minimumVisibilityScale);
    lighting.minimumGiScale = detail::ReadFloat(
        values, "lighting.minimum.gi",
        lighting.minimumGiScale);
    lighting.minimumReflectionScale = detail::ReadFloat(
        values, "lighting.minimum.reflection",
        lighting.minimumReflectionScale);
    lighting.minimumEmissiveScale = detail::ReadFloat(
        values, "lighting.minimum.emissive",
        lighting.minimumEmissiveScale);
    lighting.overloadResponse = detail::ReadFloat(
        values, "lighting.response.overload",
        lighting.overloadResponse);
    lighting.recoveryResponse = detail::ReadFloat(
        values, "lighting.response.recovery",
        lighting.recoveryResponse);
    lighting.hardwareRayQueryPreferenceThreshold = detail::ReadFloat(
        values, "lighting.hardware.preference_threshold",
        lighting.hardwareRayQueryPreferenceThreshold);

    auto& display = result.display;
    display.histogram.minimumLog2 = detail::ReadFloat(
        values, "display.histogram.minimum_log2",
        display.histogram.minimumLog2);
    display.histogram.maximumLog2 = detail::ReadFloat(
        values, "display.histogram.maximum_log2",
        display.histogram.maximumLog2);
    display.histogram.centerWeightStrength = detail::ReadFloat(
        values, "display.histogram.center_weight",
        display.histogram.centerWeightStrength);
    display.histogram.centerWeightRadius = detail::ReadFloat(
        values, "display.histogram.center_radius",
        display.histogram.centerWeightRadius);

    display.eye.photopicP50Weight = detail::ReadFloat(values, "display.eye.p50_weight", display.eye.photopicP50Weight);
    display.eye.photopicP95Weight = detail::ReadFloat(values, "display.eye.p95_weight", display.eye.photopicP95Weight);
    display.eye.photopicBrightenSeconds = detail::ReadFloat(values, "display.eye.brighten_seconds", display.eye.photopicBrightenSeconds);
    display.eye.photopicDarkenSeconds = detail::ReadFloat(values, "display.eye.darken_seconds", display.eye.photopicDarkenSeconds);
    display.eye.photopicCeilingLog2 = detail::ReadFloat(values, "display.eye.photopic_ceiling_log2", display.eye.photopicCeilingLog2);
    display.eye.photopicCeilingRecoverySeconds = detail::ReadFloat(values, "display.eye.ceiling_recovery_seconds", display.eye.photopicCeilingRecoverySeconds);
    display.eye.exposureMiddleGray = detail::ReadFloat(values, "display.eye.middle_gray", display.eye.exposureMiddleGray);
    display.eye.minimumExposureScale = detail::ReadFloat(values, "display.eye.minimum_exposure", display.eye.minimumExposureScale);
    display.eye.maximumExposureScale = detail::ReadFloat(values, "display.eye.maximum_exposure", display.eye.maximumExposureScale);
    display.eye.darkThresholdLog2 = detail::ReadFloat(values, "display.eye.dark_threshold_log2", display.eye.darkThresholdLog2);
    display.eye.darkFullLog2 = detail::ReadFloat(values, "display.eye.dark_full_log2", display.eye.darkFullLog2);
    display.eye.darkAdaptSeconds = detail::ReadFloat(values, "display.eye.dark_adapt_seconds", display.eye.darkAdaptSeconds);
    display.eye.darkResetSeconds = detail::ReadFloat(values, "display.eye.dark_reset_seconds", display.eye.darkResetSeconds);
    display.eye.overloadP99StartStops = detail::ReadFloat(values, "display.eye.overload_p99_start", display.eye.overloadP99StartStops);
    display.eye.overloadPeakStartStops = detail::ReadFloat(values, "display.eye.overload_peak_start", display.eye.overloadPeakStartStops);
    display.eye.overloadSoftRangeStops = detail::ReadFloat(values, "display.eye.overload_range", display.eye.overloadSoftRangeStops);
    display.eye.overloadAttackSeconds = detail::ReadFloat(values, "display.eye.overload_attack_seconds", display.eye.overloadAttackSeconds);
    display.eye.overloadRecoverySeconds = detail::ReadFloat(values, "display.eye.overload_recovery_seconds", display.eye.overloadRecoverySeconds);

    display.highlights.bloomEnabled = detail::ReadBool(values, "display.highlights.bloom_enabled", display.highlights.bloomEnabled);
    display.highlights.glareEnabled = detail::ReadBool(values, "display.highlights.glare_enabled", display.highlights.glareEnabled);
    display.highlights.flareEnabled = detail::ReadBool(values, "display.highlights.flare_enabled", display.highlights.flareEnabled);
    display.highlights.bloomThreshold = detail::ReadFloat(values, "display.highlights.bloom_threshold", display.highlights.bloomThreshold);
    display.highlights.bloomKnee = detail::ReadFloat(values, "display.highlights.bloom_knee", display.highlights.bloomKnee);
    display.highlights.bloomStrength = detail::ReadFloat(values, "display.highlights.bloom_strength", display.highlights.bloomStrength);
    display.highlights.bloomRadiusPixels = detail::ReadFloat(values, "display.highlights.bloom_radius", display.highlights.bloomRadiusPixels);
    display.highlights.glareThreshold = detail::ReadFloat(values, "display.highlights.glare_threshold", display.highlights.glareThreshold);
    display.highlights.glareStrength = detail::ReadFloat(values, "display.highlights.glare_strength", display.highlights.glareStrength);
    display.highlights.glareRadiusPixels = detail::ReadFloat(values, "display.highlights.glare_radius", display.highlights.glareRadiusPixels);
    display.highlights.flareThreshold = detail::ReadFloat(values, "display.highlights.flare_threshold", display.highlights.flareThreshold);
    display.highlights.flareStrength = detail::ReadFloat(values, "display.highlights.flare_strength", display.highlights.flareStrength);
    display.highlights.flareCompactness = detail::ReadFloat(values, "display.highlights.flare_compactness", display.highlights.flareCompactness);
    display.highlights.flareGhostScale = detail::ReadFloat(values, "display.highlights.flare_ghost_scale", display.highlights.flareGhostScale);

    display.toneMapping.enabled = detail::ReadBool(values, "display.tonemap.enabled", display.toneMapping.enabled);
    display.toneMapping.referenceWhiteNits = detail::ReadFloat(values, "display.tonemap.reference_white_nits", display.toneMapping.referenceWhiteNits);
    display.toneMapping.peakNits = detail::ReadFloat(values, "display.tonemap.peak_nits", display.toneMapping.peakNits);
    display.toneMapping.shoulderStart = detail::ReadFloat(values, "display.tonemap.shoulder_start", display.toneMapping.shoulderStart);
    display.toneMapping.shoulderStrength = detail::ReadFloat(values, "display.tonemap.shoulder_strength", display.toneMapping.shoulderStrength);

    display.colorLut.enabled = detail::ReadBool(values, "display.lut.enabled", display.colorLut.enabled);
    display.colorLut.strength = detail::ReadFloat(values, "display.lut.strength", display.colorLut.strength);
    if (const auto found = values.find("display.lut.asset"); found != values.end())
    {
        display.colorLutAsset = found->second;
    }

    display.output.mode = static_cast<post_process::OutputMode>(
        detail::ReadNumber<u32>(values, "display.output.mode", static_cast<u32>(display.output.mode)));
    display.output.referenceWhiteNits = detail::ReadFloat(values, "display.output.reference_white_nits", display.output.referenceWhiteNits);
    display.output.requestedPeakNits = detail::ReadFloat(values, "display.output.requested_peak_nits", display.output.requestedPeakNits);
    display.output.testPattern = static_cast<post_process::OutputTestPattern>(
        detail::ReadNumber<u32>(values, "display.output.test_pattern", static_cast<u32>(display.output.testPattern)));

    return result;
}

inline void SaveLightingDisplaySettings(
    const std::filesystem::path& projectRoot,
    const LightingDisplaySettings& settings)
{
    const auto path = detail::LightingDisplaySettingsPath(projectRoot);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("Failed to create LightingDisplay.orbitcfg.");
    }

    output << "# Orbit V0.0.7 M40 lighting/display project defaults\n";
    detail::WriteNumber(output, "schema_version", settings.schemaVersion);

    const auto& lighting = settings.lighting;
    detail::WriteBool(output, "lighting.hardware_ray_query_enabled", lighting.hardwareRayQueryEnabled);
    detail::WriteNumber(output, "lighting.emissive_gi_quality", lighting.emissiveGiQualityScale);
    detail::WriteNumber(output, "lighting.budget.direct_ms", lighting.budget.directLightingMs);
    detail::WriteNumber(output, "lighting.budget.visibility_ms", lighting.budget.visibilityMs);
    detail::WriteNumber(output, "lighting.budget.gi_ms", lighting.budget.giMs);
    detail::WriteNumber(output, "lighting.budget.reflection_ms", lighting.budget.reflectionMs);
    detail::WriteNumber(output, "lighting.budget.emissive_ms", lighting.budget.emissiveMs);
    detail::WriteNumber(output, "lighting.budget.post_ms", lighting.budget.postProcessMs);
    detail::WriteNumber(output, "lighting.minimum.visibility", lighting.minimumVisibilityScale);
    detail::WriteNumber(output, "lighting.minimum.gi", lighting.minimumGiScale);
    detail::WriteNumber(output, "lighting.minimum.reflection", lighting.minimumReflectionScale);
    detail::WriteNumber(output, "lighting.minimum.emissive", lighting.minimumEmissiveScale);
    detail::WriteNumber(output, "lighting.response.overload", lighting.overloadResponse);
    detail::WriteNumber(output, "lighting.response.recovery", lighting.recoveryResponse);
    detail::WriteNumber(output, "lighting.hardware.preference_threshold", lighting.hardwareRayQueryPreferenceThreshold);

    const auto& d = settings.display;
    detail::WriteNumber(output, "display.histogram.minimum_log2", d.histogram.minimumLog2);
    detail::WriteNumber(output, "display.histogram.maximum_log2", d.histogram.maximumLog2);
    detail::WriteNumber(output, "display.histogram.center_weight", d.histogram.centerWeightStrength);
    detail::WriteNumber(output, "display.histogram.center_radius", d.histogram.centerWeightRadius);

    detail::WriteNumber(output, "display.eye.p50_weight", d.eye.photopicP50Weight);
    detail::WriteNumber(output, "display.eye.p95_weight", d.eye.photopicP95Weight);
    detail::WriteNumber(output, "display.eye.brighten_seconds", d.eye.photopicBrightenSeconds);
    detail::WriteNumber(output, "display.eye.darken_seconds", d.eye.photopicDarkenSeconds);
    detail::WriteNumber(output, "display.eye.photopic_ceiling_log2", d.eye.photopicCeilingLog2);
    detail::WriteNumber(output, "display.eye.ceiling_recovery_seconds", d.eye.photopicCeilingRecoverySeconds);
    detail::WriteNumber(output, "display.eye.middle_gray", d.eye.exposureMiddleGray);
    detail::WriteNumber(output, "display.eye.minimum_exposure", d.eye.minimumExposureScale);
    detail::WriteNumber(output, "display.eye.maximum_exposure", d.eye.maximumExposureScale);
    detail::WriteNumber(output, "display.eye.dark_threshold_log2", d.eye.darkThresholdLog2);
    detail::WriteNumber(output, "display.eye.dark_full_log2", d.eye.darkFullLog2);
    detail::WriteNumber(output, "display.eye.dark_adapt_seconds", d.eye.darkAdaptSeconds);
    detail::WriteNumber(output, "display.eye.dark_reset_seconds", d.eye.darkResetSeconds);
    detail::WriteNumber(output, "display.eye.overload_p99_start", d.eye.overloadP99StartStops);
    detail::WriteNumber(output, "display.eye.overload_peak_start", d.eye.overloadPeakStartStops);
    detail::WriteNumber(output, "display.eye.overload_range", d.eye.overloadSoftRangeStops);
    detail::WriteNumber(output, "display.eye.overload_attack_seconds", d.eye.overloadAttackSeconds);
    detail::WriteNumber(output, "display.eye.overload_recovery_seconds", d.eye.overloadRecoverySeconds);

    detail::WriteBool(output, "display.highlights.bloom_enabled", d.highlights.bloomEnabled);
    detail::WriteBool(output, "display.highlights.glare_enabled", d.highlights.glareEnabled);
    detail::WriteBool(output, "display.highlights.flare_enabled", d.highlights.flareEnabled);
    detail::WriteNumber(output, "display.highlights.bloom_threshold", d.highlights.bloomThreshold);
    detail::WriteNumber(output, "display.highlights.bloom_knee", d.highlights.bloomKnee);
    detail::WriteNumber(output, "display.highlights.bloom_strength", d.highlights.bloomStrength);
    detail::WriteNumber(output, "display.highlights.bloom_radius", d.highlights.bloomRadiusPixels);
    detail::WriteNumber(output, "display.highlights.glare_threshold", d.highlights.glareThreshold);
    detail::WriteNumber(output, "display.highlights.glare_strength", d.highlights.glareStrength);
    detail::WriteNumber(output, "display.highlights.glare_radius", d.highlights.glareRadiusPixels);
    detail::WriteNumber(output, "display.highlights.flare_threshold", d.highlights.flareThreshold);
    detail::WriteNumber(output, "display.highlights.flare_strength", d.highlights.flareStrength);
    detail::WriteNumber(output, "display.highlights.flare_compactness", d.highlights.flareCompactness);
    detail::WriteNumber(output, "display.highlights.flare_ghost_scale", d.highlights.flareGhostScale);

    detail::WriteBool(output, "display.tonemap.enabled", d.toneMapping.enabled);
    detail::WriteNumber(output, "display.tonemap.reference_white_nits", d.toneMapping.referenceWhiteNits);
    detail::WriteNumber(output, "display.tonemap.peak_nits", d.toneMapping.peakNits);
    detail::WriteNumber(output, "display.tonemap.shoulder_start", d.toneMapping.shoulderStart);
    detail::WriteNumber(output, "display.tonemap.shoulder_strength", d.toneMapping.shoulderStrength);

    detail::WriteBool(output, "display.lut.enabled", d.colorLut.enabled);
    detail::WriteNumber(output, "display.lut.strength", d.colorLut.strength);
    output << "display.lut.asset=" << d.colorLutAsset << '\n';

    detail::WriteNumber(output, "display.output.mode", static_cast<u32>(d.output.mode));
    detail::WriteNumber(output, "display.output.reference_white_nits", d.output.referenceWhiteNits);
    detail::WriteNumber(output, "display.output.requested_peak_nits", d.output.requestedPeakNits);
    detail::WriteNumber(output, "display.output.test_pattern", static_cast<u32>(d.output.testPattern));

    output.flush();
    if (!output)
    {
        throw std::runtime_error("Failed while writing LightingDisplay.orbitcfg.");
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            throw std::runtime_error("Failed to replace LightingDisplay.orbitcfg.");
        }
    }
}

inline void PublishStudioDisplayDefaults(
    const StudioDisplayDefaults& settings) noexcept
{
    auto& published = detail::PublishedDefaultsStorage();
    published.settings = settings;
    ++published.revision;
}

[[nodiscard]] inline PublishedStudioDisplayDefaults
StudioDisplayDefaultsSnapshot() noexcept
{
    return detail::PublishedDefaultsStorage();
}
} // namespace orbit::studio_ui
