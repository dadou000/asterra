#pragma once

#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/optical_transport/EmitterTree.hpp>

#include <span>
#include <vector>

namespace orbit::lighting
{
// Hierarchy over a very large set of local lights (a city's street lamps, windows, signs) so the light list for a
// RECEIVER is a bounded cut through it instead of every light. Far from the receiver, whole districts collapse into one
// aggregated light that keeps their total flux and flux-weighted centroid; close to it the cut descends to the original
// lights, which are returned untouched.
//
// The cut is only as good as the receiver it was made for: lighting error depends on the distance from the surface being
// lit to the lamps, not from the camera. Use it per receiver region (a patch of cloud base, a tile of ground, a distant
// town seen from orbit). A cut made for one place and used at a nearby surface merges that surface's own lamps away. Total flux is preserved by construction (every light lies in exactly one
// selected node), so a thousand individually negligible lamps still add up.
//
// The merged light of a directional group (street lamps all pointing down) is a spot along the group's mean axis, not
// an isotropic point, so a district seen from above is not lit as if it radiated upward.
class CityLightTree
{
public:
    // Lights in lighting-frame metres. Builds the hierarchy (O(n log n)); rebuild when lights change.
    void Build(std::span<const LocalLight> lights, std::size_t leafSize = 8);

    struct CutResult
    {
        std::vector<LocalLight> lights;
        std::size_t mergedNodes = 0;      // nodes that stand for more than one original light
        double totalFlux = 0.0;           // lumens in the cut (== TotalFlux() always)
    };
    // Cut for a receiver: nodes subtending more than `maxAngleRadians` at it are refined until `maxLights` is used;
    // leaves still above the threshold are expanded to their original lights while the budget allows.
    [[nodiscard]] CutResult Cut(const math::Double3& receiverInFrameMeters, double maxAngleRadians, std::size_t maxLights) const;

    [[nodiscard]] std::size_t LightCount() const { return source_.size(); }
    [[nodiscard]] double TotalFlux() const { return totalFlux_; }

private:
    std::vector<LocalLight> source_;
    optical_transport::EmitterTree tree_;
    // Per node, parallel to tree_.Nodes(): flux-weighted colour and the radius within which any member reaches.
    std::vector<math::Float3> nodeColour_;
    std::vector<float> nodeReach_;
    double totalFlux_ = 0.0;
};
} // namespace orbit::lighting
