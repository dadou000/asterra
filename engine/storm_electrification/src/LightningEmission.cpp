#include <orbit/storm_electrification/LightningEmission.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace orbit::storm_electrification
{
namespace
{
using optical_transport::Pulse;
using optical_transport::Segment;

struct Rng
{
    std::uint64_t state;
    std::uint64_t Next()
    {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    double Uniform() { return static_cast<double>(Next() >> 11U) / 9007199254740992.0; }
    double Normal()
    {
        const double u = std::max(Uniform(), 1.0e-12), v = Uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
    }
};

// A plasma below this does not glow in the visible: the emission stops there, and the channel is treated as cold (a wave entering it is slow).
constexpr double kVisibleKelvin = 2800.0;

// One heating event of a segment, kept so that the temperature the channel still has when the next wave arrives is known: T = peak for
// `plateau` seconds from t0 (a current is flowing), then (peak - slow) exp(-t / tau1) + slow exp(-t / tau2).
struct Heat
{
    double t0;
    double peak;
    double slow;
    double tau1;
    double tau2;
    double plateau;
};

double Temperature(const Heat& h, const double t)
{
    if (t < h.t0) { return 0.0; }
    const double since = t - h.t0;
    if (since <= h.plateau) { return h.peak; }
    const double cooling = since - h.plateau;
    return (h.peak - h.slow) * std::exp(-cooling / h.tau1) + h.slow * std::exp(-cooling / h.tau2);
}

double Residual(const std::vector<Heat>& book, const double t)
{
    double hottest = 0.0;
    for (const Heat& h : book) { hottest = std::max(hottest, Temperature(h, t)); }
    return hottest;
}

// Peak temperature of the plasma for a current: 30 kK at 30 kA, a little hotter for more (measured 28 to 36 kK for strong strokes).
double PeakKelvin(const double amperes, const EmissionParams& p)
{
    return std::clamp(30000.0 * std::pow(std::max(amperes, 1.0) / p.referenceCurrent, 0.25), 8000.0, 36000.0);
}

double PowerPerMeter(const double amperes, const EmissionParams& p)
{
    return p.referencePowerPerMeter * std::pow(std::max(amperes, 1.0) / p.referenceCurrent, 1.5);
}

// How fast a wave runs along a channel at a given temperature: a hot, conducting channel carries it as fast as a return stroke
// (>= 10 kK), a cold one at the speed of a dart or recoil leader (<= 4 kK), in between on a logarithmic scale.
double WaveSpeed(const double kelvin, const EmissionParams& p)
{
    const double u = std::clamp((kelvin - 4000.0) / 6000.0, 0.0, 1.0);
    return p.dartLeaderSpeed * std::pow(p.returnStrokeSpeed / p.dartLeaderSpeed, u);
}

// One emission event on one segment: starting at `t0` at `peakKelvin` and `peakWattsPerMeter`, holding it for `plateau` seconds (a current
// flows) and then cooling with two time constants. The light is written as a few pulses, each with the mean temperature of its interval, so the
// colour moves from blue-white to orange with it; it stops where the plasma is too cold to glow.
void Emit(Segment& segment, std::vector<Heat>& book, const double length, const double t0, const double peakKelvin, const double peakWattsPerMeter,
          const double tau1, const double slowKelvin, const double tau2, const double plateau = 0.0)
{
    book.push_back({t0, peakKelvin, slowKelvin, tau1, tau2, plateau});
    const Heat heat = book.back();
    if (plateau > 0.0)
    {
        segment.pulses.push_back({t0, plateau, peakWattsPerMeter * length * plateau, peakKelvin});
    }
    const double start = t0 + plateau;
    double edges[11] = {0.0, 0.25 * tau1, 0.75 * tau1, 1.5 * tau1, 3.0 * tau1, 6.0 * tau1, 0.0, 0.0, 0.0, 0.0, 0.0};
    int count = 6;
    for (const double multiple : {1.0, 2.0, 3.5, 5.0, 8.0})
    {
        if (multiple * tau2 > edges[count - 1] * 1.3) { edges[count++] = multiple * tau2; }
    }
    const double floorWatts = 1.0e-6 * peakWattsPerMeter * length;
    for (int j = 0; j + 1 < count; ++j)
    {
        const double a = edges[j], b = edges[j + 1];
        const double kelvin = Temperature(heat, start + 0.5 * (a + b));
        if (kelvin < kVisibleKelvin) { break; }
        const double watts = peakWattsPerMeter * length * std::pow(kelvin / peakKelvin, 4.0);
        if (watts < floorWatts) { break; }
        segment.pulses.push_back({start + a, b - a, watts * (b - a), kelvin});
    }
}

// A return stroke or recoil event of a given current on a segment.
void Stroke(Segment& segment, std::vector<Heat>& book, const double length, const double t0, const double amperes, const EmissionParams& p)
{
    // Larger currents cool a little faster in the first millisecond (measured), but leave more heat behind: the slow phase grows with current.
    const double tau1 = 250.0e-6 * std::pow(p.referenceCurrent / std::max(amperes, 500.0), 0.1);
    const double tau2 = std::clamp(2.0e-3 * std::pow(std::max(amperes, 500.0) / 10.0e3, 0.7), 0.5e-3, 30.0e-3);
    Emit(segment, book, length, t0, PeakKelvin(amperes, p), PowerPerMeter(amperes, p), tau1, 4500.0, tau2);
}
} // namespace

double WaveSpeedForTemperature(const double kelvin, const EmissionParams& params)
{
    return WaveSpeed(kelvin, params);
}

void AssignPhysicalEmission(optical_transport::DischargeGraph& graph, const bool grounded, const std::int32_t groundSegment, const double attachTime,
                            const std::uint64_t seed, const EmissionParams& p)
{
    const std::size_t n = graph.Segments().size();
    if (n == 0) { return; }
    Rng rng{seed * 0x2545F4914F6CDD1DULL + 0x1234567ULL};

    std::vector<double> length(n), downstream(n, 1.0);
    std::vector<std::vector<std::int32_t>> children(n);
    std::vector<std::int32_t> roots;
    for (std::size_t i = 0; i < n; ++i)
    {
        const Segment& s = graph.Segments()[i];
        const double dx = s.to[0] - s.from[0], dy = s.to[1] - s.from[1], dz = s.to[2] - s.from[2];
        length[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (s.parent >= 0) { children[static_cast<std::size_t>(s.parent)].push_back(static_cast<std::int32_t>(i)); }
        else { roots.push_back(static_cast<std::int32_t>(i)); }
    }
    for (std::size_t i = n; i-- > 0;)
    {
        const std::int32_t parent = graph.Segments()[i].parent;
        if (parent >= 0) { downstream[static_cast<std::size_t>(parent)] += downstream[i]; }
    }
    double total = 0.0;
    for (const std::int32_t r : roots) { total += downstream[static_cast<std::size_t>(r)]; }
    total = std::max(total, 1.0);

    // The main path, from the root to the ground contact or to the end of the heaviest chain.
    std::vector<std::int32_t> path;
    if (grounded && groundSegment >= 0)
    {
        for (std::int32_t s = groundSegment; s >= 0; s = graph.Segments()[static_cast<std::size_t>(s)].parent) { path.push_back(s); }
        std::reverse(path.begin(), path.end());
    }
    else
    {
        std::int32_t at = -1;
        for (const std::int32_t r : roots)
        {
            if (at < 0 || downstream[static_cast<std::size_t>(r)] > downstream[static_cast<std::size_t>(at)]) { at = r; }
        }
        while (at >= 0)
        {
            path.push_back(at);
            std::int32_t next = -1;
            for (const std::int32_t c : children[static_cast<std::size_t>(at)])
            {
                if (next < 0 || downstream[static_cast<std::size_t>(c)] > downstream[static_cast<std::size_t>(next)]) { next = c; }
            }
            at = next;
        }
    }
    std::vector<char> onMain(n, 0);
    std::vector<double> fromEnd(n, 0.0);   // distance along the main path from its end (contact or tip) to the middle of the segment
    double pathLength = 0.0;
    for (const std::int32_t s : path) { onMain[static_cast<std::size_t>(s)] = 1; }
    for (std::size_t k = path.size(); k-- > 0;)
    {
        const std::size_t s = static_cast<std::size_t>(path[k]);
        fromEnd[s] = pathLength + 0.5 * length[s];
        pathLength += length[s];
    }
    pathLength = std::max(pathLength, 1.0);

    std::vector<std::vector<Heat>> heat(n);
    for (std::size_t i = 0; i < n; ++i) { graph.At(static_cast<std::int32_t>(i)).pulses.clear(); }

    // The stepped leader: a segment glows as it forms. The segments of the main path carry the leader current until it attaches and stay hot
    // (they are what the return stroke runs along); the others cool within milliseconds.
    constexpr double kLeaderWattsPerMeter = 2.0e4;   // about a fiftieth of a first return stroke (measured 1/20 to 1/100)
    for (std::size_t i = 0; i < n; ++i)
    {
        Segment& s = graph.At(static_cast<std::int32_t>(i));
        const double carried = onMain[i] ? std::max(0.0, attachTime - s.formedAt) : 0.0;
        Emit(s, heat[i], length[i], s.formedAt, 9000.0, kLeaderWattsPerMeter, 150.0e-6, 3500.0, 1.5e-3, carried);
    }

    const auto logNormal = [&](const double median, const double sigma) { return median * std::exp(sigma * rng.Normal()); };

    // A stroke along the main path (and into the branches). The first return stroke runs at the return stroke speed everywhere (the leader has
    // prepared the channel); the later events take the speed of the channel they enter from its temperature: fast in a channel that is still
    // hot, slow in one that has cooled.
    const auto strokeAlongMain = [&](const double t, const double amperes, const bool firstStroke, const bool branches) {
        std::vector<double> arrival(n, -1.0);
        double cursor = t;
        for (std::size_t k = path.size(); k-- > 0;)
        {
            const std::size_t i = static_cast<std::size_t>(path[k]);
            const double speed = firstStroke ? p.returnStrokeSpeed : WaveSpeed(Residual(heat[i], cursor), p);
            arrival[i] = cursor + 0.5 * length[i] / speed;
            cursor += length[i] / speed;
            const double current = amperes * (1.0 - 0.5 * fromEnd[i] / pathLength);
            Stroke(graph.At(static_cast<std::int32_t>(i)), heat[i], length[i], arrival[i], current, p);
        }
        if (!branches) { return; }
        for (std::size_t i = 0; i < n; ++i)
        {
            if (onMain[i]) { continue; }
            const std::int32_t parent = graph.Segments()[i].parent;
            const double parentArrival = parent >= 0 ? arrival[static_cast<std::size_t>(parent)] : t + pathLength / p.returnStrokeSpeed;
            if (parentArrival < 0.0) { continue; }
            const double speed = firstStroke ? p.returnStrokeSpeed : WaveSpeed(Residual(heat[i], parentArrival), p);
            arrival[i] = parentArrival + length[i] / speed;
            const double share = downstream[i] / total;
            if (share >= p.branchMinimumShare)
            {
                Stroke(graph.At(static_cast<std::int32_t>(i)), heat[i], length[i], arrival[i], amperes * std::min(0.5, 4.0 * share), p);
            }
        }
    };

    if (grounded)
    {
        const double first = std::clamp(logNormal(p.firstStrokeCurrent, p.currentSpread), 8.0e3, 120.0e3);
        strokeAlongMain(attachTime, first, true, true);
        double t = attachTime;
        const int subsequent = std::min(7, static_cast<int>(-std::log(std::max(rng.Uniform(), 1.0e-9)) * p.meanSubsequentStrokes));
        for (int k = 0; k < subsequent + 1; ++k)
        {
            const double strokeTime = t;
            double continuingEnd = strokeTime;
            if (rng.Uniform() < p.continuingCurrentChance)
            {
                // A continuing current: the whole path glows steadily while it flows (a current of a few hundred amperes), cools after it, and
                // it is crossed by M components (brief brightenings of a channel that is hot, so they run fast, with no dart leader).
                const bool longOne = rng.Uniform() < 0.14;
                const double duration = longOne ? 0.04 + 0.26 * rng.Uniform() : std::clamp(logNormal(0.008, 0.5), 0.003, 0.04);
                const double amperes = 100.0 + 200.0 * rng.Uniform();
                const double watts = 1.5e4 * amperes / 200.0;   // W/m: about a hundredth of a stroke
                for (const std::int32_t s : path)
                {
                    const std::size_t i = static_cast<std::size_t>(s);
                    Emit(graph.At(s), heat[i], length[i], strokeTime + 1.0e-3, 7000.0, watts, 0.8e-3, 3500.0, 6.0e-3, duration);
                }
                const int components = static_cast<int>(duration / 0.02 * (0.5 + rng.Uniform()));
                for (int m = 0; m < std::min(components, 4); ++m)
                {
                    const double when = strokeTime + 1.0e-3 + rng.Uniform() * duration;
                    strokeAlongMain(when, std::clamp(logNormal(1.2e3, 0.5), 400.0, 4.0e3), false, false);
                }
                continuingEnd = strokeTime + 1.0e-3 + duration;
            }
            if (k == subsequent) { break; }
            t = continuingEnd + std::clamp(logNormal(p.interStrokeMedian, p.interStrokeSpread), 0.012, 0.15);
            // A channel that is still glowing needs no dart leader: the next stroke is another flash along it. A cold one is first crossed by a
            // dim dart leader, a few 1e6 m/s.
            const std::size_t middle = static_cast<std::size_t>(path[path.size() / 2]);
            const bool hot = Residual(heat[middle], t - 1.0e-3) > 6000.0;
            if (!hot)
            {
                for (const std::int32_t s : path)
                {
                    const std::size_t i = static_cast<std::size_t>(s);
                    const double fromRoot = pathLength - fromEnd[i];
                    Emit(graph.At(s), heat[i], length[i], t - pathLength / p.dartLeaderSpeed + fromRoot / p.dartLeaderSpeed, 12000.0, 5.0e4, 100.0e-6, 3500.0, 1.5e-3);
                }
            }
            const double current = std::clamp(logNormal(p.subsequentCurrent, p.currentSpread), 3.0e3, 60.0e3);
            strokeAlongMain(t, current, hot, false);
        }
    }
    else
    {
        // The channel stops growing: the discharge of the whole tree (a first stroke without a ground), then a few recoil events along the main path.
        // The current of an intracloud discharge grows with the size of the channel: a few kA for a flash of a few tens of cells, much more for a big one.
        const double sizeScale = std::clamp(std::sqrt(static_cast<double>(n) / 150.0), 0.12, 2.0);
        const double discharge = std::clamp(logNormal(p.intracloudCurrent * sizeScale, p.currentSpread), 600.0, 40.0e3);
        strokeAlongMain(attachTime, discharge, true, true);
        double t = attachTime;
        const int events = 2 + static_cast<int>(rng.Uniform() * 4.0);
        for (int k = 0; k < events; ++k)
        {
            t += std::clamp(logNormal(0.012, 0.5), 0.004, 0.06);
            strokeAlongMain(t, std::clamp(logNormal(2.0e3 * sizeScale, 0.5), 300.0, 8.0e3), false, false);
        }
    }
}
} // namespace orbit::storm_electrification
