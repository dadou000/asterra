#pragma once

#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/optical_transport/DischargeGraph.hpp>

#include <vector>

namespace orbit::lighting
{
// Turns the optical emission of a short-lived event (a lightning flash) into a bounded set of ordinary local lights
// for one exposure window, so it flows through the existing tiled-light path (including its overflow aggregation).
//
// A pulse far shorter than the exposure still deposits its whole energy: each light carries the cluster's energy in
// the window divided by the window length, so brightness integrates correctly however brief the stroke.
struct TransientLightParams
{
    f64 exposureSeconds{1.0 / 60.0};
    u32 maxLights{16U};                       // hard budget: cost never depends on how many segments the flash has
    // Luminous efficacy of the radiation, lumens per optical watt. The lights' flux is later divided by the scene's
    // photopic 683 lm/W like every other local light. Broadband lightning plasma is an assumed ~150, not measured.
    f32 luminousEfficacyOfRadiation{150.0F};
    // Light is treated as reaching zero where illuminance from the cluster falls to this many lux.
    f32 minimumIlluminanceLux{0.05F};
    f32 minimumRangeMeters{50.0F};
    f32 maximumRangeMeters{80000.0F};
    math::Float3 colorLinear{0.80F, 0.87F, 1.0F};   // blue-white
};

// `originInFrameMeters` places the graph's local coordinates in the lighting frame; `t0` is the exposure start in the
// graph's time base. Lights are ordered by decreasing flux. The sum of flux / efficacy * exposure over the result
// equals graph.EnergyInWindow(t0, t0 + exposure) (energy is preserved by the clustering, not by dropping lights).
[[nodiscard]] std::vector<LocalLight> TransientLightsFromFlash(
    const optical_transport::DischargeGraph& graph,
    const math::Double3& originInFrameMeters,
    f64 t0,
    const TransientLightParams& params,
    u64 stableIdBase);
} // namespace orbit::lighting
