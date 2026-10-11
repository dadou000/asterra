#include "orbit/optical_transport/DischargeGraph.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace orbit::optical_transport
{

std::int32_t DischargeGraph::AddSegment(Segment segment)
{
    segments_.push_back(std::move(segment));
    return static_cast<std::int32_t>(segments_.size() - 1);
}

bool DischargeGraph::Transfer(Reservoir& from, Reservoir& to, double charge)
{
    // Charge moves from a positive source toward a recipient; the source must hold at least `charge` in magnitude.
    if (charge < 0.0 || std::abs(from.charge) < charge)
    {
        return false;
    }
    const double moved = from.charge >= 0.0 ? charge : -charge;
    from.charge -= moved;
    to.charge += moved;
    return true;
}

double DischargeGraph::EnergyInWindow(double t0, double t1) const
{
    double energy = 0.0;
    for (const Segment& s : segments_)
    {
        for (const Pulse& p : s.pulses)
        {
            const double a = std::max(t0, p.startTime);
            const double b = std::min(t1, p.startTime + p.duration);
            if (b <= a) continue;
            energy += p.duration > 0.0 ? p.radiantEnergy * (b - a) / p.duration : p.radiantEnergy;
        }
        // Zero-duration impulse pulses count when their instant is inside the window.
    }
    for (const Segment& s : segments_)
    {
        for (const Pulse& p : s.pulses)
        {
            if (p.duration <= 0.0 && p.startTime >= t0 && p.startTime < t1) energy += p.radiantEnergy;
        }
    }
    return energy;
}

std::vector<DischargeGraph::Cluster> DischargeGraph::ClusterEmission(double t0, double t1, std::size_t maxClusters) const
{
    std::vector<Cluster> out;
    if (segments_.empty() || maxClusters == 0) return out;
    // Grow a cell size until the occupied cell count fits the budget.
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (const Segment& s : segments_)
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min({lo[k], s.from[k], s.to[k]});
            hi[k] = std::max({hi[k], s.from[k], s.to[k]});
        }
    double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1.0e-6});
    for (double cell = extent / 64.0;; cell *= 1.5)
    {
        std::map<std::tuple<long, long, long>, Cluster> cells;
        for (const Segment& s : segments_)
        {
            double e = 0.0;
            for (const Pulse& p : s.pulses)
            {
                const double a = std::max(t0, p.startTime), b = std::min(t1, p.startTime + p.duration);
                if (p.duration > 0.0 && b > a) e += p.radiantEnergy * (b - a) / p.duration;
                else if (p.duration <= 0.0 && p.startTime >= t0 && p.startTime < t1) e += p.radiantEnergy;
            }
            if (e <= 0.0) continue;
            double mid[3];
            for (int k = 0; k < 3; ++k) mid[k] = 0.5 * (s.from[k] + s.to[k]);
            auto key = std::make_tuple(static_cast<long>(std::floor((mid[0] - lo[0]) / cell)), static_cast<long>(std::floor((mid[1] - lo[1]) / cell)),
                                       static_cast<long>(std::floor((mid[2] - lo[2]) / cell)));
            Cluster& c = cells[key];
            for (int k = 0; k < 3; ++k) c.position[k] += e * mid[k];
            c.energy += e;
        }
        if (cells.size() <= maxClusters || cell > extent * 2.0)
        {
            for (auto& [key, c] : cells)
            {
                if (c.energy > 0.0) for (int k = 0; k < 3; ++k) c.position[k] /= c.energy;
                out.push_back(c);
            }
            return out;
        }
    }
}

void DischargeGraph::Reactivate(std::int32_t index, Pulse pulse)
{
    segments_[static_cast<std::size_t>(index)].pulses.push_back(pulse);
}

void SolvePoisson(PoissonGrid& g, int iterations)
{
    const std::size_t n = static_cast<std::size_t>(g.nx) * g.ny * g.nz;
    g.potential.assign(n, 0.0);
    std::vector<double> next(n, 0.0);
    auto idx = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (static_cast<std::size_t>(z) * g.ny + y) * g.nx + x; };
    const double h2 = g.spacing * g.spacing;
    for (int it = 0; it < iterations; ++it)
    {
        for (std::uint32_t z = 1; z + 1 < g.nz; ++z)
            for (std::uint32_t y = 1; y + 1 < g.ny; ++y)
                for (std::uint32_t x = 1; x + 1 < g.nx; ++x)
                {
                    const double neighbours = g.potential[idx(x - 1, y, z)] + g.potential[idx(x + 1, y, z)] + g.potential[idx(x, y - 1, z)] +
                                              g.potential[idx(x, y + 1, z)] + g.potential[idx(x, y, z - 1)] + g.potential[idx(x, y, z + 1)];
                    next[idx(x, y, z)] = (neighbours + h2 * g.chargeDensity[idx(x, y, z)] / g.permittivity) / 6.0;
                }
        g.potential.swap(next);
    }
}

double MaxFieldMagnitude(const PoissonGrid& g)
{
    double best = 0.0;
    auto idx = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (static_cast<std::size_t>(z) * g.ny + y) * g.nx + x; };
    for (std::uint32_t z = 1; z + 1 < g.nz; ++z)
        for (std::uint32_t y = 1; y + 1 < g.ny; ++y)
            for (std::uint32_t x = 1; x + 1 < g.nx; ++x)
            {
                const double ex = (g.potential[idx(x + 1, y, z)] - g.potential[idx(x - 1, y, z)]) / (2.0 * g.spacing);
                const double ey = (g.potential[idx(x, y + 1, z)] - g.potential[idx(x, y - 1, z)]) / (2.0 * g.spacing);
                const double ez = (g.potential[idx(x, y, z + 1)] - g.potential[idx(x, y, z - 1)]) / (2.0 * g.spacing);
                best = std::max(best, std::sqrt(ex * ex + ey * ey + ez * ez));
            }
    return best;
}

} // namespace orbit::optical_transport
