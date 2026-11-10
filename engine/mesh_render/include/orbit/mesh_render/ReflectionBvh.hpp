#pragma once
#include <orbit/lighting/ReflectionScene.hpp>
#include <orbit/mesh_import/MeshAsset.hpp>
#include <span>
#include <vector>

namespace orbit::mesh_render
{
struct ReflectionBvh
{
    std::vector<lighting::ReflectionTriangle> triangles;
    std::vector<lighting::ReflectionBvhNode> nodes;
};
// Skips malformed/degenerate and non-opaque parts instead of treating cutouts
// as solid mirrors. The field remains their approximate fallback.
[[nodiscard]] std::vector<lighting::ReflectionTriangle> BuildReflectionTriangles(
    const mesh_import::MeshAsset &asset);
[[nodiscard]] ReflectionBvh BuildReflectionBvh(std::vector<lighting::ReflectionTriangle> triangles);
[[nodiscard]] lighting::ReflectionTriangle TransformReflectionTriangle(
    const lighting::ReflectionTriangle &triangle, const std::array<f32, 12> &rows);
struct ReflectionHit
{
    bool hit{false};
    f32 distance{0.0F};
    u32 triangle{0U};
    math::Float3 normal{};
};
// CPU reference for nearest-hit and GPU-layout verification.
[[nodiscard]] ReflectionHit TraceReflectionBvh(const ReflectionBvh &bvh, math::Float3 origin,
                                               math::Float3 direction, f32 minimumDistance,
                                               f32 maximumDistance);
} // namespace orbit::mesh_render
