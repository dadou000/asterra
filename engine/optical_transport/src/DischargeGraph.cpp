#include "orbit/optical_transport/DischargeGraph.hpp"

#include "orbit/optical_transport/Blackbody.hpp"

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
            double weightedColour[3] = {0.0, 0.0, 0.0};
            double coloured = 0.0;
            for (const Pulse& p : s.pulses)
            {
                const double a = std::max(t0, p.startTime), b = std::min(t1, p.startTime + p.duration);
                double part = 0.0;
                if (p.duration > 0.0 && b > a) part = p.radiantEnergy * (b - a) / p.duration;
                else if (p.duration <= 0.0 && p.startTime >= t0 && p.startTime < t1) part = p.radiantEnergy;
                e += part;
                if (part > 0.0 && p.temperatureKelvin > 0.0)
                {
                    const auto rgb = BlackbodyLinearRgb(p.temperatureKelvin);
                    for (int k = 0; k < 3; ++k) weightedColour[k] += part * rgb[static_cast<std::size_t>(k)];
                    coloured += part;
                }
            }
            if (e <= 0.0) continue;
            double mid[3];
            for (int k = 0; k < 3; ++k) mid[k] = 0.5 * (s.from[k] + s.to[k]);
            auto key = std::make_tuple(static_cast<long>(std::floor((mid[0] - lo[0]) / cell)), static_cast<long>(std::floor((mid[1] - lo[1]) / cell)),
                                       static_cast<long>(std::floor((mid[2] - lo[2]) / cell)));
            Cluster& c = cells[key];
            for (int k = 0; k < 3; ++k) c.position[k] += e * mid[k];
            c.energy += e;
            if (coloured > 0.0)
            {
                // Accumulated as energy x colour; normalised when the cell is emitted.
                for (int k = 0; k < 3; ++k) c.colour[k] = (c.hasColour ? c.colour[k] : 0.0) + weightedColour[k];
                c.hasColour = true;
            }
        }
        if (cells.size() <= maxClusters || cell > extent * 2.0)
        {
            for (auto& [key, c] : cells)
            {
                if (c.energy > 0.0) for (int k = 0; k < 3; ++k) c.position[k] /= c.energy;
                if (c.hasColour)
                {
                    const double peak = std::max({c.colour[0], c.colour[1], c.colour[2], 1.0e-30});
                    for (int k = 0; k < 3; ++k) c.colour[k] /= peak;
                }
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

} // namespace orbit::optical_transport
