#pragma once

#include <orbit/lighting/EmissiveSampling.hpp>

#include <span>
#include <vector>

namespace orbit::lighting
{
struct PlanetaryEmissionCell
{
    // Energy-preserving aggregate used for GI/far appearance.
    math::Float3 integratedRadianceArea{};
    f64 radiantImportance{0.0};
    u32 contributingEmitters{0U};
};

struct PlanetaryEmissionLevel
{
    u32 width{0U};
    u32 height{0U};
    std::vector<PlanetaryEmissionCell> cells;
};

struct PlanetaryEmissionField
{
    frames::FrameId frame{};
    universe::BodyId body{};
    math::Double3 bodyCenterInFrameMeters{};
    u64 sourceRevision{0U};

    std::vector<PlanetaryEmissionLevel> levels;
};

struct PlanetaryEmissionFieldConfig
{
    u32 baseWidth{256U};
    u32 baseHeight{128U};
    u32 maximumLevels{9U};
};

[[nodiscard]] PlanetaryEmissionField
BuildPlanetaryEmissionField(
    frames::FrameId frame,
    universe::BodyId body,
    const math::Double3& bodyCenterInFrameMeters,
    u64 sourceRevision,
    std::span<const EmissiveSampledEmitter> emitters,
    const PlanetaryEmissionFieldConfig& config = {});

[[nodiscard]] const PlanetaryEmissionCell*
SamplePlanetaryEmission(
    const PlanetaryEmissionField& field,
    u32 level,
    const math::Double3& directionFromBodyCenter) noexcept;

[[nodiscard]] math::Float3
TotalPlanetaryIntegratedRadianceArea(
    const PlanetaryEmissionLevel& level) noexcept;
} // namespace orbit::lighting
