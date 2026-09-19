#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainPosition.hpp>

#include <array>
#include <cstddef>

namespace orbit::terrain_erosion
{
// M23 physical process tiers. These are simulation/amplification scales, not
// render clipmap levels. Their numeric values are stable array indices.
enum class PhysicalTerrainScale : u8
{
    Macro = 0,
    Regional,
    Local,
    Fine,
    Micro,
    Count
};

inline constexpr std::size_t kPhysicalTerrainScaleCount =
    static_cast<std::size_t>(PhysicalTerrainScale::Count);

// Process families are assigned to physical scales by policy. Individual
// processes remain optional; this mask describes which families are eligible
// to execute at a tier, not which systems must be enabled for a planet.
enum class MultiScaleTerrainProcess : u32
{
    None = 0,
    GeologyUplift = 1U << 0U,
    Drainage = 1U << 1U,
    StreamPower = 1U << 2U,
    Hydraulic = 1U << 3U,
    Aeolian = 1U << 4U,
    ThermalGravity = 1U << 5U,
    SedimentExchange = 1U << 6U,
    Glacial = 1U << 7U,
    River = 1U << 8U,
    Coastal = 1U << 9U,
    ProceduralDetail = 1U << 10U
};

using MultiScaleTerrainProcessMask = u32;

[[nodiscard]] constexpr MultiScaleTerrainProcessMask ProcessBit(
    const MultiScaleTerrainProcess process) noexcept
{
    return static_cast<MultiScaleTerrainProcessMask>(process);
}

struct PhysicalTerrainScaleLevel
{
    PhysicalTerrainScale scale{PhysicalTerrainScale::Macro};

    // Solver/sample support in physical meters. The public M01 footprint is
    // produced directly from this value.
    f64 sampleSpacingMeters{500.0};

    // Half-open spectral ownership [minimum, maximum), except the macro tier
    // whose maximum acts as the supported body-scale ceiling.
    f64 minimumFeatureWavelengthMeters{1'000.0};
    f64 maximumFeatureWavelengthMeters{100'000'000.0};

    MultiScaleTerrainProcessMask processMask{0};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] bool OwnsFeatureWavelength(
        f64 wavelengthMeters) const noexcept;
};

struct MultiScaleTerrainProfile
{
    std::array<
        PhysicalTerrainScaleLevel,
        kPhysicalTerrainScaleCount> levels{};

    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] MultiScaleTerrainProfile
DefaultMultiScaleTerrainProfile() noexcept;

struct PhysicalTerrainSelection
{
    f64 requestedSampleSpacingMeters{500.0};
    u8 activeLevelCount{1};
    PhysicalTerrainScale finestScale{PhysicalTerrainScale::Macro};
    MultiScaleTerrainProcessMask activeProcessMask{0};

    [[nodiscard]] bool Includes(
        PhysicalTerrainScale scale) const noexcept;

    [[nodiscard]] bool Allows(
        MultiScaleTerrainProcess process) const noexcept;
};

struct PhysicalTerrainScaleStats
{
    u64 executions{0};
    u64 processedCells{0};
    f64 cpuMilliseconds{0.0};
    f64 gpuMilliseconds{0.0};
};

class MultiScaleTerrainTelemetry
{
public:
    void Record(
        PhysicalTerrainScale scale,
        u64 processedCells,
        f64 cpuMilliseconds,
        f64 gpuMilliseconds);

    [[nodiscard]] const PhysicalTerrainScaleStats&
    Stats(PhysicalTerrainScale scale) const;

    void Reset() noexcept;

private:
    std::array<
        PhysicalTerrainScaleStats,
        kPhysicalTerrainScaleCount> stats_{};
};

class MultiScaleTerrainPlanner
{
public:
    explicit MultiScaleTerrainPlanner(
        MultiScaleTerrainProfile profile =
            DefaultMultiScaleTerrainProfile());

    [[nodiscard]] const MultiScaleTerrainProfile&
    Profile() const noexcept;

    [[nodiscard]] const PhysicalTerrainScaleLevel&
    Level(PhysicalTerrainScale scale) const;

    // Selection is based only on physical sample support. No camera,
    // viewport, clipmap ring, GPU slot or frame identifier participates.
    [[nodiscard]] PhysicalTerrainSelection Select(
        f64 requestedSampleSpacingMeters) const;

    [[nodiscard]] PhysicalTerrainSelection Select(
        const terrain::TerrainSampleFootprint& footprint) const;

    [[nodiscard]] terrain::TerrainSampleFootprint
    FootprintFor(PhysicalTerrainScale scale) const;

    [[nodiscard]] PhysicalTerrainScale ScaleForFeatureWavelength(
        f64 wavelengthMeters) const noexcept;

    // Stable scale identity deliberately excludes the requested fine-detail
    // selection. Refining a page therefore cannot re-seed or re-phase macro
    // terrain. scaleRevision is owned by dependency tracking for this tier.
    [[nodiscard]] u64 StableScaleKey(
        PhysicalTerrainScale scale,
        u64 planetRootSeed,
        u64 scaleRevision) const;

private:
    MultiScaleTerrainProfile profile_{};
};
} // namespace orbit::terrain_erosion
