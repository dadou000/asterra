#include <orbit/terrain_material_column/SurfaceResolver.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain_material_column
{
namespace
{
[[nodiscard]] f32 ExposedLayerDepth(
    const MaterialColumnCell& column,
    const ExposedSurfaceKind material) noexcept
{
    switch (material)
    {
    case ExposedSurfaceKind::Bedrock:
        return 0.0F;
    case ExposedSurfaceKind::Regolith:
        return column.regolithMeters;
    case ExposedSurfaceKind::Soil:
        return column.soilMeters;
    case ExposedSurfaceKind::Sand:
        return column.sandMeters;
    case ExposedSurfaceKind::Debris:
        return column.debrisMeters;
    }

    return 0.0F;
}
} // namespace

bool GeologySample::IsValid() const noexcept
{
    const auto unit =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F &&
                value <= 1.0F;
        };

    return
        bedrockMaterial.IsValid() &&
        unit(hardness) &&
        unit(cohesion) &&
        unit(hydraulicErodibility) &&
        unit(aeolianErodibility) &&
        unit(permeability) &&
        unit(chemicalWeatherability) &&
        unit(fractureTendency) &&
        std::isfinite(
            densityKgPerCubicMeter) &&
        densityKgPerCubicMeter > 0.0F;
}

bool ProcessDerivedFields::IsValid() const noexcept
{
    return
        std::isfinite(
            standingWaterDepthMeters) &&
        standingWaterDepthMeters >= 0.0F &&
        std::isfinite(
            snowDepthMeters) &&
        snowDepthMeters >= 0.0F;
}

bool ExposedSurfaceState::BedrockExposed() const noexcept
{
    return
        material ==
            ExposedSurfaceKind::Bedrock;
}

bool ExposedSurfaceState::IsValid() const noexcept
{
    if (!substrateRock.IsValid() ||
        !geology.IsValid() ||
        geology.bedrockMaterial !=
            substrateRock ||
        !std::isfinite(
            exposedLayerDepthMeters) ||
        exposedLayerDepthMeters <
            0.0F ||
        !std::isfinite(
            surfaceHeightMeters) ||
        !std::isfinite(
            moisture) ||
        moisture < 0.0F ||
        moisture > 1.0F ||
        !std::isfinite(
            standingWaterDepthMeters) ||
        standingWaterDepthMeters <
            0.0F ||
        !std::isfinite(
            snowDepthMeters) ||
        snowDepthMeters <
            0.0F)
    {
        return false;
    }

    if (BedrockExposed())
    {
        return
            exposedRock.IsValid() &&
            exposedRock ==
                substrateRock &&
            exposedLayerDepthMeters ==
                0.0F;
    }

    return
        !exposedRock.IsValid() &&
        exposedLayerDepthMeters >
            0.0F;
}

GeologySample SampleColumnGeology(
    const MaterialColumnCell& column,
    const terrain_geology::GeologicalMaterialLibrary& geology)
{
    if (!column.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M18 cannot sample geology for an invalid M08 column.");
    }

    const auto* rock =
        geology.Find(
            column.bedrockMaterial);

    if (rock == nullptr)
    {
        throw std::invalid_argument(
            "Orbit M18 M08 column references an unknown M02 rock.");
    }

    return {
        .bedrockMaterial =
            rock->id,
        .hardness =
            rock->hardness,
        .cohesion =
            rock->cohesion,
        .hydraulicErodibility =
            rock->hydraulicErodibility,
        .aeolianErodibility =
            rock->aeolianErodibility,
        .permeability =
            rock->permeability,
        .chemicalWeatherability =
            rock->chemicalWeatherability,
        .fractureTendency =
            rock->fractureTendency,
        .densityKgPerCubicMeter =
            rock->density
    };
}

ExposedSurfaceState ResolveSurface(
    const MaterialColumnCell& column,
    const GeologySample& geology,
    const ProcessDerivedFields& fields)
{
    if (!column.IsValid() ||
        !geology.IsValid() ||
        !fields.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M18 exposed-surface resolver received invalid physical state.");
    }

    if (geology.bedrockMaterial !=
        column.bedrockMaterial)
    {
        throw std::invalid_argument(
            "Orbit M18 geology sample does not match the M08 substrate identity.");
    }

    const ExposedSurfaceKind exposed =
        column.ExposedSurface();

    ExposedSurfaceState result{
        .material =
            exposed,
        .substrateRock =
            column.bedrockMaterial,
        .exposedRock =
            exposed ==
                    ExposedSurfaceKind::
                        Bedrock
                ? column.
                    bedrockMaterial
                : terrain_geology::
                    RockTypeId{},
        .exposedLayerDepthMeters =
            ExposedLayerDepth(
                column,
                exposed),
        .surfaceHeightMeters =
            column.
                SurfaceHeightMeters(),
        .moisture =
            column.moisture,
        .standingWaterDepthMeters =
            fields.
                standingWaterDepthMeters,
        .snowDepthMeters =
            fields.
                snowDepthMeters,
        .geology =
            geology
    };

    if (!result.IsValid())
    {
        throw std::logic_error(
            "Orbit M18 produced an invalid exposed-surface state.");
    }

    return result;
}
} // namespace orbit::terrain_material_column
