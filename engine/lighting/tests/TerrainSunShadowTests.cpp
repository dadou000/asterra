#include <orbit/lighting/TerrainSunShadow.hpp>
#include <orbit/celestial_lighting/CelestialLighting.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <stop_token>

namespace
{
class Source final : public orbit::terrain::TerrainSource
{
public:
    bool ridge{false};
    bool invalid{false};
    bool changeRevisionDuringBake{false};
    mutable std::atomic<orbit::u64> revision{42U};
    mutable std::atomic<orbit::u32> sampleCount{0U};
    orbit::terrain::TerrainSample Sample(const orbit::terrain::TerrainQuery& q) const noexcept override
    {
        if (changeRevisionDuringBake && sampleCount.fetch_add(1U) == 128U)
            revision.store(43U);
        if (invalid) return {.elevationMeters = std::numeric_limits<double>::quiet_NaN()};
        const double y = q.unitDirection.y / 0.07;
        return {.elevationMeters = ridge && q.unitDirection.x > 0.8 ? 100.0 * std::exp(-y * y) : 0.0};
    }
    orbit::u64 Revision() const noexcept override { return revision.load(); }
};

// A coastal shelf (sea or land) with a ridge ~300 m ahead along the equator.
class Coast final : public orbit::terrain::TerrainSource
{
public:
    bool sea{false};
    double land{5.0};
    orbit::terrain::TerrainSample Sample(const orbit::terrain::TerrainQuery& q) const noexcept override
    {
        const double lon = std::atan2(q.unitDirection.z, q.unitDirection.x);
        const double y = q.unitDirection.y / 0.3;
        const double lonRidge = (lon - 0.29) / 0.03;
        const double ridge = 100.0 * std::exp(-lonRidge * lonRidge - y * y);
        const bool shelf = std::abs(lon) < 0.1 && std::abs(q.unitDirection.y) < 0.3;
        return {.elevationMeters = ridge + (shelf ? (sea ? -20.0 : land) : 0.0)};
    }
    orbit::u64 Revision() const noexcept override { return 7U; }
};
}

