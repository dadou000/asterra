#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>
#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
struct RegionalElevationDeltaField
{
    world::SurfaceFrame surfaceFrame{};
    u32 resolution{0};
    f64 halfExtentMeters{0.0};
    f64 spacingMeters{0.0};

    std::vector<f32> elevationDeltaMeters;

    [[nodiscard]] f32 At(
        u32 x,
        u32 y) const;

    [[nodiscard]] f64 SampleOffset(
        const math::Double2& offsetMeters) const noexcept;
};

[[nodiscard]] RegionalElevationDeltaField
BuildRegionalElevationDeltaField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    std::span<const f32> cumulativeElevationDeltaMeters);

struct RegionalElevationDeltaConfig
{
    f64 regionEdgeFadeMeters{20'000.0};

    // Multipliers relative to the regional grid sample spacing.
    f64 fullDetailFootprintScale{0.5};
    f64 fadeOutFootprintScale{4.0};
};

class RegionalElevationDeltaTerrainSource final :
    public terrain::TerrainSource
{
public:
    RegionalElevationDeltaTerrainSource(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        std::vector<RegionalElevationDeltaField> fields,
        RegionalElevationDeltaConfig config = {});

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;

    [[nodiscard]] const std::vector<
        RegionalElevationDeltaField>&
    Fields() const noexcept;

private:
    world::PlanetDefinition planet_{};

    std::shared_ptr<const terrain::TerrainSource>
        source_;

    std::vector<RegionalElevationDeltaField>
        fields_;

    RegionalElevationDeltaConfig config_{};
    u64 localRevision_{0};
};
} // namespace orbit::terrain_erosion
