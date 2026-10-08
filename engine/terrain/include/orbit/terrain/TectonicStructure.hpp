#pragma once

#include <orbit/core/Types.hpp>

#include <string_view>

namespace orbit::terrain
{
enum class TectonicBoundaryType : u8
{
    None,
    Convergent,
    Divergent,
    Transform
};

[[nodiscard]] constexpr std::string_view TectonicBoundaryTypeName(
    const TectonicBoundaryType type) noexcept
{
    switch (type)
    {
    case TectonicBoundaryType::Convergent: return "convergent";
    case TectonicBoundaryType::Divergent: return "divergent";
    case TectonicBoundaryType::Transform: return "transform";
    case TectonicBoundaryType::None: break;
    }
    return "none";
}

// Planet structural layer: stable, direction-only fields that terrain,
// hydrology, biomes and hazards can query without owning plate logic. Derived
// analytically from the persisted TectonicFieldDesc -- nothing here is stored
// or authored separately, and it never feeds back into plate placement, so
// existing terrain output is unchanged.
struct TectonicStructureSample
{
    u32 plateId{0};
    u32 neighbourPlateId{0};
    bool continental{false};
    bool neighbourContinental{false};

    TectonicBoundaryType boundaryType{TectonicBoundaryType::None};
    // 0..1 strength of the dominant boundary influence at this point.
    f64 boundaryStrength{0.0};
    f64 convergence{0.0};
    f64 divergence{0.0};
    f64 transform{0.0};

    // Rigid-plate surface speed relative to the mantle frame, in
    // radians/second-equivalent units scaled by planet radius (m per unit
    // angular speed).
    f64 plateSpeedMetersPerUnit{0.0};

    // 0..1, 0 = oceanic and 1 = continental crust; continuous and independent
    // of plate ids (a plate can carry both). Thickness is derived from it.
    f64 continentalCrustFraction{0.0};
    f64 crustThicknessKm{0.0};
    // 0 = newly formed crust, 1 = ancient craton.
    f64 crustAge{0.0};
    // Crustal-age proxy used to select landscape morphology: mountains of low
    // age are sharp and high-relief, old ones rounded and deeply eroded.
    f64 geologicalAge{0.0};

    f64 upliftMeters{0.0};
    f64 subsidenceMeters{0.0};
    // 0..1, tectonic stress (convergent loading or shear).
    f64 stress{0.0};
    // 0..1, subduction arcs, rift volcanism and hotspot chains.
    f64 volcanism{0.0};
    // 0..1 subduction polarity fields: the trench lies on the descending
    // plate's side of the boundary, the volcanic arc inland on the
    // overriding plate's side. Zero away from subduction zones.
    f64 subductionTrench{0.0};
    f64 volcanicArc{0.0};
};
} // namespace orbit::terrain