int main()
{
    using namespace orbit::lighting;
    {
        // Grazing sun (2 degrees) toward a ridge: land is shadowed, but a point
        // on open water is not terrain-shadowed (v152 reference trace).
        Coast coast;
        const double elevation = std::numbers::pi / 90.0;
        const orbit::math::Double3 sun{std::sin(elevation), 0.0, std::cos(elevation)};
        coast.sea = false;
        const auto landField = BuildTerrainSunShadowField(coast, 1000.0, 512U, 256U);
        coast.sea = true;
        const auto seaField = BuildTerrainSunShadowField(coast, 1000.0, 512U, 256U);
        if (TerrainSunVisibility(landField, {1005.5, 0.0, 0.0}, sun) != 0.0) return 30;
        if (TerrainSunVisibility(seaField, {1000.5, 0.0, 0.0}, sun) != 1.0) return 31;
        // The same sea point, lifted clear of the water, is shadowed again.
        if (TerrainSunVisibility(seaField, {1030.0, 0.0, 0.0}, sun) == 1.0) return 32;
        // A pixel below the smoothed field ground (true terrain lower than the
        // coarse caster) must not self-shadow: sun 30 degrees up, away from the ridge.
        coast.sea = false;
        coast.land = 50.0;
        const auto hillField = BuildTerrainSunShadowField(coast, 1000.0, 512U, 256U);
        const orbit::math::Double3 away{0.5, 0.0, -0.8660254037844386};
        if (TerrainSunVisibility(hillField, {1010.0, 0.0, 0.0}, away) != 1.0) return 33;
    }
    Source source;
    const auto flat = BuildTerrainSunShadowField(source, 1000.0, 128U, 64U);
    if (flat.sourceRevision != 42U || flat.minimumElevationMeters != 0.0 ||
        flat.maximumElevationMeters != 0.0) return 1;
    if (TerrainSunVisibility(flat, {0, -1001, 0}, {0, 1, 0}) != 0.0) return 2;
    if (TerrainSunVisibility(flat, {0, 1001, 0}, {0, 1, 0}) != 1.0) return 3;
    // The sphere alone leaves this grazing ray lit. A mountain away from the
    // observer blocks it; no framebuffer/camera is available to the query.
    if (TerrainSunVisibility(flat, {1000, -200, 0}, {0, 1, 0}) != 1.0) return 4;
    source.ridge = true;
    const auto ridge = BuildTerrainSunShadowField(source, 1000.0, 128U, 64U);
    if (TerrainSunVisibility(ridge, {1000, -200, 0}, {0, 1, 0}) != 0.0) return 5;
    if (TerrainSunVisibility(ridge, {1000, -200, 0}, {0, -1, 0}) != 1.0) return 6;
    if (TerrainSunVisibility(ridge, {1150, -200, 0}, {0, 1, 0}) != 1.0) return 7;
    if (TerrainSunVisibility(ridge, {1000, -200, 0}, {0, 10, 0}) != 0.0) return 8;
    // Wrapped longitude must not introduce a seam into sun traversal.
    if (TerrainSunVisibility(flat, {-1000, -200, 0.001}, {0, 1, 0}) !=
        TerrainSunVisibility(flat, {-1000, -200, -0.001}, {0, 1, 0})) return 9;
    // A finite stellar disc must create continuous partial visibility, and
    // a larger apparent disc produces a wider penumbra at the same blocker.
    const double partial = TerrainSunVisibility(flat, {1000, -200, 0}, {0, 1, 0}, 0.02);
    if (!(partial > 0.0 && partial < 1.0)) return 11;
    const double smallStar = TerrainSunVisibility(flat, {1005, -200, 0}, {0, 1, 0}, 0.02);
    const double largeStar = TerrainSunVisibility(flat, {1005, -200, 0}, {0, 1, 0}, 0.05);
    if (!(largeStar < smallStar && largeStar > 0.0)) return 12;
    if (TerrainSunVisibility(ridge, {1000, -200, 0}, {0, 1, 0}, 0.02) != 0.0) return 13;
    const auto fraction = orbit::celestial_lighting::DiscHorizonVisibleFraction;
    if (std::abs(fraction(0.0, 0.005) - 0.5) > 1.0e-12 ||
        fraction(-0.01, 0.005) != 0.0 || fraction(0.01, 0.005) != 1.0) return 14;
    for (int i = -99; i <= 99; ++i)
    {
        const double angle = static_cast<double>(i) * 0.00005;
        if (std::abs(fraction(angle, 0.005) + fraction(-angle, 0.005) - 1.0) > 1.0e-12) return 15;
    }
    // Two limb envelopes are a monotone reduction: the minimum over all
    // terrain samples of disc visibility must equal one disc integration
    // at the tightest horizon angle (the production CPU/HLSL fast path).
    for (double alpha : {0.005, 0.02, 0.06})
    {
        const std::array<double, 8> clearances{
            -0.04, 0.01, 0.002, -0.003, 0.023, 0.051, -0.012, 0.0};
        double legacy = 1.0, worst = std::numeric_limits<double>::infinity();
        for (double angle : clearances)
        {
            legacy = std::min(legacy, fraction(angle, alpha));
            worst = std::min(worst, angle);
        }
        if (std::abs(legacy - fraction(worst, alpha)) > 1.0e-14) return 16;
    }
    // A changed erosion/terrain revision must stop the background build
    // before it spends a whole 128x64 field's worth of source samples.
    source.sampleCount = 0U;
    source.changeRevisionDuringBake = true;
    try
    {
        static_cast<void>(BuildTerrainSunShadowField(source, 1000.0, 128U, 64U));
        return 17;
    }
    catch (const std::runtime_error&) {}
    if (source.sampleCount >= 128U * 64U) return 18;
    source.changeRevisionDuringBake = false;

    // Slopes order horizon candidates identically to atan2, provided the
    // traversal's ray distance is positive. The finite disc's two contact
    // limits can therefore be tested without per-sample inverse trig.
    for (double alpha : {0.001, 0.005, 0.05})
    {
        double slow = std::numeric_limits<double>::infinity();
        double fast = std::numeric_limits<double>::infinity();
        for (unsigned i = 1U; i <= 1024U; ++i)
        {
            const double distance = 2.0 + 0.25 * i;
            const double clearance = 3.0 * std::sin(i * 0.077) - 1.0;
            slow = std::min(slow, std::atan2(clearance, distance));
            fast = std::min(fast, clearance / distance);
        }
        if (std::abs(slow - std::atan(fast)) > 1.0e-14 ||
            (slow <= -alpha) != (fast <= -std::tan(alpha))) return 19;
    }
    std::stop_source canceled;
    canceled.request_stop();
    try
    {
        static_cast<void>(BuildTerrainSunShadowField(source, 1000.0,
            128U, 64U, canceled.get_token()));
        return 20;
    }
    catch (const std::runtime_error&) {}

    source.invalid = true;
    try
    {
        static_cast<void>(BuildTerrainSunShadowField(source, 1000.0, 16U, 8U));
        return 10;
    }
    catch (const std::runtime_error&) {}
    return 0;
}
