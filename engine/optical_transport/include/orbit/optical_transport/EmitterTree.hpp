#pragma once

#include <cstdint>
#include <vector>

namespace orbit::optical_transport
{

struct Emitter
{
    double position[3] = {0, 0, 0};
    double power = 0.0;       // radiant power, W (spectral weighting is a per-channel multiple of this)
    double axis[3] = {0, -1, 0};  // emission axis for a cosine-lobe emitter
    double directionality = 0.0;  // 0 isotropic, 1 cosine lobe about axis (e.g. a downward streetlight)
};

// Hierarchy of emitters preserving integrated power, bounds and directionality so a merged cluster never turns
// into an isotropic point. Selection is bounded: a node is accepted whole only when a conservative error bound
// says the approximation is within tolerance; the energy it stands for is never discarded.
class EmitterTree
{
public:
    void Build(std::vector<Emitter> emitters, std::size_t leafSize = 4);

    struct Node
    {
        double center[3] = {0, 0, 0};   // power-weighted centroid
        double boundsMin[3] = {0, 0, 0};
        double boundsMax[3] = {0, 0, 0};
        double power = 0.0;
        double axis[3] = {0, -1, 0};    // mean emission axis (power weighted)
        double directionality = 0.0;    // coherence of member lobes (|mean axis| * weighted directionality)
        std::int32_t left = -1, right = -1;
        std::uint32_t first = 0, count = 0;  // leaf range into sorted emitters
    };

    // Irradiance (W/m^2) on a receiver with normal `normal` from one node treated as a single source, no occlusion.
    double NodeIrradiance(const Node& node, const double receiver[3], const double normal[3]) const;

    // Cut through the tree: nodes (or leaves expanded to emitters) such that the sum of per-node error bounds is
    // within relativeTolerance of the estimated total. Returns irradiance and how many sources were evaluated.
    struct Estimate
    {
        double irradiance = 0.0;
        double errorBound = 0.0;
        std::size_t sourcesEvaluated = 0;
        double powerAccounted = 0.0;  // always the full emitter power: nothing is dropped
    };
    Estimate Evaluate(const double receiver[3], const double normal[3], double relativeTolerance) const;

    // Exact sum over all individual emitters, for reference.
    double ExactIrradiance(const double receiver[3], const double normal[3]) const;

    const std::vector<Node>& Nodes() const { return nodes_; }
    std::size_t EmitterCount() const { return emitters_.size(); }
    double TotalPower() const { return nodes_.empty() ? 0.0 : nodes_[0].power; }

private:
    std::vector<Emitter> emitters_;
    std::vector<Node> nodes_;
    std::size_t leafSize_ = 4;

    std::int32_t BuildRange(std::size_t first, std::size_t count);
    double ErrorBound(const Node& node, const double receiver[3], const double normal[3]) const;
    double EmitterIrradiance(const Emitter& e, const double receiver[3], const double normal[3]) const;
};

} // namespace orbit::optical_transport
