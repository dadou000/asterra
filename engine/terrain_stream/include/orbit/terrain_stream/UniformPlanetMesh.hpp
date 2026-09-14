#pragma once
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>
#include <vector>

namespace orbit::terrain_stream
{
inline constexpr u32 kMaximumUniformPlanetLod = 8;
struct UniformPlanetSample
{
    f32 elevationMeters{};
    f32 waterDepthMeters{};
    u32 biome0{};
    u32 biome1{};
    // Octahedral-encoded surface normal (two snorm16 lanes), baked once
    // at build time instead of finite-differencing neighbor heights in
    // the vertex shader every frame -- this mesh is static once built.
    u32 normalOctXY{};
};
static_assert(sizeof(UniformPlanetSample) == 20);
struct UniformPlanetMesh
{
    u32 lod{};
    u32 resolution{};
    std::vector<UniformPlanetSample> samples;
    // One face's topology, reused for all six faces.
    std::vector<u32> indices;
};
[[nodiscard]] u32 UniformPlanetResolution(u32 lod);
[[nodiscard]] UniformPlanetMesh BuildUniformPlanetMesh(
    const world::PlanetDefinition& planet, const terrain::TerrainSource& source, u32 lod);
}
