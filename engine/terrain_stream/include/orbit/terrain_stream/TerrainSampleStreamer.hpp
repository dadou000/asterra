#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_stream/ToroidalResidency.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>
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

    bool morphToCoarser{false};
    f64 morphStartHalfExtentMeters{0.0};
    f64 morphEndHalfExtentMeters{0.0};
    f64 coarseSpacingMeters{0.0};
    f64 coarseFootprintMeters{0.0};

    world::SurfaceFrame surfaceFrame{};
    world::SurfaceFrame coarseSurfaceFrame{};

    u32 originX{0};
    u32 originY{0};
    std::vector<PhysicalRegion> regions;
};

struct TerrainSampleValue
{
    f32 elevationMeters{0.0F};
    f32 morphTargetXMeters{0.0F};
    f32 morphTargetYMeters{0.0F};

    // Eight normalized biome weights packed as RGBA8 + RGBA8.
    u32 biomeWeights0{0};
    u32 biomeWeights1{0};
};

static_assert(
    sizeof(TerrainSampleValue) ==
    5U * sizeof(u32));

struct TerrainSamplePatch
{
    PhysicalRegion region{};
    std::vector<TerrainSampleValue> samples;
};

struct TerrainSampleResult
{
    u32 levelIndex{0};
    std::vector<TerrainSamplePatch> patches;
    u64 sampleCount{0};
};

namespace detail
{
struct TerrainSampleBatchState;
}

class TerrainSampleBatch
{
public:
    TerrainSampleBatch();
    ~TerrainSampleBatch();

    TerrainSampleBatch(
        const TerrainSampleBatch&) = delete;
    TerrainSampleBatch& operator=(
        const TerrainSampleBatch&) = delete;

    TerrainSampleBatch(
        TerrainSampleBatch&&) noexcept;
    TerrainSampleBatch& operator=(
        TerrainSampleBatch&&) noexcept;

    [[nodiscard]] bool IsComplete() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] u64 SourceRevision() const noexcept;

private:
    jobs::JobGroup group_;
    std::shared_ptr<
        detail::TerrainSampleBatchState>
        state_;

    friend class TerrainSampleStreamer;
};

class TerrainSampleStreamer
{
public:
    TerrainSampleStreamer(
        jobs::JobSystem& jobSystem,
        world::PlanetDefinition planet,
        const terrain::TerrainSource& terrainSource);

    [[nodiscard]] TerrainSampleBatch Submit(
        std::span<
            const TerrainSampleRequest> requests);

    [[nodiscard]] bool TryCollect(
        TerrainSampleBatch& batch,
        std::vector<TerrainSampleResult>& results);

    [[nodiscard]] std::vector<TerrainSampleResult>
    WaitCollect(
        TerrainSampleBatch& batch);

    [[nodiscard]] std::vector<TerrainSampleResult>
    GenerateBlocking(
        std::span<
            const TerrainSampleRequest> requests);

    [[nodiscard]] u64 SourceRevision() const noexcept;

private:
    [[nodiscard]] TerrainSamplePatch GeneratePatch(
        const TerrainSampleRequest& request,
        const PhysicalRegion& region) const;

    jobs::JobSystem& jobSystem_;
    world::PlanetDefinition planet_;
    const terrain::TerrainSource& terrainSource_;
};
} // namespace orbit::terrain_stream
