#include <orbit/terrain_erosion/MultiScaleTerrain.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_erosion
{
namespace
{
[[nodiscard]] constexpr std::size_t ScaleIndex(
    const PhysicalTerrainScale scale) noexcept
{
    return static_cast<std::size_t>(scale);
}

[[nodiscard]] bool FinitePositive(const f64 value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool FiniteNonNegative(const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool NearlyEqual(
    const f64 a,
    const f64 b) noexcept
{
    const f64 magnitude =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= magnitude * 1.0e-12;
}

[[nodiscard]] MultiScaleTerrainProcessMask Mask(
    const std::initializer_list<MultiScaleTerrainProcess> processes) noexcept
{
    MultiScaleTerrainProcessMask mask = 0;
    for (const auto process : processes)
    {
        mask |= ProcessBit(process);
    }
    return mask;
}
} // namespace

bool PhysicalTerrainScaleLevel::IsValid() const noexcept
{
    if (ScaleIndex(scale) >= kPhysicalTerrainScaleCount ||
        !FinitePositive(sampleSpacingMeters) ||
        !FinitePositive(minimumFeatureWavelengthMeters) ||
        !FinitePositive(maximumFeatureWavelengthMeters) ||
        minimumFeatureWavelengthMeters >=
            maximumFeatureWavelengthMeters ||
        processMask == 0)
    {
        return false;
    }

    // A physical solver tier must have enough support to represent the
    // shortest wavelength it owns. This keeps the hierarchy consistent with
    // TerrainSampleFootprint::MinimumResolvedWavelengthMeters().
    return sampleSpacingMeters * 2.0 <=
        minimumFeatureWavelengthMeters * (1.0 + 1.0e-12);
}

bool PhysicalTerrainScaleLevel::OwnsFeatureWavelength(
    const f64 wavelengthMeters) const noexcept
{
    if (!std::isfinite(wavelengthMeters) ||
        wavelengthMeters <= 0.0)
    {
        return false;
    }

    return wavelengthMeters >= minimumFeatureWavelengthMeters &&
           wavelengthMeters < maximumFeatureWavelengthMeters;
}

bool MultiScaleTerrainProfile::IsValid() const noexcept
{
    for (std::size_t index = 0;
         index < levels.size();
         ++index)
    {
        const auto& level = levels[index];
        if (!level.IsValid() ||
            ScaleIndex(level.scale) != index)
        {
            return false;
        }

        if (index == 0)
        {
            continue;
        }

        const auto& coarser = levels[index - 1U];

        if (level.sampleSpacingMeters >=
            coarser.sampleSpacingMeters)
        {
            return false;
        }

        // Spectral shells must touch but never overlap. Refining physical
        // detail therefore adds a new frequency shell instead of changing the
        // macro shell already solved.
        if (!NearlyEqual(
                level.maximumFeatureWavelengthMeters,
                coarser.minimumFeatureWavelengthMeters))
        {
            return false;
        }
    }

    return true;
}

MultiScaleTerrainProfile
DefaultMultiScaleTerrainProfile() noexcept
{
    MultiScaleTerrainProfile profile{};

    profile.levels[0] = {
        .scale = PhysicalTerrainScale::Macro,
        .sampleSpacingMeters = 500.0,
        .minimumFeatureWavelengthMeters = 1'000.0,
        .maximumFeatureWavelengthMeters = 100'000'000.0,
        .processMask = Mask({
            MultiScaleTerrainProcess::GeologyUplift,
            MultiScaleTerrainProcess::Drainage})
    };

    profile.levels[1] = {
        .scale = PhysicalTerrainScale::Regional,
        .sampleSpacingMeters = 50.0,
        .minimumFeatureWavelengthMeters = 100.0,
        .maximumFeatureWavelengthMeters = 1'000.0,
        .processMask = Mask({
            MultiScaleTerrainProcess::StreamPower,
            MultiScaleTerrainProcess::SedimentExchange,
            MultiScaleTerrainProcess::Glacial,
            MultiScaleTerrainProcess::River,
            MultiScaleTerrainProcess::Coastal})
    };

    profile.levels[2] = {
        .scale = PhysicalTerrainScale::Local,
        .sampleSpacingMeters = 5.0,
        .minimumFeatureWavelengthMeters = 10.0,
        .maximumFeatureWavelengthMeters = 100.0,
        .processMask = Mask({
            MultiScaleTerrainProcess::Hydraulic,
            MultiScaleTerrainProcess::Aeolian,
            MultiScaleTerrainProcess::SedimentExchange,
            MultiScaleTerrainProcess::River,
            MultiScaleTerrainProcess::Coastal})
    };

    profile.levels[3] = {
        .scale = PhysicalTerrainScale::Fine,
        .sampleSpacingMeters = 0.5,
        .minimumFeatureWavelengthMeters = 1.0,
        .maximumFeatureWavelengthMeters = 10.0,
        .processMask = Mask({
            MultiScaleTerrainProcess::Hydraulic,
            MultiScaleTerrainProcess::ThermalGravity,
            MultiScaleTerrainProcess::SedimentExchange,
            MultiScaleTerrainProcess::Coastal})
    };

    profile.levels[4] = {
        .scale = PhysicalTerrainScale::Micro,
        .sampleSpacingMeters = 0.1,
        .minimumFeatureWavelengthMeters = 0.2,
        .maximumFeatureWavelengthMeters = 1.0,
        .processMask = Mask({
            MultiScaleTerrainProcess::ProceduralDetail})
    };

    return profile;
}

bool PhysicalTerrainSelection::Includes(
    const PhysicalTerrainScale scale) const noexcept
{
    const std::size_t index = ScaleIndex(scale);
    return index < kPhysicalTerrainScaleCount &&
           index < activeLevelCount;
}

bool PhysicalTerrainSelection::Allows(
    const MultiScaleTerrainProcess process) const noexcept
{
    return
        (activeProcessMask & ProcessBit(process)) != 0;
}

void MultiScaleTerrainTelemetry::Record(
    const PhysicalTerrainScale scale,
    const u64 processedCells,
    const f64 cpuMilliseconds,
    const f64 gpuMilliseconds)
{
    const std::size_t index = ScaleIndex(scale);
    if (index >= stats_.size())
    {
        throw std::invalid_argument(
            "M23 telemetry scale is invalid.");
    }

    if (!FiniteNonNegative(cpuMilliseconds) ||
        !FiniteNonNegative(gpuMilliseconds))
    {
        throw std::invalid_argument(
            "M23 telemetry timings must be finite and non-negative.");
    }

    auto& stats = stats_[index];
    ++stats.executions;
    stats.processedCells += processedCells;
    stats.cpuMilliseconds += cpuMilliseconds;
    stats.gpuMilliseconds += gpuMilliseconds;
}

const PhysicalTerrainScaleStats&
MultiScaleTerrainTelemetry::Stats(
    const PhysicalTerrainScale scale) const
{
    const std::size_t index = ScaleIndex(scale);
    if (index >= stats_.size())
    {
        throw std::invalid_argument(
            "M23 telemetry scale is invalid.");
    }

    return stats_[index];
}

void MultiScaleTerrainTelemetry::Reset() noexcept
{
    stats_ = {};
}

MultiScaleTerrainPlanner::MultiScaleTerrainPlanner(
    MultiScaleTerrainProfile profile)
    : profile_(std::move(profile))
{
    if (!profile_.IsValid())
    {
        throw std::invalid_argument(
            "M23 multi-scale terrain profile is invalid.");
    }
}

const MultiScaleTerrainProfile&
MultiScaleTerrainPlanner::Profile() const noexcept
{
    return profile_;
}

const PhysicalTerrainScaleLevel&
MultiScaleTerrainPlanner::Level(
    const PhysicalTerrainScale scale) const
{
    const std::size_t index = ScaleIndex(scale);
    if (index >= profile_.levels.size())
    {
        throw std::invalid_argument(
            "M23 physical terrain scale is invalid.");
    }

    return profile_.levels[index];
}

PhysicalTerrainSelection MultiScaleTerrainPlanner::Select(
    const f64 requestedSampleSpacingMeters) const
{
    if (!FinitePositive(requestedSampleSpacingMeters))
    {
        throw std::invalid_argument(
            "M23 physical sample spacing must be finite and positive.");
    }

    PhysicalTerrainSelection selection{
        .requestedSampleSpacingMeters =
            requestedSampleSpacingMeters,
        .activeLevelCount = 1,
        .finestScale = PhysicalTerrainScale::Macro,
        .activeProcessMask = profile_.levels[0].processMask
    };

    for (std::size_t index = 1;
         index < profile_.levels.size();
         ++index)
    {
        const auto& level = profile_.levels[index];

        if (requestedSampleSpacingMeters >
            level.sampleSpacingMeters)
        {
            break;
        }

        selection.activeLevelCount =
            static_cast<u8>(index + 1U);
        selection.finestScale = level.scale;
        selection.activeProcessMask |= level.processMask;
    }

    return selection;
}

PhysicalTerrainSelection MultiScaleTerrainPlanner::Select(
    const terrain::TerrainSampleFootprint& footprint) const
{
    if (!footprint.IsValid())
    {
        throw std::invalid_argument(
            "M23 terrain footprint must be finite and positive.");
    }

    return Select(footprint.diameterMeters);
}

terrain::TerrainSampleFootprint
MultiScaleTerrainPlanner::FootprintFor(
    const PhysicalTerrainScale scale) const
{
    return {
        .diameterMeters = Level(scale).sampleSpacingMeters
    };
}

PhysicalTerrainScale
MultiScaleTerrainPlanner::ScaleForFeatureWavelength(
    const f64 wavelengthMeters) const noexcept
{
    for (const auto& level : profile_.levels)
    {
        if (level.OwnsFeatureWavelength(wavelengthMeters))
        {
            return level.scale;
        }
    }

    return PhysicalTerrainScale::Count;
}

u64 MultiScaleTerrainPlanner::StableScaleKey(
    const PhysicalTerrainScale scale,
    const u64 planetRootSeed,
    const u64 scaleRevision) const
{
    const auto& level = Level(scale);

    u64 value = terrain::StableCombine64(
        0x4D32335343414C45ULL, // "M23SCALE"
        planetRootSeed);
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(scale));
    value = terrain::StableCombine64(
        value,
        scaleRevision);
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(level.sampleSpacingMeters));
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(
            level.minimumFeatureWavelengthMeters));
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(
            level.maximumFeatureWavelengthMeters));
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(level.processMask));
    return value;
}
} // namespace orbit::terrain_erosion
