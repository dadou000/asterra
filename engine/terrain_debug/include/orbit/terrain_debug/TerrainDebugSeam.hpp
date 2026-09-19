#pragma once

#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugRaster.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace orbit::terrain_debug
{
class TerrainDebugLivePages;
class TerrainDebugPageData;

struct TerrainDebugSeamComparison
{
    TerrainDebugSeamProbe provenance{};

    u32 samplesCompared{0};
    u32 mismatchedSamples{0};

    // Meaningful for scalar/vector fields. Category/boolean/revision/LOD use
    // exact identity and leave this at zero.
    f64 maximumDifference{0.0};

    [[nodiscard]] bool Comparable() const noexcept;
    [[nodiscard]] bool ValuesContinuous() const noexcept;
};

// Compares matching physical edge samples using the cube-neighborhood's
// receiving-edge and reverse-sample mapping. The two views must represent the
// same debug field and their dimensions must agree with their page stamps.
[[nodiscard]] TerrainDebugSeamComparison CompareSeamValues(
    const TerrainDebugPageStamp& page,
    const TerrainDebugRasterView& pageView,
    world::TileEdge edge,
    const TerrainDebugPageStamp* neighbor,
    const TerrainDebugRasterView* neighborView,
    f64 numericTolerance = 1.0e-5);

enum class TerrainDebugSeamState : u8
{
    Continuous,
    MissingNeighbor,
    PhysicalLodMismatch,
    RevisionMismatch,
    FieldUnavailable,
    ValueMismatch
};

struct TerrainDebugSeamInspection
{
    world::TileEdge edge{world::TileEdge::North};
    TerrainDebugSeamState state{
        TerrainDebugSeamState::MissingNeighbor};
    TerrainDebugSeamComparison comparison{};
};

[[nodiscard]] std::string_view TerrainDebugSeamStateName(
    TerrainDebugSeamState state) noexcept;

// Resolves the four canonical physical neighbors from the live-page registry
// and evaluates the selected field using the same cube-face/sample remapping
// as CompareSeamValues(). No terrain generation is triggered by inspection.
[[nodiscard]] std::array<TerrainDebugSeamInspection, 4>
InspectTerrainDebugSeams(
    const TerrainDebugPageData& page,
    TerrainDebugField field,
    const TerrainDebugLivePages& livePages,
    f64 numericTolerance = 1.0e-5);

[[nodiscard]] u64 TerrainDebugSeamOverlayFingerprint(
    std::span<const TerrainDebugSeamInspection> seams) noexcept;

// Draws a presentation-only colored border directly into a tightly-packed
// RGBA8 physical-page debug image. Each N/E/S/W edge reflects its inspection
// state. This never mutates the underlying terrain/debug field samples.
void ApplyTerrainDebugSeamOverlayRgba8(
    std::vector<u8>& rgba,
    u32 width,
    u32 height,
    std::span<const TerrainDebugSeamInspection> seams,
    u32 thickness = 2U);

} // namespace orbit::terrain_debug
