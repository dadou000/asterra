#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_stream/ToroidalResidency.hpp>
#include <orbit/world/Planet.hpp>

#include <span>
#include <vector>

namespace orbit::terrain_stream
{
struct TerrainSampleRequest
{
    u32 levelIndex{0};
    u32 resolution{0};
    f64 spacingMeters{0.0};
    f64 footprintMeters{0.0};
    world::SurfaceFrame surfaceFrame{};
    u32 originX{0};
    u32 originY{0};
    std::vector<PhysicalRegion> regions;
};

struct TerrainSamplePatch
{
    PhysicalRegion region{};
    std::vector<f32> elevations;
};

struct TerrainSampleResult
{
    u32 levelIndex{0};
    std::vector<TerrainSamplePatch> patches;
    u64 sampleCount{0};
};

class TerrainSampleStreamer
{
public:
    TerrainSampleStreamer(
        jobs::JobSystem& jobSystem,
        world::PlanetDefinition planet,
        const terrain::TerrainSource& terrainSource);

    [[nodiscard]] std::vector<TerrainSampleResult>
    GenerateBlocking(
        std::span<const TerrainSampleRequest> requests);

private:
    [[nodiscard]] TerrainSamplePatch GeneratePatch(
        const TerrainSampleRequest& request,
        const PhysicalRegion& region) const;

    jobs::JobSystem& jobSystem_;
    world::PlanetDefinition planet_;
    const terrain::TerrainSource& terrainSource_;
};
} // namespace orbit::terrain_stream
