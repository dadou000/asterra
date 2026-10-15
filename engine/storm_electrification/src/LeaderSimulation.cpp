#include "orbit/storm_electrification/LeaderSimulation.hpp"

#include "orbit/storm_electrification/LightningEmission.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace orbit::storm_electrification
{

namespace
{
using optical_transport::FlashKind;
using optical_transport::Pulse;
using optical_transport::Segment;

// SplitMix64: a small, well-mixed generator so flash `n` of seed `s` is reproducible without any shared state.
struct Rng
{
    std::uint64_t state;
    std::uint64_t Next()
    {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double Uniform() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
};

double DensityFactor(const LeaderParams& p, double height) { return std::exp(-height / p.densityScaleHeight); }

struct Cell { std::uint32_t i, j, k; };
}

double MaxBreakdownRatio(const ChargeGrid& g, const LeaderParams& p, std::uint32_t* bi, std::uint32_t* bj, std::uint32_t* bk)
{
    double best = 0.0;
    for (std::uint32_t k = 0; k < g.Nz(); ++k)
    {
        const double threshold = p.initiationField * DensityFactor(p, g.Height(k));
        for (std::uint32_t j = 0; j < g.Ny(); ++j)
            for (std::uint32_t i = 0; i < g.Nx(); ++i)
            {
                if (g.Refractory(g.Index(i, j, k))) continue;
                const double ratio = g.FieldMagnitude(i, j, k) / threshold;
                if (ratio > best)
                {
                    best = ratio;
                    if (bi) { *bi = i; *bj = j; *bk = k; }
                }
            }
    }
    return best;
}

FlashResult TryFlash(ChargeGrid& grid, const LeaderParams& p, double startTime, std::uint64_t flashIndex)
{
    FlashResult out;
    Rng rng{p.seed * 0x2545F4914F6CDD1DULL + flashIndex * 0x9E3779B97F4A7C15ULL + 12345ULL};
    // A small flash: short, never to the ground, and it does not solve the potential again.
    const bool small = p.smallFlashFraction > 0.0 && rng.Uniform() < p.smallFlashFraction;
    std::uint32_t maxCells = p.maxChannelCells;
    if (small)
    {
        const double u = std::max(rng.Uniform(), 1.0e-12), v = rng.Uniform();
        const double normal = std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
        maxCells = static_cast<std::uint32_t>(std::clamp(p.smallFlashMedianCells * std::exp(0.6 * normal), 8.0, static_cast<double>(p.smallFlashMaxCells)));
    }
    if (!small) { grid.SolvePotential(); }
    std::uint32_t si = 0, sj = 0, sk = 0;
    const double strongest = MaxBreakdownRatio(grid, p, &si, &sj, &sk);
    if (strongest < 1.0)
    {
        return out;
    }
    if (p.initiationSpread > 0.0)
    {
        // A cell chosen uniformly (one pass, reservoir sampling) among those with a field of at least (1 - spread) of the strongest.
        const double bar = std::max(1.0, (1.0 - p.initiationSpread) * strongest);
        double seen = 0.0;
        for (std::uint32_t k = 0; k < grid.Nz(); ++k)
        {
            const double threshold = p.initiationField * DensityFactor(p, grid.Height(k));
            for (std::uint32_t j = 0; j < grid.Ny(); ++j)
                for (std::uint32_t i = 0; i < grid.Nx(); ++i)
                {
                    if (grid.Refractory(grid.Index(i, j, k)) || grid.FieldMagnitude(i, j, k) / threshold < bar) continue;
                    seen += 1.0;
                    if (rng.Uniform() * seen < 1.0) { si = i; sj = j; sk = k; }
                }
        }
    }
    out.energyBefore = grid.FieldEnergy();

    const double h = grid.Spacing();
    const auto key = [&](const Cell& c) { return static_cast<std::uint64_t>(grid.Index(c.i, c.j, c.k)); };
    const auto centre = [&](const Cell& c, double v[3]) { v[0] = (c.i + 0.5) * h; v[1] = (c.j + 0.5) * h; v[2] = (c.k + 0.5) * h; };

    // Channel bookkeeping: cell -> segment index of the segment that created it (root has parent -1).
    std::vector<Cell> channel{{si, sj, sk}};
    std::unordered_map<std::uint64_t, std::int32_t> segmentOf;
    std::unordered_set<std::uint64_t> hasChild;   // cells that already grew a branch
    segmentOf[key(channel[0])] = -1;
    double channelPotential = grid.Potential()[grid.Index(si, sj, sk)];
    double groundTime = 0.0;
    std::int32_t groundSegment = -1;

    optical_transport::DischargeGraph graph(FlashKind::IntraCloud);
    const auto addCellSegment = [&](const Cell& parent, const Cell& cell, double time) {
        Segment s;
        centre(parent, s.from);
        centre(cell, s.to);
        s.parent = segmentOf[key(parent)];
        s.formedAt = time;
        s.conductivity = 1.0;
        const double length = h;
        s.pulses.push_back({time, h / p.leaderSpeed, p.leaderEnergyPerMeter * length});
        const std::int32_t index = graph.AddSegment(std::move(s));
        segmentOf[key(cell)] = index;
        return index;
    };

    // Charge the channel could neutralise, by polarity, counted once per cell of the neighbourhood it touches.
    std::vector<unsigned char> counted(grid.CellCount(), 0);
    double reachablePositive = 0.0, reachableNegative = 0.0;
    const double cellVolume = h * h * h;
    const long radius = static_cast<long>(p.neutralizationRadiusCells);
    const auto touch = [&](const Cell& c) {
        for (long dz = -radius; dz <= radius; ++dz)
            for (long dy = -radius; dy <= radius; ++dy)
                for (long dx = -radius; dx <= radius; ++dx)
                {
                    const long x = static_cast<long>(c.i) + dx, y = static_cast<long>(c.j) + dy, z = static_cast<long>(c.k) + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= static_cast<long>(grid.Nx()) || y >= static_cast<long>(grid.Ny()) ||
                        z >= static_cast<long>(grid.Nz()))
                        continue;
                    const std::size_t cell = grid.Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z));
                    if (counted[cell]) continue;
                    counted[cell] = 1;
                    for (const double q : {grid.IceCharge()[cell], grid.GraupelCharge()[cell]})
                        (q > 0.0 ? reachablePositive : reachableNegative) += std::abs(q) * p.neutralizationFraction * cellVolume;
                }
    };
    touch(channel[0]);
    bool grounded = false;
    // An ungrounded channel neutralises equal amounts of both polarities, so the smaller one limits it; a grounded
    // channel delivers its net charge to the ground, so either polarity reaching the budget is enough.
    const auto budgetReached = [&] {
        if (channel.size() < p.minChannelCells) return false;
        return (grounded ? std::max(reachablePositive, reachableNegative) : std::min(reachablePositive, reachableNegative)) >=
               p.maxFlashCharge;
    };

    // Growth rounds: every round each tip may extend; the channel front moves at leaderSpeed.
    double time = startTime;
    std::uint32_t round = 0;
    // Some flashes are descending leaders that seek the ground.
    const bool seekGround = !small && p.groundSeekChance > 0.0 && rng.Uniform() < p.groundSeekChance;
    // Where the channel is going: the cell with the largest potential difference from it (the opposite charge), or the ground below its start.
    double goalI = si, goalJ = sj, goalK = sk;
    if (!small && p.goalBias > 0.0)
    {
        if (seekGround) { goalK = 0.0; }
        else
        {
            double largest = -1.0;
            for (std::uint32_t k = 0; k < grid.Nz(); ++k)
                for (std::uint32_t j = 0; j < grid.Ny(); ++j)
                    for (std::uint32_t i = 0; i < grid.Nx(); ++i)
                    {
                        const double difference = std::abs(grid.Potential()[grid.Index(i, j, k)] - channelPotential);
                        if (difference > largest) { largest = difference; goalI = i; goalJ = j; goalK = k; }
                    }
        }
    }

    // Neighbour steps: the 6 face neighbours, or all 26 when the channel may run diagonally.
    std::vector<std::array<int, 3>> d;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
            {
                const int nonzero = (dx != 0) + (dy != 0) + (dz != 0);
                if (nonzero == 0 || (nonzero > 1 && !p.diagonalGrowth)) continue;
                d.push_back({dx, dy, dz});
            }
    std::uint32_t idleRounds = 0;
    // A descending leader does not stop for its charge budget or for crossing air with no charge, only when it connects (or at the cell cap).
    while (channel.size() < maxCells && ((seekGround && !grounded) || (!budgetReached() && idleRounds < p.stagnationRounds)))
    {
        const double reachedBefore = reachablePositive + reachableNegative;
        struct Candidate { Cell cell; Cell parent; double weight; };
        std::vector<Candidate> candidates;
        std::unordered_map<std::uint64_t, std::size_t> best;
        for (const Cell& c : channel)
        {
            // Only the tips (cells with no child yet) extend, plus an occasional side branch from the middle of the channel.
            if (p.sideBranchChance < 1.0 && hasChild.count(key(c)) && rng.Uniform() > p.sideBranchChance) continue;
            for (const auto& dir : d)
            {
                const long x = static_cast<long>(c.i) + dir[0], y = static_cast<long>(c.j) + dir[1], z = static_cast<long>(c.k) + dir[2];
                if (x < 1 || y < 1 || z < 0 || x >= static_cast<long>(grid.Nx()) - 1 || y >= static_cast<long>(grid.Ny()) - 1 ||
                    z >= static_cast<long>(grid.Nz()) - 1)
                    continue;
                const Cell n{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z)};
                if (segmentOf.count(key(n))) continue;
                const double phi = grid.Potential()[grid.Index(n.i, n.j, n.k)];
                const double threshold = p.propagationField * DensityFactor(p, grid.Height(n.k));
                // Positive end extends toward lower potential, negative end toward higher: either way the potential
                // step across the new cell must beat the propagation field.
                const double stepLength = h * std::sqrt(static_cast<double>(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]));
                const double excess = std::abs(phi - channelPotential) / stepLength - threshold;
                if (excess <= 0.0) continue;
                double weight = p.growthExponent == 1.0 ? excess : std::pow(excess, p.growthExponent);
                if (p.screening < 1.0)
                {
                    int touching = 0;
                    for (const auto& around : d)
                    {
                        const long ax = x + around[0], ay = y + around[1], az = z + around[2];
                        if (ax < 0 || ay < 0 || az < 0) continue;
                        const Cell other{static_cast<std::uint32_t>(ax), static_cast<std::uint32_t>(ay), static_cast<std::uint32_t>(az)};
                        if (segmentOf.count(key(other))) ++touching;
                    }
                    if (touching > 1) weight *= std::pow(p.screening, static_cast<double>(touching - 1));
                }
                if (!small && p.goalBias > 0.0)
                {
                    const double gx = goalI - static_cast<double>(c.i), gy = goalJ - static_cast<double>(c.j), gz = goalK - static_cast<double>(c.k);
                    const double gl = std::sqrt(gx * gx + gy * gy + gz * gz);
                    const double sl = std::sqrt(static_cast<double>(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]));
                    if (gl > 0.0 && sl > 0.0)
                    {
                        weight *= 1.0 + p.goalBias * std::max(0.0, (gx * dir[0] + gy * dir[1] + gz * dir[2]) / (gl * sl));
                    }
                }
                if (seekGround)
                {
                    const double down = std::max(0.0, -static_cast<double>(dir[2]) / std::sqrt(static_cast<double>(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2])));
                    weight *= 1.0 + p.groundSeekBias * down;
                }
                if (p.directionPersistence > 0.0)
                {
                    const std::int32_t reaching = segmentOf[key(c)];
                    if (reaching >= 0)
                    {
                        const Segment& in = graph.Segments()[static_cast<std::size_t>(reaching)];
                        const double hx = in.to[0] - in.from[0], hy = in.to[1] - in.from[1], hz = in.to[2] - in.from[2];
                        const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
                        const double sl = std::sqrt(static_cast<double>(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]));
                        if (hl > 0.0 && sl > 0.0)
                        {
                            const double cosine = (hx * dir[0] + hy * dir[1] + hz * dir[2]) / (hl * sl);
                            weight *= 1.0 + p.directionPersistence * std::max(0.0, cosine);
                        }
                    }
                }
                const auto found = best.find(key(n));
                if (found == best.end())
                {
                    best[key(n)] = candidates.size();
                    candidates.push_back({n, c, weight});
                }
                else if (weight > candidates[found->second].weight)
                {
                    candidates[found->second].parent = c;
                }
            }
        }
        if (candidates.empty()) break;
        // Extend by a bounded number of cells per round chosen proportionally to their excess field.
        const std::size_t picks = std::max<std::size_t>(
            1, std::min<std::size_t>(candidates.size(), 1 + static_cast<std::size_t>(static_cast<double>(channel.size()) / std::max(p.picksDivisor, 1.0e-3))));
        for (std::size_t n = 0; n < picks && !candidates.empty(); ++n)
        {
            double total = 0.0;
            for (const auto& c : candidates) total += c.weight;
            double r = rng.Uniform() * total;
            std::size_t chosen = 0;
            for (; chosen + 1 < candidates.size(); ++chosen)
            {
                r -= candidates[chosen].weight;
                if (r <= 0.0) break;
            }
            const Candidate pick = candidates[chosen];
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(chosen));
            if (segmentOf.count(key(pick.cell))) continue;
            const std::int32_t index = addCellSegment(pick.parent, pick.cell, time);
            hasChild.insert(key(pick.parent));
            channel.push_back(pick.cell);
            touch(pick.cell);
            if (!grounded && static_cast<double>(pick.cell.k) < p.groundContactHeight)
            {
                grounded = true;
                groundTime = time;
                groundSegment = index;
                channelPotential = 0.0;   // a connection to the ground pins the whole channel at 0 V
            }
        }
        idleRounds = (reachablePositive + reachableNegative > reachedBefore) ? 0 : idleRounds + 1;
        ++round;
        time += h / p.leaderSpeed;
    }
    out.channelCells = static_cast<std::uint32_t>(channel.size());
    out.endTime = time;
    if (channel.size() < 3)
    {
        return out;   // the field exceeded threshold but no channel could propagate: no flash
    }

    // Classification and the bright stage.
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (const Cell& c : channel)
    {
        double v[3];
        centre(c, v);
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], v[a]); hi[a] = std::max(hi[a], v[a]); }
    }
    const double span = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    out.kind = grounded ? FlashKind::CloudToGround : (span > p.crossCloudSpanMeters ? FlashKind::CloudToCloud : FlashKind::IntraCloud);
    optical_transport::DischargeGraph bright(out.kind);
    for (const auto& s : graph.Segments()) bright.AddSegment(s);
    if (grounded)
    {
        // Return stroke: a current wave up the path from the ground contact to the root at returnStrokeSpeed.
        std::vector<std::int32_t> path;
        for (std::int32_t s = groundSegment; s >= 0; s = bright.Segments()[static_cast<std::size_t>(s)].parent) path.push_back(s);
        double pathLength = h * static_cast<double>(path.size());
        const double perMeter = p.returnStrokeEnergy / std::max(pathLength, h);
        double travelled = 0.0;
        for (const std::int32_t s : path)
        {
            bright.Reactivate(s, {out.endTime + travelled / p.returnStrokeSpeed, p.pulseDuration, perMeter * h});
            travelled += h;
        }
        (void)groundTime;
    }
    else
    {
        // No return stroke: the channel is bright where it conducts, fading as it forms (every cell glows as it is made
        // and again a little later when the opposite polarity reaches it).
        const double lateTime = out.endTime;
        const std::size_t count = bright.Segments().size();
        // The energy of a segment: equal shares, or (currentWeighted) in proportion to the segments downstream of it, the charge it carries.
        std::vector<double> share(count, 1.0);
        if (p.currentWeighted && count > 0)
        {
            std::vector<double> downstream(count, 1.0);
            for (std::size_t s = count; s-- > 0;)
            {
                const std::int32_t parent = bright.Segments()[s].parent;
                if (parent >= 0) downstream[static_cast<std::size_t>(parent)] += downstream[s];
            }
            double total = 0.0;
            for (const double v : downstream) total += v;
            for (std::size_t s = 0; s < count; ++s) share[s] = downstream[s] * static_cast<double>(count) / total;   // the mean stays 1
        }
        for (std::size_t s = 0; s < count; ++s)
        {
            bright.Reactivate(static_cast<std::int32_t>(s), {lateTime + 1.0e-4 * rng.Uniform(), p.pulseDuration, p.intracloudEnergyPerMeter * h * share[s]});
        }
    }
    if (p.physicalEmission)
    {
        AssignPhysicalEmission(bright, grounded, groundSegment, out.endTime, p.seed * 0x9E3779B97F4A7C15ULL + flashIndex);
    }
    out.graph = std::move(bright);

    // Neutralisation near the channel. Positive and negative charge are removed in equal amounts unless the channel
    // is grounded, in which case the imbalance is delivered to the ground.
    std::vector<unsigned char> near(grid.CellCount(), 0);
    const long r = radius;
    for (const Cell& c : channel)
        for (long dz = -r; dz <= r; ++dz)
            for (long dy = -r; dy <= r; ++dy)
                for (long dx = -r; dx <= r; ++dx)
                {
                    const long x = static_cast<long>(c.i) + dx, y = static_cast<long>(c.j) + dy, z = static_cast<long>(c.k) + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= static_cast<long>(grid.Nx()) || y >= static_cast<long>(grid.Ny()) ||
                        z >= static_cast<long>(grid.Nz()))
                        continue;
                    near[grid.Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z))] = 1;
                }
    // A flash must dissipate field energy. Removing equal charge from a high- and a low-potential region does so to first
    // order, but with a large charge moved over a large channel the second-order term can win, so the neutralisation is
    // tried at full strength and halved (from a snapshot) until the energy drops; if it never does, no flash happens.
    const std::vector<double> iceBefore = grid.IceCharge(), graupelBefore = grid.GraupelCharge();
    bool accepted = false;
    for (int attempt = 0; attempt < 4 && !accepted; ++attempt)
    {
        const double strength = std::pow(0.5, attempt);
        grid.IceCharge() = iceBefore;
        grid.GraupelCharge() = graupelBefore;
        out.positiveRemoved = 0.0;
        out.negativeRemoved = 0.0;
        const double fraction = p.neutralizationFraction * strength;
        double removablePositive = 0.0, removableNegative = 0.0;
        for (std::size_t c = 0; c < grid.CellCount(); ++c)
        {
            if (!near[c]) continue;
            for (const double q : {grid.IceCharge()[c], grid.GraupelCharge()[c]})
            {
                (q > 0.0 ? removablePositive : removableNegative) += std::abs(q) * fraction * cellVolume;
            }
        }
        double scalePositive = 1.0, scaleNegative = 1.0;
        if (!grounded)
        {
            const double common = std::min(removablePositive, removableNegative);
            scalePositive = removablePositive > 0.0 ? common / removablePositive : 0.0;
            scaleNegative = removableNegative > 0.0 ? common / removableNegative : 0.0;
        }
        // Never neutralise more than the flash budget of either polarity, whatever the local charge density.
        if (removablePositive > p.maxFlashCharge) scalePositive = std::min(scalePositive, p.maxFlashCharge / removablePositive);
        if (removableNegative > p.maxFlashCharge) scaleNegative = std::min(scaleNegative, p.maxFlashCharge / removableNegative);
        if (!grounded)
        {
            // Keep the two polarities equal after capping so an ungrounded flash still conserves charge exactly.
            const double positive = removablePositive * scalePositive, negative = removableNegative * scaleNegative;
            const double common = std::min(positive, negative);
            scalePositive = positive > 0.0 ? scalePositive * common / positive : 0.0;
            scaleNegative = negative > 0.0 ? scaleNegative * common / negative : 0.0;
        }
        for (std::size_t c = 0; c < grid.CellCount(); ++c)
        {
            if (!near[c]) continue;
            for (std::vector<double>* carrier : {&grid.IceCharge(), &grid.GraupelCharge()})
            {
                double& q = (*carrier)[c];
                const double scale = q > 0.0 ? scalePositive : scaleNegative;
                const double removed = q * fraction * scale;
                q -= removed;
                (removed > 0.0 ? out.positiveRemoved : out.negativeRemoved) += std::abs(removed) * cellVolume;
            }
        }
        if (small)
        {
            out.energyAfter = out.energyBefore;   // not solved again: a small flash is taken to dissipate field energy
            accepted = true;
        }
        else
        {
            grid.SolvePotential();
            out.energyAfter = grid.FieldEnergy();
            accepted = out.energyAfter < out.energyBefore;
        }
    }
    if (!accepted)
    {
        grid.IceCharge() = iceBefore;
        grid.GraupelCharge() = graupelBefore;
        grid.SolvePotential();
        out.positiveRemoved = out.negativeRemoved = 0.0;
        out.energyAfter = out.energyBefore;
        return out;   // the channel could not be made to dissipate energy: nothing happened
    }
    out.groundCharge = out.positiveRemoved - out.negativeRemoved;   // net charge taken out of the cloud
    if (small)
    {
        // The next flash of this step starts elsewhere: the cells within a few of the channel are set aside until the potential is solved again.
        std::vector<unsigned char> around(grid.CellCount(), 0U);
        for (const Cell& c : channel)
            for (long dz = -4; dz <= 4; ++dz)
                for (long dy = -4; dy <= 4; ++dy)
                    for (long dx = -4; dx <= 4; ++dx)
                    {
                        const long x = static_cast<long>(c.i) + dx, y = static_cast<long>(c.j) + dy, z = static_cast<long>(c.k) + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= static_cast<long>(grid.Nx()) || y >= static_cast<long>(grid.Ny()) || z >= static_cast<long>(grid.Nz())) continue;
                        around[grid.Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z))] = 1U;
                    }
        grid.MarkRefractory(around);
    }
    out.occurred = true;
    out.startTime = startTime;
    return out;
}

} // namespace orbit::storm_electrification
