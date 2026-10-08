#include "TectonicGrowth.hpp"

#include "ProceduralNoise.hpp"

#include <orbit/terrain/BakedTectonics.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <thread>

namespace orbit::terrain
{
namespace
{
constexpr f64 kClaimScale = 0.75;
constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();
constexpr f64 kFarClaim = -1.0e6;
// Box-blur radius (texels) of the arrival costs; see Blur().
constexpr i32 kBlurRadius = 4;

// Stencil: the 8-neighbourhood plus knight moves. Plain 8-neighbour Dijkstra
// has up to ~8% direction-dependent length error, which would imprint an
// octagonal bias on the plate shapes; 16 neighbours bring it to ~2%.
constexpr std::array<std::array<i32, 2>, 16> kStencil{{
    {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    {1, 2}, {1, -2}, {-1, 2}, {-1, -2}, {2, 1}, {2, -1}, {-2, 1}, {-2, -1}}};

struct Item
{
    f32 cost;
    u32 node;
    bool operator>(const Item& other) const noexcept { return cost > other.cost; }
};
using Queue = std::priority_queue<Item, std::vector<Item>, std::greater<Item>>;

struct Grid
{
    u32 resolution{0};
    std::size_t nodes{0};
    std::vector<std::array<f32, 3>> direction;
    std::vector<f32> multiplier;

    [[nodiscard]] std::size_t Index(u32 face, i32 x, i32 y) const noexcept
    {
        return (static_cast<std::size_t>(face) * resolution + static_cast<std::size_t>(y)) *
                   resolution + static_cast<std::size_t>(x);
    }

    // Texel x, y of a face, possibly outside it: resolves through the cube
    // edge to the nearest texel of the face that owns that direction.
    [[nodiscard]] std::size_t Resolve(u32 face, i32 x, i32 y) const noexcept
    {
        const i32 r = static_cast<i32>(resolution);
        if (x >= 0 && y >= 0 && x < r && y < r)
        {
            return Index(face, x, y);
        }
        const world::CubeCoordinate cube = world::UnitDirectionToCube(
            BakedTectonicTexelDirection(face, x, y, resolution));
        const f64 fx = (cube.uv.x + 1.0) * 0.5 * resolution - 0.5;
        const f64 fy = (cube.uv.y + 1.0) * 0.5 * resolution - 0.5;
        const i32 nx = std::clamp(static_cast<i32>(std::floor(fx + 0.5)), 0, r - 1);
        const i32 ny = std::clamp(static_cast<i32>(std::floor(fy + 0.5)), 0, r - 1);
        return Index(static_cast<u32>(cube.face), nx, ny);
    }

    [[nodiscard]] f32 Length(std::size_t a, std::size_t b) const noexcept
    {
        const auto& p = direction[a];
        const auto& q = direction[b];
        const f32 dx = p[0] - q[0];
        const f32 dy = p[1] - q[1];
        const f32 dz = p[2] - q[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

// Dijkstra from one source. With `limit` set, a node is settled (and expanded)
// only while its cost stays within limit[node].
void Grow(
    const Grid& grid,
    const std::size_t source,
    const f32 sourceCost,
    const std::vector<f32>* limit,
    std::vector<f32>& cost,
    const std::atomic<bool>* cancel)
{
    const i32 r = static_cast<i32>(grid.resolution);
    Queue queue;
    cost[source] = sourceCost;
    queue.push({sourceCost, static_cast<u32>(source)});
    u32 counter = 0;
    while (!queue.empty())
    {
        const Item item = queue.top();
        queue.pop();
        const std::size_t n = item.node;
        if (item.cost > cost[n])
        {
            continue;
        }
        if (limit != nullptr && item.cost > (*limit)[n])
        {
            cost[n] = kInfinity;
            continue;
        }
        if (cancel != nullptr && (++counter & 0xFFFFU) == 0U &&
            cancel->load(std::memory_order_relaxed))
        {
            return;
        }

        const u32 face = static_cast<u32>(n / (static_cast<std::size_t>(r) * r));
        const std::size_t within = n % (static_cast<std::size_t>(r) * r);
        const i32 y = static_cast<i32>(within / r);
        const i32 x = static_cast<i32>(within % r);
        for (const auto& offset : kStencil)
        {
            const std::size_t m = grid.Resolve(face, x + offset[0], y + offset[1]);
            if (m == n)
            {
                continue;
            }
            const f32 candidate = item.cost + grid.Length(n, m) * 0.5F *
                (grid.multiplier[n] + grid.multiplier[m]);
            if (candidate < cost[m])
            {
                cost[m] = candidate;
                queue.push({candidate, static_cast<u32>(m)});
            }
        }
    }
}
// Separable box blur on the cube-sphere grid (taps cross cube edges through
// Resolve), applied twice for a near-Gaussian of sigma ~ sqrt(2 r (r + 1) / 3)
// texels. The arrival costs are distance-like and have kinks along their cut
// loci (where shortest paths switch); the boundary normals are gradients, so a
// kink would be a straight-edged jump in the compression/shear balance.
void Blur(const Grid& grid, std::vector<f32>& values, const i32 radius)
{
    const i32 r = static_cast<i32>(grid.resolution);
    std::vector<f32> scratch(values.size());
    const f32 weight = 1.0F / static_cast<f32>(2 * radius + 1);
    for (u32 pass = 0U; pass < 4U; ++pass)
    {
        const bool alongX = (pass % 2U) == 0U;
        for (u32 face = 0U; face < kBakedTectonicFaces; ++face)
        {
            for (i32 y = 0; y < r; ++y)
            {
                for (i32 x = 0; x < r; ++x)
                {
                    f32 sum = 0.0F;
                    for (i32 k = -radius; k <= radius; ++k)
                    {
                        sum += values[grid.Resolve(face, alongX ? x + k : x, alongX ? y : y + k)];
                    }
                    scratch[grid.Index(face, x, y)] = sum * weight;
                }
            }
        }
        values.swap(scratch);
    }
}
} // namespace

std::shared_ptr<const TectonicGrowth> TectonicGrowth::Build(
    const detail::TectonicField& field,
    const u32 resolution,
    const u64 seed,
    const std::atomic<bool>* const cancel,
    u32 workers)
{
    const u32 plateCount = field.PlateCount();
    auto result = std::shared_ptr<TectonicGrowth>(new TectonicGrowth());
    result->resolution_ = resolution;
    result->plateCount_ = plateCount;
    result->claimBase_ = 1.0;

    Grid grid;
    grid.resolution = resolution;
    grid.nodes = static_cast<std::size_t>(kBakedTectonicFaces) * resolution * resolution;
    grid.direction.resize(grid.nodes);
    grid.multiplier.resize(grid.nodes);
    const u64 noiseSeed = seed ^ 0x47524F574854ULL;
    for (u32 face = 0; face < kBakedTectonicFaces; ++face)
    {
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const math::Double3 d = BakedTectonicTexelDirection(
                    face, static_cast<i32>(x), static_cast<i32>(y), resolution);
                const std::size_t n = grid.Index(face, static_cast<i32>(x), static_cast<i32>(y));
                grid.direction[n] = {static_cast<f32>(d.x), static_cast<f32>(d.y), static_cast<f32>(d.z)};
                // Lognormal travel cost: plate-scale basins and barriers, a
                // regional texture, and fine roughness that keeps the borders
                // fractal.
                const f64 field3 =
                    0.55 * detail::ValueNoise3D(d * 2.4, noiseSeed ^ 0x1ULL) +
                    0.30 * detail::ValueNoise3D(d * 7.0, noiseSeed ^ 0x2ULL) +
                    0.15 * detail::ValueNoise3D(d * 21.0, noiseSeed ^ 0x3ULL);
                grid.multiplier[n] = static_cast<f32>(std::exp(1.15 * field3));
            }
        }
    }

    // Seeds and head starts (larger plates start closer, like the claim bias).
    f64 maxBias = -1.0e9;
    for (u32 i = 0; i < plateCount; ++i)
    {
        maxBias = std::max(maxBias, field.PlateSizeBias(i));
    }
    std::vector<std::size_t> seeds(plateCount);
    std::vector<f32> startCost(plateCount);
    for (u32 i = 0; i < plateCount; ++i)
    {
        const world::CubeCoordinate cube = world::UnitDirectionToCube(field.PlateSeed(i));
        const i32 r = static_cast<i32>(resolution);
        const i32 x = std::clamp(static_cast<i32>(std::floor((cube.uv.x + 1.0) * 0.5 * resolution)), 0, r - 1);
        const i32 y = std::clamp(static_cast<i32>(std::floor((cube.uv.y + 1.0) * 0.5 * resolution)), 0, r - 1);
        seeds[i] = grid.Index(static_cast<u32>(cube.face), x, y);
        startCost[i] = static_cast<f32>((maxBias - field.PlateSizeBias(i)) / kClaimScale);
    }

    // Pass 1: the winning arrival cost everywhere.
    std::vector<f32> best(grid.nodes, kInfinity);
    {
        Queue queue;
        for (u32 i = 0; i < plateCount; ++i)
        {
            if (startCost[i] < best[seeds[i]])
            {
                best[seeds[i]] = startCost[i];
                queue.push({startCost[i], static_cast<u32>(seeds[i])});
            }
        }
        const i32 r = static_cast<i32>(resolution);
        u32 counter = 0;
        while (!queue.empty())
        {
            const Item item = queue.top();
            queue.pop();
            const std::size_t n = item.node;
            if (item.cost > best[n])
            {
                continue;
            }
            if (cancel != nullptr && (++counter & 0xFFFFU) == 0U &&
                cancel->load(std::memory_order_relaxed))
            {
                return nullptr;
            }
            const u32 face = static_cast<u32>(n / (static_cast<std::size_t>(r) * r));
            const std::size_t within = n % (static_cast<std::size_t>(r) * r);
            const i32 y = static_cast<i32>(within / r);
            const i32 x = static_cast<i32>(within % r);
            for (const auto& offset : kStencil)
            {
                const std::size_t m = grid.Resolve(face, x + offset[0], y + offset[1]);
                if (m == n)
                {
                    continue;
                }
                const f32 candidate = item.cost + grid.Length(n, m) * 0.5F *
                    (grid.multiplier[n] + grid.multiplier[m]);
                if (candidate < best[m])
                {
                    best[m] = candidate;
                    queue.push({candidate, static_cast<u32>(m)});
                }
            }
        }
    }

    // Pass 2: each plate's arrival cost, kept only within a boundary band of
    // the winner (plus slack, since the band test runs on settled costs).
    const f64 band =
        field.BoundaryWidth() / kClaimScale + 0.08;
    std::vector<f32> limit(grid.nodes);
    for (std::size_t n = 0; n < grid.nodes; ++n)
    {
        limit[n] = best[n] + static_cast<f32>(band);
    }
    result->cost_.assign(static_cast<std::size_t>(plateCount) * grid.nodes, kInfinity);

    workers = std::max(1U, std::min(workers, plateCount));
    std::atomic<u32> next{0U};
    const auto work = [&]()
    {
        for (;;)
        {
            const u32 plate = next.fetch_add(1U, std::memory_order_relaxed);
            if (plate >= plateCount)
            {
                return;
            }
            std::vector<f32> cost(grid.nodes, kInfinity);
            Grow(grid, seeds[plate], startCost[plate], &limit, cost, cancel);
            // Outside its band a plate carries the band edge value instead of
            // infinity: the field stays continuous (so the blur and the
            // gradient are well defined) and the plate still sits just outside
            // the candidate range of the winner.
            for (std::size_t n = 0; n < grid.nodes; ++n)
            {
                const f32 edge = limit[n] + 0.04F;
                cost[n] = std::isfinite(cost[n]) ? std::min(cost[n], edge) : edge;
            }
            Blur(grid, cost, kBlurRadius);
            std::copy(cost.begin(), cost.end(),
                result->cost_.begin() + static_cast<std::ptrdiff_t>(plate * grid.nodes));
        }
    };
    std::vector<std::thread> threads;
    for (u32 t = 1U; t < workers; ++t)
    {
        threads.emplace_back(work);
    }
    work();
    for (auto& thread : threads)
    {
        thread.join();
    }
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
    {
        return nullptr;
    }
    return result;
}

std::size_t TectonicGrowth::NodeAt(const u32 face, const i32 x, const i32 y) const noexcept
{
    const i32 r = static_cast<i32>(resolution_);
    if (x >= 0 && y >= 0 && x < r && y < r)
    {
        return (static_cast<std::size_t>(face) * resolution_ + static_cast<std::size_t>(y)) *
                   resolution_ + static_cast<std::size_t>(x);
    }
    const world::CubeCoordinate cube = world::UnitDirectionToCube(
        BakedTectonicTexelDirection(face, x, y, resolution_));
    const f64 fx = (cube.uv.x + 1.0) * 0.5 * resolution_ - 0.5;
    const f64 fy = (cube.uv.y + 1.0) * 0.5 * resolution_ - 0.5;
    const i32 nx = std::clamp(static_cast<i32>(std::floor(fx + 0.5)), 0, r - 1);
    const i32 ny = std::clamp(static_cast<i32>(std::floor(fy + 0.5)), 0, r - 1);
    return (static_cast<std::size_t>(cube.face) * resolution_ + static_cast<std::size_t>(ny)) *
               resolution_ + static_cast<std::size_t>(nx);
}

f64 TectonicGrowth::ClaimAt(const u32 plate, const std::size_t node) const noexcept
{
    const std::size_t nodes =
        static_cast<std::size_t>(kBakedTectonicFaces) * resolution_ * resolution_;
    const f32 cost = cost_[static_cast<std::size_t>(plate) * nodes + node];
    return std::isfinite(cost) ? claimBase_ - kClaimScale * static_cast<f64>(cost) : kFarClaim;
}

f64 TectonicGrowth::ClaimInterpolated(
    const u32 plate,
    const u32 face,
    const i32 x,
    const i32 y) const noexcept
{
    const i32 r = static_cast<i32>(resolution_);
    if (x >= 0 && y >= 0 && x < r && y < r)
    {
        return ClaimAt(plate, NodeAt(face, x, y));
    }
    const world::CubeCoordinate cube = world::UnitDirectionToCube(
        BakedTectonicTexelDirection(face, x, y, resolution_));
    const f64 fx = std::clamp((cube.uv.x + 1.0) * 0.5 * resolution_ - 0.5, 0.0, static_cast<f64>(r - 1));
    const f64 fy = std::clamp((cube.uv.y + 1.0) * 0.5 * resolution_ - 0.5, 0.0, static_cast<f64>(r - 1));
    const i32 x0 = std::min(static_cast<i32>(fx), r - 2);
    const i32 y0 = std::min(static_cast<i32>(fy), r - 2);
    const f64 tx = fx - x0;
    const f64 ty = fy - y0;
    const u32 f = static_cast<u32>(cube.face);
    const auto at = [&](const i32 px, const i32 py)
    {
        return ClaimAt(plate, NodeAt(f, px, py));
    };
    const f64 a = at(x0, y0);
    const f64 b = at(x0 + 1, y0);
    const f64 c = at(x0, y0 + 1);
    const f64 d = at(x0 + 1, y0 + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

void TectonicGrowth::Claims(
    const u32 face,
    const i32 x,
    const i32 y,
    detail::ClaimArray& claims) const noexcept
{
    for (u32 i = 0; i < plateCount_; ++i)
    {
        claims[i] = ClaimInterpolated(i, face, x, y);
    }
}

math::Double3 TectonicGrowth::BoundaryNormal(
    const u32 face,
    const i32 x,
    const i32 y,
    const u32 i,
    const u32 j) const noexcept
{
    // Gradient of F = claim_j - claim_i on the sphere from the four axis
    // neighbours: solve g . a = dFa, g . b = dFb in the tangent plane spanned
    // by the neighbour baselines a and b (not orthogonal near cube edges). The
    // baseline is several texels wide: the grown boundaries are stair-stepped
    // at texel scale, and a one-texel difference would turn that into normal
    // noise (and so speckled compression/shear classification).
    constexpr i32 kSpan = 3;
    const auto value = [&](const i32 dx, const i32 dy)
    {
        return ClaimInterpolated(j, face, x + dx, y + dy) - ClaimInterpolated(i, face, x + dx, y + dy);
    };
    const auto direction = [&](const i32 dx, const i32 dy)
    {
        return BakedTectonicTexelDirection(face, x + dx, y + dy, resolution_);
    };
    const math::Double3 a = direction(kSpan, 0) - direction(-kSpan, 0);
    const math::Double3 b = direction(0, kSpan) - direction(0, -kSpan);
    const f64 fa = value(kSpan, 0) - value(-kSpan, 0);
    const f64 fb = value(0, kSpan) - value(0, -kSpan);
    const f64 aa = math::Dot(a, a);
    const f64 ab = math::Dot(a, b);
    const f64 bb = math::Dot(b, b);
    const f64 det = aa * bb - ab * ab;
    if (!(std::abs(det) > 1.0e-18))
    {
        return {};
    }
    const f64 c0 = (fa * bb - fb * ab) / det;
    const f64 c1 = (fb * aa - fa * ab) / det;
    const math::Double3 gradient = a * c0 + b * c1;
    const f64 length = std::sqrt(math::LengthSquared(gradient));
    return length > 1.0e-12 ? gradient * (1.0 / length) : math::Double3{};
}
} // namespace orbit::terrain
