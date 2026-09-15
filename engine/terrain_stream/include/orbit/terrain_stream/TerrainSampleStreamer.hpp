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

    // Shading normals are sampled at this footprint/epsilon regardless
    // of this level's own (possibly much coarser) geometric spacing, so
    // every ring shades with the same ground-truth micro-relief the
    // finest clipmap level would see up close instead of a blocky
    // normal derived from wide-apart geometry samples. Zero falls back
    // to this level's own footprint/spacing (legacy behavior).
    f64 fineNormalFootprintMeters{0.0};
    f64 fineNormalEpsilonMeters{0.0};

    // Offset of this level's snapped window center inside surfaceFrame's
    // stable spherical lattice. Ordinary clipmap motion changes this by exact
    // integer cell steps while surfaceFrame stays fixed, so retained toroidal
    // samples keep their world-space address.
    math::Double2 centerOffsetMeters{};

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
    f32 standingWaterDepthMeters{0.0F};

    // Ground-truth slope (d elevation / d east, d elevation / d north),
    // sampled at the request's fine-normal footprint/epsilon -- see
    // TerrainSampleRequest above. Independent of this level's own
    // geometric sample spacing.
    f32 fineSlopeEast{0.0F};
    f32 fineSlopeNorth{0.0F};
};

static_assert(
    sizeof(TerrainSampleValue) ==
    8U * sizeof(u32));

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

    // Finite-differences the terrain source at a small, LOD-independent
    // epsilon to recover the actual ground micro-slope at `offsetMeters`
    // within `surfaceFrame`, instead of the level's own (possibly huge)
    // sample spacing.
    [[nodiscard]] math::Double2 SampleFineSlope(
        const world::SurfaceFrame& surfaceFrame,
        const math::Double2& offsetMeters,
        f64 footprintMeters,
        f64 epsilonMeters) const noexcept;

    jobs::JobSystem& jobSystem_;
    world::PlanetDefinition planet_;
    const terrain::TerrainSource& terrainSource_;
};
} // namespace orbit::terrain_stream
