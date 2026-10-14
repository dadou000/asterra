#include <orbit/path_geometry/CorridorDesignPreview.hpp>
#include <stdexcept>

namespace orbit::path_geometry
{
CorridorDesignPreview BuildCorridorDesignPreview(
    const PathCenterline& source,
    const paths::PathProfile& profile,
    CorridorDesignOptions options,
    const CorridorTerrainOffsetSampler& terrainAboveDatum)
{
    CorridorDesignPreview result;
    result.geometry = BuildPathDerived(source, profile, options.geometry);
    const auto rules = EngineeringRulesForProfile(profile, options.engineering);
    result.engineering = AnalyzeCorridorEngineering(result.geometry.stations, rules);
    if (!result.engineering.Valid())
        throw std::invalid_argument("Cannot preview engineering for invalid derived path stations.");
    result.suggestedBankRadians =
        SuggestCorridorBankProfile(result.engineering, rules);
    if (terrainAboveDatum)
        result.earthworks = EstimateCorridorEarthworks(
            result.geometry.stations, terrainAboveDatum, options.earthworks);
    return result;
}
} // namespace orbit::path_geometry
