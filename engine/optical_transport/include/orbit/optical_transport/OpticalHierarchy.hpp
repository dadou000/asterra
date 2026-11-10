#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orbit::optical_transport
{

// Sparse multiresolution summary of an extinction field on a regular 3D grid (power-of-two edge). Every node keeps
// conservative min/max extinction and the mean so empty space is skipped, homogeneous regions are integrated
// analytically and only boundaries refine. Independent of the camera.
class OpticalHierarchy
{
public:
    // `extinction` has edge^3 values (x fastest). `edge` must be a power of two. cellSize in metres.
    void Build(const std::vector<float>& extinction, std::uint32_t edge, double cellSize);

    // Update one voxel and refresh ancestors; returns the number of nodes touched (dirty-range size).
    std::size_t SetVoxel(std::uint32_t x, std::uint32_t y, std::uint32_t z, float value);

    struct Bounds
    {
        float minimum = 0.0f;
        float maximum = 0.0f;
        float mean = 0.0f;
    };
    Bounds Root() const;

    // Conservative optical-depth interval along a segment [origin, origin + direction * length] (direction unit, metres).
    // lower <= true tau <= upper. `nodesVisited` reports traversal cost; `estimate` is the mean-extinction integral at
    // the resolution reached (skipping provably empty nodes, integrating homogeneous nodes analytically).
    struct DepthResult
    {
        double lower = 0.0;
        double upper = 0.0;
        double estimate = 0.0;
        std::size_t nodesVisited = 0;
    };
    // stopLevel > 0 stops refining at that level and returns the conservative min/max interval there.
    DepthResult OpticalDepth(const double origin[3], const double direction[3], double length,
                            std::size_t stopLevel = 0) const;

    std::uint32_t Edge() const { return edge_; }
    std::uint64_t Version() const { return version_; }

private:
    struct Level
    {
        std::uint32_t edge = 0;
        std::vector<Bounds> nodes;
    };
    std::vector<Level> levels_;  // levels_[0] is the leaf level
    std::uint32_t edge_ = 0;
    double cellSize_ = 1.0;
    std::uint64_t version_ = 0;

    void Recompute(std::size_t level, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    void Traverse(std::size_t level, std::uint32_t x, std::uint32_t y, std::uint32_t z, const double origin[3],
                  const double direction[3], double t0, double t1, std::size_t stopLevel, DepthResult& out) const;
};

} // namespace orbit::optical_transport
