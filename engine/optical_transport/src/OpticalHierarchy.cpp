#include "orbit/optical_transport/OpticalHierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::optical_transport
{

void OpticalHierarchy::Build(const std::vector<float>& extinction, std::uint32_t edge, double cellSize)
{
    edge_ = edge;
    cellSize_ = cellSize;
    levels_.clear();
    Level leaf;
    leaf.edge = edge;
    leaf.nodes.resize(static_cast<std::size_t>(edge) * edge * edge);
    for (std::size_t i = 0; i < leaf.nodes.size(); ++i)
    {
        const float v = i < extinction.size() ? extinction[i] : 0.0f;
        leaf.nodes[i] = {v, v, v};
    }
    levels_.push_back(std::move(leaf));
    while (levels_.back().edge > 1)
    {
        const Level& child = levels_.back();
        Level parent;
        parent.edge = child.edge / 2;
        parent.nodes.resize(static_cast<std::size_t>(parent.edge) * parent.edge * parent.edge);
        levels_.push_back(std::move(parent));
        const std::size_t p = levels_.size() - 1;
        for (std::uint32_t z = 0; z < levels_[p].edge; ++z)
            for (std::uint32_t y = 0; y < levels_[p].edge; ++y)
                for (std::uint32_t x = 0; x < levels_[p].edge; ++x)
                    Recompute(p, x, y, z);
    }
    ++version_;
}

void OpticalHierarchy::Recompute(std::size_t level, std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    const Level& child = levels_[level - 1];
    Bounds b{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), 0.0f};
    double sum = 0.0;
    for (std::uint32_t dz = 0; dz < 2; ++dz)
        for (std::uint32_t dy = 0; dy < 2; ++dy)
            for (std::uint32_t dx = 0; dx < 2; ++dx)
            {
                const std::size_t idx = (static_cast<std::size_t>(2 * z + dz) * child.edge + (2 * y + dy)) * child.edge + (2 * x + dx);
                const Bounds& c = child.nodes[idx];
                b.minimum = std::min(b.minimum, c.minimum);
                b.maximum = std::max(b.maximum, c.maximum);
                sum += c.mean;
            }
    b.mean = static_cast<float>(sum / 8.0);
    Level& self = levels_[level];
    self.nodes[(static_cast<std::size_t>(z) * self.edge + y) * self.edge + x] = b;
}

std::size_t OpticalHierarchy::SetVoxel(std::uint32_t x, std::uint32_t y, std::uint32_t z, float value)
{
    levels_[0].nodes[(static_cast<std::size_t>(z) * edge_ + y) * edge_ + x] = {value, value, value};
    std::size_t touched = 1;
    for (std::size_t level = 1; level < levels_.size(); ++level)
    {
        x /= 2; y /= 2; z /= 2;
        Recompute(level, x, y, z);
        ++touched;
    }
    ++version_;
    return touched;
}

OpticalHierarchy::Bounds OpticalHierarchy::Root() const
{
    return levels_.empty() ? Bounds{} : levels_.back().nodes[0];
}

void OpticalHierarchy::Traverse(std::size_t level, std::uint32_t x, std::uint32_t y, std::uint32_t z, const double o[3],
                                const double d[3], double t0, double t1, std::size_t stopLevel, DepthResult& out) const
{
    ++out.nodesVisited;
    const double size = cellSize_ * static_cast<double>(1u << level);
    const double lo[3] = {x * size, y * size, z * size};
    // Slab test of the node box against the segment [t0, t1].
    double a = t0, b = t1;
    for (int k = 0; k < 3; ++k)
    {
        if (std::abs(d[k]) < 1.0e-12)
        {
            if (o[k] < lo[k] || o[k] > lo[k] + size) return;
            continue;
        }
        double s0 = (lo[k] - o[k]) / d[k];
        double s1 = (lo[k] + size - o[k]) / d[k];
        if (s0 > s1) std::swap(s0, s1);
        a = std::max(a, s0);
        b = std::min(b, s1);
        if (a >= b) return;
    }
    const Bounds& node = levels_[level].nodes[(static_cast<std::size_t>(z) * levels_[level].edge + y) * levels_[level].edge + x];
    if (node.maximum == 0.0f)
    {
        return;  // provably empty: skipped without refinement
    }
    const double len = b - a;
    if (level <= stopLevel || node.maximum == node.minimum)
    {
        // Coarse stop (conservative interval) or homogeneous node (exact, analytic).
        out.lower += node.minimum * len;
        out.upper += node.maximum * len;
        out.estimate += node.mean * len;
        return;
    }
    for (std::uint32_t dz = 0; dz < 2; ++dz)
        for (std::uint32_t dy = 0; dy < 2; ++dy)
            for (std::uint32_t dx = 0; dx < 2; ++dx)
                Traverse(level - 1, 2 * x + dx, 2 * y + dy, 2 * z + dz, o, d, a, b, stopLevel, out);
}

OpticalHierarchy::DepthResult OpticalHierarchy::OpticalDepth(const double origin[3], const double direction[3], double length,
                                                              std::size_t stopLevel) const
{
    DepthResult out;
    if (levels_.empty()) return out;
    Traverse(levels_.size() - 1, 0, 0, 0, origin, direction, 0.0, length, stopLevel, out);
    return out;
}

} // namespace orbit::optical_transport
