#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <orbit/mesh_render/ReflectionBvh.hpp>
#include <random>

namespace
{
int failures = 0;
void Check(bool condition, const char *name)
{
    if (!condition)
    {
        std::cerr << name << '\n';
        ++failures;
    }
}
} // namespace
int main()
{
    using namespace orbit;
    using namespace mesh_render;
    mesh_import::MeshAsset asset;
    asset.materials.emplace_back();
    asset.materials[0].metallicFactor = 0.0F;
    // Two opposing walls, plus many separated triangles to exercise skips.
    for (u32 i = 0; i < 128; ++i)
    {
        const f32 z = i == 0U ? 5.0F : i == 1U ? 2.0F : 10.0F + static_cast<f32>(i);
        const f32 x = i < 2U ? 0.0F : 20.0F;
        const u32 first = static_cast<u32>(asset.vertices.size());
        for (const auto &p :
             std::array<std::array<f32, 3>, 3>{{{x - 1, -1, z}, {x + 1, -1, z}, {x, 1, z}}})
        {
            mesh_import::MeshVertex v;
            v.position = p;
            v.normal = {0, 0, -1};
            asset.vertices.push_back(v);
        }
        asset.indices.insert(asset.indices.end(), {first, first + 1U, first + 2U});
    }
    asset.parts.push_back({0U, static_cast<u32>(asset.indices.size()), 0U});
    const auto triangles = BuildReflectionTriangles(asset);
    const auto bvh = BuildReflectionBvh(triangles);
    Check(bvh.triangles.size() == 128U && !bvh.nodes.empty(), "build complete geometry");
    Check(bvh.nodes[0].escape == bvh.nodes.size(), "root escape terminates traversal");
    for (std::size_t i = 0; i < bvh.nodes.size(); ++i)
    {
        const auto &n = bvh.nodes[i];
        Check(n.escape > i && n.escape <= bvh.nodes.size(), "preorder escape stays in bounds");
        Check(n.first + n.count <= bvh.triangles.size(), "leaf spans remain in bounds");
    }
    const auto near = TraceReflectionBvh(bvh, {0, 0, 0}, {0, 0, 1}, 0.001F, 1000.0F);
    Check(near.hit && std::abs(near.distance - 2.0F) < 1e-5F && near.normal.z < 0,
          "nearest hit across BVH branches");
    Check(!TraceReflectionBvh(bvh, {0, 0, 0}, {0, 0, 1}, 0.001F, 1.5F).hit,
          "maximum distance is respected");
    Check(!TraceReflectionBvh(bvh, {0, 0, 0}, {1, 0, 0}, 0.001F, 1000.0F).hit,
          "parallel slab ray misses");
    const auto inside = TraceReflectionBvh(bvh, {0, 0, 3}, {0, 0, 1}, 0.001F, 100.0F);
    Check(inside.hit && std::abs(inside.distance - 2.0F) < 1e-5F,
          "ray originating within scene bounds");
    const std::array<f32, 12> rows{0, 0, 2, 3, 0, 2, 0, 0, -2, 0, 0, 0};
    const auto rotated = BuildReflectionBvh({TransformReflectionTriangle(triangles[1], rows)});
    const auto transformed = TraceReflectionBvh(rotated, {0, 0, 0}, {1, 0, 0}, 0.001F, 20.0F);
    Check(transformed.hit && std::abs(transformed.distance - 7.0F) < 1e-5F,
          "rotation uniform scale translation");
    Check(transformed.normal.x < -.99F, "normal follows instance transform");
    // Compare nearest-hit results to individual triangles for random rays.
    std::mt19937 random(5U);
    std::uniform_real_distribution<f32> angle(-.8F, .8F);
    for (u32 ray = 0; ray < 250; ++ray)
    {
        const math::Float3 d{angle(random), angle(random), 1.0F};
        const auto actual = TraceReflectionBvh(bvh, {0, 0, 0}, d, .001F, 1000.0F);
        bool found = false;
        f32 distance = 1000.0F;
        for (const auto &t : triangles)
        {
            const auto expected =
                TraceReflectionBvh(BuildReflectionBvh({t}), {0, 0, 0}, d, .001F, 1000.0F);
            if (expected.hit)
            {
                found = true;
                distance = std::min(distance, expected.distance);
            }
        }
        Check(actual.hit == found && (!found || std::abs(actual.distance - distance) < 1e-4F),
              "BVH matches exhaustive nearest triangle");
    }
    auto malformed = asset;
    malformed.indices[0] = 999999U;
    Check(BuildReflectionTriangles(malformed).size() == 127U, "invalid indices are skipped");
    malformed = asset;
    malformed.vertices[0].position[0] = std::numeric_limits<f32>::quiet_NaN();
    Check(BuildReflectionTriangles(malformed).size() == 127U, "nonfinite triangles are skipped");
    malformed = asset;
    malformed.materials[0].alphaMode = mesh_import::AlphaMode::Mask;
    Check(BuildReflectionTriangles(malformed).empty(),
          "cutouts are not falsely promoted to opaque geometry");
    Check(BuildReflectionBvh({}).nodes.empty(), "empty scene clears all geometry");
    return failures == 0 ? 0 : 1;
}
