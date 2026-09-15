#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TectonicFieldDesc.hpp>

#include <array>
#include <vector>

namespace orbit::terrain::detail
{
// Result of locating a sample direction relative to the two nearest
// tectonic plates. Everything here is a pure function of `direction` only
// (never `footprintMeters`), so it can be multiplied into elevation masks
// without introducing footprint-dependent discontinuities.
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
    // Plate-type identity of the two nearest plates, for classifying a
    // boundary's geological subtype (e.g. orogeny needs both continental,
    // subduction needs at least one oceanic) without a second plate lookup.
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
