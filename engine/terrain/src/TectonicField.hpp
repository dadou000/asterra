#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TectonicFieldDesc.hpp>
#include <orbit/terrain/TectonicStructure.hpp>

#include <array>
#include <functional>
#include <vector>

namespace orbit::terrain::detail
{
// Result of locating a sample direction relative to the two nearest
// tectonic plates. Everything here is a pure function of `direction` only
// (never `footprintMeters`), so it can be multiplied into elevation masks
// without introducing footprint-dependent discontinuities.
using ClaimArray = std::array<f64, kMaxTectonicPlates>;
// Unit tangent at the sample point from plate i towards plate j.
using BoundaryNormalFn = std::function<math::Double3(u32, u32)>;

struct TectonicSample
{
    u32 nearestPlate{0};
    u32 secondPlate{0};
    // 0..1, strongest where the two plates are actively colliding (weighted
    // down for ocean-ocean collisions), zero away from any boundary.
    f64 convergenceMask{0.0};
    // 0..1, strongest where the two plates are actively separating (mid-ocean
    // ridge / continental rift), zero away from any boundary. Unlike
    // convergenceMask, not weighted down for an ocean-ocean pairing --
    // spreading ridges are the most common real divergent boundary.
    f64 divergenceMask{0.0};
    // 0..1, strongest where the two plates slide laterally past each other
    // (transform/strike-slip fault) rather than converging or diverging,
    // zero away from any boundary.
    f64 transformMask{0.0};
    // Smoothly blended continental/oceanic elevation bias between the two
    // nearest plates -- shapes coastlines to cohere with plate identity.
    f64 plateBiasMeters{0.0};
    // convergenceMask split by the crust types of the colliding pair, each
    // unscaled and continuous across the field (a max over every plate pair
    // near the surface point). Use these, not the two nearest plates' flags,
    // to weight anything by collision type: the flags switch abruptly where
    // the runner-up plate changes.
    f64 convergenceContinental{0.0};
    f64 convergenceMixed{0.0};
    f64 convergenceOceanic{0.0};
    // Plate-type identity of the two nearest plates, for classifying a
    // boundary's geological subtype (e.g. orogeny needs both continental,
    // subduction needs at least one oceanic) without a second plate lookup.
    // Subduction with polarity (zero away from a subduction pair): the trench
    // sits on the descending plate's side of the boundary, the volcanic arc
    // inland on the overriding plate's side. Each pair decides which plate
    // descends (the more oceanic one, else the older), so both are continuous
    // within a pair and the max over pairs keeps them continuous across the
    // field.
    f64 subductionTrench{0.0};
    f64 subductionArc{0.0};
    // Baked structural elevation (m); zero from the plate model itself, which
    // only the baker's structure evaluation produces.
    f64 structuralElevationMeters{0.0};
    bool nearestIsContinental{false};
    bool secondIsContinental{false};
};

// Deterministic, allocation-free "plate tectonics" layer. Constructed once
// per GlobalTerrainFields instance; Sample()/HotspotElevationMeters() are
// pure functions of direction, safe to call concurrently.
class TectonicField
{
public:
    TectonicField(
        f64 planetRadiusMeters,
        const TectonicFieldDesc& desc);

    [[nodiscard]] TectonicSample Sample(
        const math::Double3& direction) const noexcept;

    // Same evaluation from caller-supplied per-plate claims (larger = closer
    // to that plate) instead of the seed-distance ones, and optionally a
    // boundary normal provider (unit tangent from plate i towards plate j).
    // The baker uses this with claims from noise-metric plate growth
    // (TectonicGrowth); without a provider the seed directions give the
    // normal.
    [[nodiscard]] TectonicSample SampleWithClaims(
        const math::Double3& direction,
        const ClaimArray& claims,
        const BoundaryNormalFn* normal) const noexcept;

    // Plate seeds and the claim-width constant, for plate growth.
    [[nodiscard]] u32 PlateCount() const noexcept { return plateCount_; }
    [[nodiscard]] math::Double3 PlateSeed(u32 plate) const noexcept
    {
        return plates_[plate].seedDirection;
    }
    [[nodiscard]] f64 PlateSizeBias(u32 plate) const noexcept
    {
        return plates_[plate].sizeBiasDot;
    }
    [[nodiscard]] f64 BoundaryWidth() const noexcept { return desc_.boundaryWidthDot; }

    // includeHotspot = false leaves hotspot chains out of uplift and volcanism
    // (the baker stores only the plate-driven part; hotspots are closed form
    // and added at sample time).
    [[nodiscard]] TectonicStructureSample SampleStructure(
        const math::Double3& direction,
        bool includeHotspot = true) const noexcept;

    [[nodiscard]] TectonicStructureSample SampleStructureWithClaims(
        const math::Double3& direction,
        bool includeHotspot,
        const ClaimArray& claims,
        const BoundaryNormalFn* normal) const noexcept;

    [[nodiscard]] f64 HotspotElevationMeters(
        const math::Double3& direction) const noexcept;

    [[nodiscard]] std::vector<GpuTectonicPlate> BuildGpuPlates() const;
    [[nodiscard]] std::vector<GpuTectonicHotspot> BuildGpuHotspots() const;

private:
    struct Plate
    {
        math::Double3 seedDirection{};
        bool isContinental{false};
        f64 continentalBiasMeters{0.0};
        // Rigid-body rotation vector (axis * angular speed). Surface
        // velocity anywhere on the sphere is Cross(eulerVector, position).
        math::Double3 eulerVector{};
        // Per-plate additive bias on the nearest-plate distance metric --
        // turns a plain nearest-seed Voronoi diagram (where every cell is
        // close to the same size for evenly spread seeds) into a
        // multiplicatively-weighted one, so some plates claim a much
        // larger share of the sphere than others without moving any seed
        // (which risks two seeds landing close enough to degenerate into
        // one abnormally huge cell -- see plateIrregularity).
        f64 sizeBiasDot{0.0};
        // Interior crust properties used only by SampleStructure.
        f64 crustThicknessKm{0.0};
        f64 crustAge{0.0};
        // Plate-wide mean continental fraction; the per-point fraction adds
        // intra-plate continent geometry on top (SampleStructure).
        f64 continentalBase{0.0};
    };

    struct Hotspot
    {
        math::Double3 mantlePosition{};
        std::array<math::Double3, kMaxTectonicHotspotAgeSteps> chainPoint{};
        std::array<f64, kMaxTectonicHotspotAgeSteps> chainAmplitude{};
        std::array<f64, kMaxTectonicHotspotAgeSteps> chainChordRadius{};
        f64 boundingCosine{-1.0};
    };

    TectonicFieldDesc desc_;
    f64 planetRadiusMeters_{1.0};
    std::array<Plate, kMaxTectonicPlates> plates_{};
    u32 plateCount_{0};
    std::array<Hotspot, kMaxTectonicHotspots> hotspots_{};
    u32 hotspotCount_{0};
    u32 hotspotAgeSteps_{0};
};
} // namespace orbit::terrain::detail
