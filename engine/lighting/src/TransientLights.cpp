#include <orbit/lighting/TransientLights.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::lighting
{
std::vector<LocalLight> TransientLightsFromFlash(
    const optical_transport::DischargeGraph& graph,
    const math::Double3& origin,
    const f64 t0,
    const TransientLightParams& params,
    const u64 stableIdBase)
{
    std::vector<LocalLight> lights;
    if (!(params.exposureSeconds > 0.0) || params.maxLights == 0U)
    {
        return lights;
    }
    auto clusters = graph.ClusterEmission(t0, t0 + params.exposureSeconds, params.maxLights);
    std::sort(clusters.begin(), clusters.end(), [](const auto& a, const auto& b) { return a.energy > b.energy; });

    for (std::size_t index = 0U; index < clusters.size(); ++index)
    {
        const auto& cluster = clusters[index];
        if (!(cluster.energy > 0.0))
        {
            continue;
        }
        const f64 opticalWatts = cluster.energy / params.exposureSeconds;
        const f64 lumens = opticalWatts * static_cast<f64>(params.luminousEfficacyOfRadiation);
        // Isotropic intensity I = flux / 4 pi; illuminance falls as I / d^2, so it reaches the floor at d = sqrt(I / E).
        const f64 intensity = lumens / (4.0 * std::numbers::pi);
        const f64 range = std::sqrt(intensity / static_cast<f64>(params.minimumIlluminanceLux));

        LocalLight light;
        light.type = LocalLightType::Point;
        light.positionInFrameMeters = {
            origin.x + cluster.position[0], origin.y + cluster.position[1], origin.z + cluster.position[2]};
        light.colorLinear = params.colorLinear;
        light.luminousFluxLumens = static_cast<f32>(lumens);
        light.rangeMeters = static_cast<f32>(std::clamp(
            range, static_cast<f64>(params.minimumRangeMeters), static_cast<f64>(params.maximumRangeMeters)));
        light.stableId = stableIdBase + index;
        lights.push_back(light);
    }
    return lights;
}
} // namespace orbit::lighting
