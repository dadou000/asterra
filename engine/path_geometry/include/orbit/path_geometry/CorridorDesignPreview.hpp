#pragma once
#include <orbit/path_geometry/CorridorEngineering.hpp>
#include <orbit/path_geometry/CorridorEarthworks.hpp>

namespace orbit::path_geometry
{
// Read-only authoring preview. The authored network remains the sole authority;
// the mesh, engineering warnings and quantities are disposable derived data.
struct CorridorDesignPreview
{
    PathDerivedProduct geometry;
    CorridorEngineeringReport engineering;
    std::vector<f64> suggestedBankRadians;
    std::optional<CorridorEarthworkEstimate> earthworks;
};
struct CorridorDesignOptions
{
    PathBuildOptions geometry{};
    CorridorEngineeringRules engineering{};
    CorridorEarthworkOptions earthworks{};
};
[[nodiscard]] CorridorDesignPreview BuildCorridorDesignPreview(
    const PathCenterline& source,
    const paths::PathProfile& profile,
    CorridorDesignOptions options = {},
    const CorridorTerrainOffsetSampler& terrainAboveDatum = {});
} // namespace orbit::path_geometry
