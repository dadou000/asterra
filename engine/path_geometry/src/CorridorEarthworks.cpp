#include <orbit/path_geometry/CorridorEarthworks.hpp>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace orbit::path_geometry
{
CorridorEarthworkEstimate EstimateCorridorEarthworks(
    const std::vector<PathStation>& stations,
    const CorridorTerrainOffsetSampler& terrainAboveDatum,
    const CorridorEarthworkOptions options)
{
    if (!terrainAboveDatum || !std::isfinite(options.halfWidthMeters) ||
        options.halfWidthMeters <= 0.0 ||
        !std::isfinite(options.shoulderMeters) || options.shoulderMeters < 0.0 ||
        options.crossSectionSamples < 2U || options.crossSectionSamples > 512U ||
        (options.crossSectionSamples % 2U) != 0U ||
        !std::isfinite(options.maximumStationSpacingMeters) ||
        options.maximumStationSpacingMeters <= 0.0)
        throw std::invalid_argument("Invalid corridor earthwork sampler or options.");

    CorridorEarthworkEstimate result;
    if (stations.empty()) return result;
    const f64 halfWidth = options.halfWidthMeters + options.shoulderMeters;
    const f64 dx = 2.0 * halfWidth /
        static_cast<f64>(options.crossSectionSamples);
    const auto section = [&](const PathStation& s) {
        if (!std::isfinite(s.stationMeters) ||
            !std::isfinite(s.position.x) || !std::isfinite(s.position.y) ||
            !std::isfinite(s.position.z) ||
            !std::isfinite(s.lateral.x) || !std::isfinite(s.lateral.y) ||
            !std::isfinite(s.lateral.z) ||
            std::abs(math::LengthSquared(s.lateral) - 1.0) > 0.1)
            throw std::invalid_argument("Invalid earthwork station frame.");
        CorridorEarthworkStation resultSection{.stationMeters=s.stationMeters};
        for (u32 j=0; j<options.crossSectionSamples; ++j)
        {
            const f64 lateral = -halfWidth + (static_cast<f64>(j)+0.5)*dx;
            const f64 offset = terrainAboveDatum(s.position + s.lateral*lateral);
            if (!std::isfinite(offset))
                throw std::invalid_argument("Earthwork source returned a nonfinite offset.");
            resultSection.cutAreaSquareMeters += std::max(offset,0.0)*dx;
            resultSection.fillAreaSquareMeters += std::max(-offset,0.0)*dx;
        }
        return resultSection;
    };
    result.sections.push_back(section(stations.front()));
    for (std::size_t i=1; i<stations.size(); ++i)
    {
        const auto& a=stations[i-1];
        const auto& b=stations[i];
        const f64 ds=b.stationMeters-a.stationMeters;
        if (!std::isfinite(ds) || ds<=0.0)
            throw std::invalid_argument("Earthwork stations must increase strictly.");
        // Re-sample long segments, so previews cannot silently underestimate
        // the volume when sparse source stations cross varied terrain.
        const auto subdivisions=static_cast<u32>(std::ceil(ds/options.maximumStationSpacingMeters));
        if (subdivisions==0U || subdivisions>100000U)
            throw std::invalid_argument("Earthwork subdivision budget exceeded.");
        for (u32 k=1;k<=subdivisions;++k)
        {
            const f64 t=static_cast<f64>(k)/static_cast<f64>(subdivisions);
            PathStation s;
            s.stationMeters=a.stationMeters+ds*t;
            s.position=a.position*(1.0-t)+b.position*t;
            s.lateral=math::Normalize(a.lateral*(1.0-t)+b.lateral*t);
            const auto current=section(s);
            const auto& previous=result.sections.back();
            const f64 step=current.stationMeters-previous.stationMeters;
            result.cutCubicMeters +=
                (previous.cutAreaSquareMeters+current.cutAreaSquareMeters)*0.5*step;
            result.fillCubicMeters +=
                (previous.fillAreaSquareMeters+current.fillAreaSquareMeters)*0.5*step;
            result.evaluatedLengthMeters += step;
            result.sections.push_back(current);
        }
    }
    return result;
}
} // namespace orbit::path_geometry
