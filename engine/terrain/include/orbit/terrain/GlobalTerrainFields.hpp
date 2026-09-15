#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TectonicFieldDesc.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
#include <memory>
#include <vector>

namespace orbit::terrain
{
namespace detail
{
// Defined in the private src/TectonicField.hpp -- only forward-declared
// here so this public header stays free of the implementation. See
// GlobalTerrainFields.cpp for the definition of GlobalTerrainFields'
// pimpl-held instance.
class TectonicField;
} // namespace detail

// Exported copies of TectonicField's private per-plate/per-hotspot state,
// for a GPU field generator to upload verbatim -- the GPU and CPU
// generators must render the *same* plates/hotspots (a mountain range
// shown on the 2D map, itself CPU-generated, has to be the same range the
// 3D clipmap terrain renders under it), so this is deliberately exported
// data, not independently regenerated GPU-side from a different hash.
struct GpuTectonicPlate
{
    math::Double3 seedDirection{};
    bool isContinental{false};
    f64 continentalBiasMeters{0.0};
    math::Double3 eulerVector{};
    f64 sizeBiasDot{0.0};
};

struct GpuTectonicHotspot
{
    math::Double3 mantlePosition{};
    f64 boundingCosine{-1.0};
    u32 ageSteps{0};
    std::array<math::Double3, kMaxTectonicHotspotAgeSteps> chainPoint{};
    std::array<f64, kMaxTectonicHotspotAgeSteps> chainAmplitude{};
    std::array<f64, kMaxTectonicHotspotAgeSteps> chainChordRadius{};
};

struct GlobalTerrainFieldDesc
{
    // Zero means derive from the owning terrain source seed.
    u64 seed{0};

    f64 seaLevelMeters{0.0};

    f64 continentalAmplitudeMeters{3'600.0};
    f64 continentalWavelengthMeters{4'800'000.0};
    f64 continentalBiasMeters{-650.0};

    f64 mountainAmplitudeMeters{2'800.0};
    f64 mountainWavelengthMeters{1'500'000.0};

    f64 climateWavelengthMeters{3'200'000.0};
    f64 equatorTemperatureC{31.0};
    f64 poleTemperatureC{-24.0};
    f64 temperatureVariationC{4.0};
    f64 lapseRateCPerKilometer{6.2};

    TectonicFieldDesc tectonic{};
};

struct GlobalTerrainFieldSample
{
    f64 coarseElevationMeters{0.0};
    f64 landMask{0.0};
    TerrainClimate climate{};
    BiomeWeights biomes{};
    // Direction-only (never footprintMeters-dependent) plate-boundary
    // convergence strength, 0..1 -- replaces the old noise-based mountain
    // range mask so ranges cohere into plate-boundary-shaped chains.
    f64 convergenceMask{0.0};
    // 0..1, strongest where the two nearest plates are actively separating
    // (mid-ocean ridge / continental rift). See TectonicSample.
    f64 divergenceMask{0.0};
    // 0..1, strongest where the two nearest plates slide laterally past
    // each other (transform/strike-slip fault). See TectonicSample.
    f64 transformMask{0.0};
    // Plate-type identity of the two nearest plates -- lets a caller tell
    // a continental collision (orogeny) apart from a subduction zone, or
    // an oceanic spreading ridge apart from a continental rift, from the
    // convergence/divergence masks above.
    bool nearestPlateContinental{false};
    bool secondPlateContinental{false};
    // Additional relief from any volcanic hotspot chains at this point.
    f64 hotspotElevationMeters{0.0};
};

class GlobalTerrainFields
{
public:
    GlobalTerrainFields(
        world::PlanetDefinition planet,
        GlobalTerrainFieldDesc desc = {});
    ~GlobalTerrainFields();

    GlobalTerrainFields(GlobalTerrainFields&&) noexcept;
    GlobalTerrainFields& operator=(GlobalTerrainFields&&) noexcept;
    GlobalTerrainFields(const GlobalTerrainFields&) = delete;
    GlobalTerrainFields& operator=(const GlobalTerrainFields&) = delete;

    [[nodiscard]] GlobalTerrainFieldSample Sample(
        const TerrainQuery& query) const noexcept;

    [[nodiscard]] const GlobalTerrainFieldDesc&
    Description() const noexcept;

    // Exports the exact plates/hotspots this instance generated, for a
    // GPU field generator to upload -- see GpuTectonicPlate's comment for
    // why this must be exported rather than regenerated GPU-side.
    [[nodiscard]] std::vector<GpuTectonicPlate>
    TectonicPlatesForGpu() const;

    [[nodiscard]] std::vector<GpuTectonicHotspot>
    TectonicHotspotsForGpu() const;

    // Cheap coarse elevation estimate at an arbitrary direction: plate
    // continental bias + continental noise signal + a small analytic
    // convergence/hotspot bump, with no ridge noise, no detail octaves,
    // and no recursion into Sample()/SampleNormalized(). Used only by
    // AnalyticTerrainSource's rain-shadow upwind probe, where a handful
    // of extra full detailed samples per query would be too costly.
    [[nodiscard]] f64 PlateElevationEstimateMeters(
        const math::Double3& direction) const noexcept;

private:
    // The analytic source already normalizes its direction and classifies
    // biomes after adding relief. Avoid doing that work twice per sample.
    friend class AnalyticTerrainSource;
    [[nodiscard]] GlobalTerrainFieldSample SampleNormalized(
        const TerrainQuery& query,
        bool includeBiomes) const noexcept;

    [[nodiscard]] f64 ContinentalSignal(
        const math::Double3& direction) const noexcept;

    world::PlanetDefinition planet_;
    GlobalTerrainFieldDesc desc_;
    std::unique_ptr<detail::TectonicField> tectonicField_;
};
} // namespace orbit::terrain
