#include <orbit/terrain_stream/UniformPlanetMesh.hpp>
#include <orbit/math/Vector.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace orbit::terrain_stream
{
namespace
{
// Octahedral normal encoding (Cigolle et al.), quantized to two snorm16
// lanes packed into one u32. Enough precision for lighting a baked,
// never-updated mesh; avoids storing three raw floats per vertex.
[[nodiscard]] u32 PackOctahedralNormal(const math::Double3& n)
{
    const f64 l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    const f64 invL1 = l1 > 0.0 ? 1.0 / l1 : 0.0;
    f64 ox = n.x * invL1;
    f64 oy = n.y * invL1;
    if (n.z < 0.0)
    {
        const f64 wrappedX =
            (1.0 - std::abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        const f64 wrappedY =
            (1.0 - std::abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        ox = wrappedX;
        oy = wrappedY;
    }
    const auto quantize = [](const f64 v)
    {
        const auto q = static_cast<i32>(
            std::clamp(v, -1.0, 1.0) * 32'767.0);
        return static_cast<u32>(static_cast<u16>(q));
    };
    return quantize(ox) | (quantize(oy) << 16U);
}
}

u32 UniformPlanetResolution(const u32 lod)
{
    if (lod > kMaximumUniformPlanetLod)
        throw std::invalid_argument("Uniform planet LOD must be between 0 and " +
            std::to_string(kMaximumUniformPlanetLod) + ".");
    return (32U << lod) + 1U;
}
UniformPlanetMesh BuildUniformPlanetMesh(const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source, const u32 lod)
{
    UniformPlanetMesh mesh{.lod = lod, .resolution = UniformPlanetResolution(lod)};
    const u32 n = mesh.resolution;
    const u32 cells = n - 1U;
    const std::size_t faceSamples = static_cast<std::size_t>(n) * n;
    mesh.samples.resize(faceSamples * 6U);
    const f64 footprint = planet.radiusMeters * 1.5707963267948966 / cells;
    const auto pack = [](const f32 a, const f32 b, const f32 c, const f32 d)
    {
        const auto q = [](const f32 v) { return static_cast<u32>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F); };
        return q(a) | (q(b) << 8U) | (q(c) << 16U) | (q(d) << 24U);
    };
    for (u32 face = 0; face < 6; ++face)
    {
        for (u32 y = 0; y < n; ++y)
        {
            for (u32 x = 0; x < n; ++x)
            {
                const auto direction = world::CubeToUnitDirection({static_cast<world::CubeFace>(face),
                    {2.0 * x / cells - 1.0, 2.0 * y / cells - 1.0}});
                const auto sample = source.Sample({direction, footprint});
                const auto b = terrain::NormalizeBiomeWeights(sample.biomes);
                mesh.samples[face * faceSamples + static_cast<std::size_t>(y) * n + x] = {
                    static_cast<f32>(sample.elevationMeters), static_cast<f32>(sample.standingWaterDepthMeters),
                    pack(b.ocean, b.desert, b.grassland, b.temperateForest),
                    pack(b.borealForest, b.tundra, b.alpine, b.wetland)};
            }
        }
    }
    // Bake the surface normal once here (finite-differenced from the
    // already-sampled neighbor heights) instead of leaving the vertex
    // shader to redo this same neighbor lookup and cross product every
    // single frame for a mesh that never changes once built.
    const auto directionAt = [cells](const u32 face, const u32 x, const u32 y)
    {
        return world::CubeToUnitDirection({static_cast<world::CubeFace>(face),
            {2.0 * x / cells - 1.0, 2.0 * y / cells - 1.0}});
    };
    for (u32 face = 0; face < 6; ++face)
    {
        for (u32 y = 0; y < n; ++y)
        {
            for (u32 x = 0; x < n; ++x)
            {
                const u32 left = x > 0 ? x - 1U : 0U;
                const u32 right = std::min(x + 1U, n - 1U);
                const u32 down = y > 0 ? y - 1U : 0U;
                const u32 up = std::min(y + 1U, n - 1U);
                const auto heightAt = [&](const u32 hx, const u32 hy)
                {
                    const auto& s = mesh.samples[face * faceSamples + static_cast<std::size_t>(hy) * n + hx];
                    return static_cast<f64>(s.elevationMeters) + static_cast<f64>(s.waterDepthMeters);
                };
                const math::Double3 dx =
                    directionAt(face, right, y) * (planet.radiusMeters + heightAt(right, y)) -
                    directionAt(face, left, y) * (planet.radiusMeters + heightAt(left, y));
                const math::Double3 dy =
                    directionAt(face, x, up) * (planet.radiusMeters + heightAt(x, up)) -
                    directionAt(face, x, down) * (planet.radiusMeters + heightAt(x, down));
                math::Double3 normal = math::Normalize(math::Cross(dx, dy));
                if (math::Dot(normal, directionAt(face, x, y)) < 0.0)
                {
                    normal = normal * -1.0;
                }
                mesh.samples[face * faceSamples + static_cast<std::size_t>(y) * n + x].normalOctXY =
                    PackOctahedralNormal(normal);
            }
        }
    }
    mesh.indices.reserve(static_cast<std::size_t>(cells) * cells * 6U);
    for (u32 y = 0; y < cells; ++y)
        for (u32 x = 0; x < cells; ++x)
        {
            const u32 a = y * n + x;
            mesh.indices.insert(mesh.indices.end(), {a, a + n, a + 1U, a + 1U, a + n, a + n + 1U});
        }
    return mesh;
}
}
