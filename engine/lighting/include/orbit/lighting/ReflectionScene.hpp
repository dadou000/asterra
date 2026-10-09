#pragma once

#include <array>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/rhi/AccelerationStructure.hpp>
#include <orbit/rhi/Resource.hpp>

namespace orbit::lighting
{
// Compact exact geometry and material factors; no descriptor indexing required.
// Texture detail is supplied by the existing surface field when available.
struct ReflectionTriangle
{
    math::Float4 p0{};
    math::Float4 edge1{};
    math::Float4 edge2{};
    math::Float4 normal0{};
    math::Float4 normal1{};
    math::Float4 normal2{};
    math::Float4 albedoMetallic{};
    math::Float4 emissionRoughness{};
};
struct ReflectionBvhNode
{
    math::Float3 minimum{};
    u32 first{0U};
    math::Float3 maximum{};
    u32 count{0U}; // zero = branch, children immediately follow in preorder
    u32 escape{0U};
    std::array<u32, 3> reserved{};
};
static_assert(sizeof(ReflectionTriangle) == 128U);
static_assert(sizeof(ReflectionBvhNode) == 48U);

struct ReflectionSceneInput
{
    rhi::Buffer *triangles{nullptr};
    rhi::AccelerationStructure *acceleration{nullptr};
    rhi::Buffer *nodes{nullptr};
    u32 nodeCount{0U};
    u64 revision{0U};
    math::Double3 originInFrameMeters{};
    math::Float3 toSun{0.0F, 1.0F, 0.0F};
    math::Float3 sunIrradiance{};
    math::Float3 skyIrradiance{};
    math::Float3 localUp{0.0F, 1.0F, 0.0F};
};
} // namespace orbit::lighting
