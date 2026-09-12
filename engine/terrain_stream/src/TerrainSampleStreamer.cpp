#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_stream
{
namespace detail
{
struct TerrainSampleBatchState
{
    std::vector<TerrainSampleRequest> requests;
    std::vector<TerrainSampleResult> results;
};
} // namespace detail

namespace
{
[[nodiscard]] u32 WrapIndex(
    const i64 value,
    const u32 size) noexcept
{
    const i64 modulus =
        static_cast<i64>(size);

    i64 wrapped =
        value % modulus;

    if (wrapped < 0)
    {
        wrapped += modulus;
    }

    return static_cast<u32>(wrapped);
}

void ValidateRequest(
    const TerrainSampleRequest& request)
{
    if (request.resolution < 2)
    {
        throw std::invalid_argument(
            "Orbit terrain sampling requires at least two samples per axis.");
    }

    if (request.spacingMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain sampling spacing must be positive.");
    }

    if (request.footprintMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain sampling footprint must be positive.");
    }

    if (request.morphToCoarser)
    {
        if (request.coarseSpacingMeters <= 0.0 ||
            request.coarseFootprintMeters <= 0.0)
        {
            throw std::invalid_argument(
                "Orbit terrain coarse morph sampling requires positive coarse spacing and footprint.");
        }

        if (request.morphEndHalfExtentMeters <=
            request.morphStartHalfExtentMeters)
        {
            throw std::invalid_argument(
                "Orbit terrain coarse morph range must have positive width.");
        }
    }

    if (request.originX >= request.resolution ||
        request.originY >= request.resolution)
    {
        throw std::invalid_argument(
            "Orbit terrain sampling toroidal origin is outside the sample window.");
    }

    for (const PhysicalRegion& region :
         request.regions)
    {
        if (region.width == 0 ||
            region.height == 0)
        {
            throw std::invalid_argument(
                "Orbit terrain sampling regions cannot be empty.");
        }

        if (region.x + region.width >
                request.resolution ||
            region.y + region.height >
                request.resolution)
        {
            throw std::invalid_argument(
                "Orbit terrain sampling region exceeds the physical sample window.");
        }
    }
}
} // namespace

TerrainSampleBatch::TerrainSampleBatch() = default;
TerrainSampleBatch::~TerrainSampleBatch() = default;
TerrainSampleBatch::TerrainSampleBatch(
    TerrainSampleBatch&&) noexcept = default;
TerrainSampleBatch&
TerrainSampleBatch::operator=(
    TerrainSampleBatch&&) noexcept = default;

bool TerrainSampleBatch::IsComplete() const noexcept
{
    return group_.IsComplete();
}

bool TerrainSampleBatch::IsValid() const noexcept
{
    return state_ != nullptr;
}

TerrainSampleStreamer::TerrainSampleStreamer(
    jobs::JobSystem& jobSystem,
    const world::PlanetDefinition planet,
    const terrain::TerrainSource& terrainSource)
    : jobSystem_(jobSystem),
      planet_(planet),
      terrainSource_(terrainSource)
{
    if (planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain sampling requires a positive planet radius.");
    }
}

TerrainSampleBatch TerrainSampleStreamer::Submit(
    const std::span<
        const TerrainSampleRequest> requests)
{
    TerrainSampleBatch batch;

    batch.state_ =
        std::make_shared<
            detail::TerrainSampleBatchState>();

    batch.state_->requests.assign(
        requests.begin(),
        requests.end());

    batch.state_->results.resize(
        requests.size());

    for (std::size_t requestIndex = 0;
         requestIndex <
            batch.state_->requests.size();
         ++requestIndex)
    {
        const TerrainSampleRequest& request =
            batch.state_->requests[
                requestIndex];

        ValidateRequest(request);

        TerrainSampleResult& result =
            batch.state_->results[
                requestIndex];

        result.levelIndex =
            request.levelIndex;

        result.patches.resize(
            request.regions.size());

        for (const PhysicalRegion& region :
             request.regions)
        {
            result.sampleCount +=
                static_cast<u64>(
                    region.width) *
                static_cast<u64>(
                    region.height);
        }
    }

    const auto state =
        batch.state_;

    for (std::size_t requestIndex = 0;
         requestIndex <
            state->requests.size();
         ++requestIndex)
    {
        const TerrainSampleRequest& request =
            state->requests[
                requestIndex];

        for (std::size_t regionIndex = 0;
             regionIndex <
                request.regions.size();
             ++regionIndex)
        {
            jobSystem_.Submit(
                batch.group_,
                [this,
                 state,
                 requestIndex,
                 regionIndex]
                {
                    const TerrainSampleRequest&
                        localRequest =
                            state->requests[
                                requestIndex];

                    state->results[
                        requestIndex].
                        patches[
                            regionIndex] =
                                GeneratePatch(
                                    localRequest,
                                    localRequest.
                                        regions[
                                            regionIndex]);
                });
        }
    }

    return batch;
}

bool TerrainSampleStreamer::TryCollect(
    TerrainSampleBatch& batch,
    std::vector<TerrainSampleResult>& results)
{
    if (!batch.IsValid() ||
        !batch.IsComplete())
    {
        return false;
    }

    jobSystem_.Wait(
        batch.group_);

    results =
        std::move(
            batch.state_->results);

    batch.state_.reset();
    return true;
}

std::vector<TerrainSampleResult>
TerrainSampleStreamer::WaitCollect(
    TerrainSampleBatch& batch)
{
    if (!batch.IsValid())
    {
        return {};
    }

    jobSystem_.Wait(
        batch.group_);

    std::vector<TerrainSampleResult> results =
        std::move(
            batch.state_->results);

    batch.state_.reset();
    return results;
}

std::vector<TerrainSampleResult>
TerrainSampleStreamer::GenerateBlocking(
    const std::span<
        const TerrainSampleRequest> requests)
{
    TerrainSampleBatch batch =
        Submit(requests);

    return WaitCollect(batch);
}

TerrainSamplePatch
TerrainSampleStreamer::GeneratePatch(
    const TerrainSampleRequest& request,
    const PhysicalRegion& region) const
{
    TerrainSamplePatch patch{};
    patch.region = region;

    patch.samples.resize(
        static_cast<std::size_t>(
            region.width) *
        static_cast<std::size_t>(
            region.height));

    const f64 halfCells =
        static_cast<f64>(
            request.resolution - 1U) *
        0.5;

    for (u32 localY = 0;
         localY < region.height;
         ++localY)
    {
        const u32 physicalY =
            region.y + localY;

        const u32 logicalY =
            WrapIndex(
                static_cast<i64>(
                    physicalY) -
                static_cast<i64>(
                    request.originY),
                request.resolution);

        for (u32 localX = 0;
             localX < region.width;
             ++localX)
        {
            const u32 physicalX =
                region.x + localX;

            const u32 logicalX =
                WrapIndex(
                    static_cast<i64>(
                        physicalX) -
                    static_cast<i64>(
                        request.originX),
                    request.resolution);

            const math::Double2 offsetMeters{
                (static_cast<f64>(
                     logicalX) -
                 halfCells) *
                    request.spacingMeters,
                (static_cast<f64>(
                     logicalY) -
                 halfCells) *
                    request.spacingMeters
            };

            const math::Double3 direction =
                world::DirectionAtSurfaceOffset(
                    planet_,
                    request.surfaceFrame,
                    offsetMeters);

            f64 elevation =
                terrainSource_.Sample({
                    .unitDirection =
                        direction,
                    .footprintMeters =
                        request.
                            footprintMeters
                }).elevationMeters;

            math::Double2 morphTarget =
                offsetMeters;

            if (request.morphToCoarser)
            {
                const math::Double2
                    coarseLocalOffset =
                        world::
                            SurfaceOffsetBetweenDirections(
                                planet_,
                                request.
                                    coarseSurfaceFrame,
                                direction);

                const math::Double2
                    snappedCoarseOffset{
                        std::round(
                            coarseLocalOffset.x /
                            request.
                                coarseSpacingMeters) *
                            request.
                                coarseSpacingMeters,
                        std::round(
                            coarseLocalOffset.y /
                            request.
                                coarseSpacingMeters) *
                            request.
                                coarseSpacingMeters
                    };

                const math::Double3
                    coarseDirection =
                        world::
                            DirectionAtSurfaceOffset(
                                planet_,
                                request.
                                    coarseSurfaceFrame,
                                snappedCoarseOffset);

                morphTarget =
                    world::
                        SurfaceOffsetBetweenDirections(
                            planet_,
                            request.
                                surfaceFrame,
                            coarseDirection);

                const f64 edgeDistance =
                    std::max(
                        std::abs(
                            offsetMeters.x),
                        std::abs(
                            offsetMeters.y));

                const f64 normalized =
                    std::clamp(
                        (edgeDistance -
                         request.
                            morphStartHalfExtentMeters) /
                            (request.
                                morphEndHalfExtentMeters -
                             request.
                                morphStartHalfExtentMeters),
                        0.0,
                        1.0);

                const f64 morph =
                    normalized *
                    normalized *
                    (3.0 -
                     2.0 * normalized);

                if (morph > 0.0)
                {
                    const f64 coarseElevation =
                        terrainSource_.Sample({
                            .unitDirection =
                                coarseDirection,
                            .footprintMeters =
                                request.
                                    coarseFootprintMeters
                        }).elevationMeters;

                    elevation =
                        elevation *
                            (1.0 - morph) +
                        coarseElevation *
                            morph;
                }
            }

            patch.samples[
                static_cast<std::size_t>(
                    localY) *
                    region.width +
                localX] = {
                    .elevationMeters =
                        static_cast<f32>(
                            elevation),
                    .morphTargetXMeters =
                        static_cast<f32>(
                            morphTarget.x),
                    .morphTargetYMeters =
                        static_cast<f32>(
                            morphTarget.y)
                };
        }
    }

    return patch;
}
} // namespace orbit::terrain_stream
