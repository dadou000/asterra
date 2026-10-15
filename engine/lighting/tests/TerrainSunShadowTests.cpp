#include <orbit/lighting/TerrainSunShadow.hpp>
#include <orbit/celestial_lighting/CelestialLighting.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
class Source final : public orbit::terrain::TerrainSource
{
public:
    bool ridge{false};
    bool invalid{false};
    orbit::terrain::TerrainSample Sample(const orbit::terrain::TerrainQuery& q) const noexcept override
    {
        if (invalid) return {.elevationMeters = std::numeric_limits<double>::quiet_NaN()};
        const double y = q.unitDirection.y / 0.07;
        return {.elevationMeters = ridge && q.unitDirection.x > 0.8 ? 100.0 * std::exp(-y * y) : 0.0};
    }
    orbit::u64 Revision() const noexcept override { return 42U; }
};
}

int main()
{
    using namespace orbit::lighting;
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
    source.invalid = true;
    try
    {
        static_cast<void>(BuildTerrainSunShadowField(source, 1000.0, 16U, 8U));
        return 10;
    }
    catch (const std::runtime_error&) {}
    return 0;
}
