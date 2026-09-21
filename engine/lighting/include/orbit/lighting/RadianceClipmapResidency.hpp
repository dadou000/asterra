#pragma once

#include <orbit/lighting/RadianceClipmap.hpp>

#include <optional>
#include <span>
#include <vector>

namespace orbit::lighting
{
struct RadianceResidentCell
{
    RadianceCellKey key{};
    RadianceCell cell{};
    u64 sourceRevision{0U};
    f32 invalidationPriorityBoost{0.0F};
    bool occupied{false};
    bool dirty{true};
};

struct RadianceUpdateCandidate
{
    RadianceCellKey key{};
    u32 physicalIndex{0U};
    f32 priority{0.0F};
    f32 ageSeconds{0.0F};
};

struct RadianceResidencyStats
{
    u64 residentCells{0U};
    u64 reusedCells{0U};
    u64 replacedCells{0U};
    u64 dirtyCells{0U};
};


struct GpuRadianceLevelInfo
{
    // xyz = center logical cell center in camera-relative GPU meters
    // w   = cell size in meters
    math::Float4 centerCellSize{};

    u32 centerModuloX{0U};
    u32 centerModuloY{0U};
    u32 centerModuloZ{0U};
    u32 cellsPerAxis{0U};

    u32 cellOffset{0U};
    u32 cellCount{0U};
    u32 level{0U};
    u32 reserved{0U};
};

static_assert(sizeof(GpuRadianceLevelInfo) == 48U);

struct RadianceGpuSnapshot
{
    u64 sourceRevision{0U};
    std::vector<GpuRadianceLevelInfo> levels;
    std::vector<GpuRadianceCell> cells;
};

class RadianceClipmapResidency
{
public:
    explicit RadianceClipmapResidency(
        RadianceClipmapConfig config);

    void Reset(
        const LightingView& view,
        const math::Double3& observerInFrameMeters,
        u64 sourceRevision);

    [[nodiscard]] RadianceResidencyStats ScrollTo(
        const LightingView& view,
        const math::Double3& observerInFrameMeters,
        u64 sourceRevision,
        f32 deltaSeconds);

    void InvalidateSphere(
        const math::Double3& centerInFrameMeters,
        f64 radiusMeters,
        u64 sourceRevision,
        f32 priorityBoost = 0.0F,
        bool preservePreviousValue = false);


    // Marks resident cells for budgeted relighting while preserving their
    // last valid value for GPU fallback until the refresh reaches them.
    void RequestGlobalRefresh() noexcept;

    [[nodiscard]] std::vector<RadianceUpdateCandidate>
    BuildUpdateList(
        const math::Double3& observerInFrameMeters,
        u32 maximumUpdates) const;

    [[nodiscard]] bool CommitUpdate(
        const RadianceCellKey& key,
        const DirectionalIrradianceL1& irradiance,
        u32 sampleCount,
        u64 sourceRevision);

    [[nodiscard]] const RadianceResidentCell* Lookup(
        const RadianceCellKey& key,
        u64 requiredRevision) const noexcept;

    [[nodiscard]] RadianceResidencyStats Stats() const noexcept;

    [[nodiscard]] RadianceGpuSnapshot BuildGpuSnapshot(
        const LightingView& view) const;

    [[nodiscard]] const RadianceClipmapConfig& Config() const noexcept;

private:
    struct Level
    {
        u32 level{0U};
        i64 centerX{0};
        i64 centerY{0};
        i64 centerZ{0};
        std::vector<RadianceResidentCell> slots;
    };

    [[nodiscard]] u32 PhysicalIndex(
        const RadianceCellKey& key) const noexcept;

    [[nodiscard]] RadianceResidentCell* SlotFor(
        const RadianceCellKey& key) noexcept;

    [[nodiscard]] const RadianceResidentCell* SlotFor(
        const RadianceCellKey& key) const noexcept;

    [[nodiscard]] bool BelongsToCurrentWindow(
        const RadianceCellKey& key) const noexcept;

    RadianceClipmapConfig config_{};
    frames::FrameId frame_{};
    universe::BodyId body_{};
    u64 sourceRevision_{0U};
    std::vector<Level> levels_;
};
} // namespace orbit::lighting
