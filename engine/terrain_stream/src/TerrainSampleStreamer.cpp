#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <orbit/math/Vector.hpp>

#include <stdexcept>

namespace orbit::terrain_stream
{
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

std::vector<TerrainSampleResult>
TerrainSampleStreamer::GenerateBlocking(
    const std::span<
        const TerrainSampleRequest> requests)
{
    std::vector<TerrainSampleResult> results(
        requests.size());

    std::size_t taskCount = 0;

    for (std::size_t requestIndex = 0;
         requestIndex < requests.size();
         ++requestIndex)
    {
        const TerrainSampleRequest& request =
            requests[requestIndex];

        ValidateRequest(request);

        TerrainSampleResult& result =
            results[requestIndex];

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

        taskCount +=
            request.regions.size();
    }

    if (taskCount == 0)
    {
        return results;
    }

    if (taskCount == 1)
    {
        for (std::size_t requestIndex = 0;
             requestIndex < requests.size();
             ++requestIndex)
        {
            const TerrainSampleRequest& request =
                requests[requestIndex];

            TerrainSampleResult& result =
                results[requestIndex];

            for (std::size_t regionIndex = 0;
                 regionIndex <
                    request.regions.size();
                 ++regionIndex)
            {
                result.patches[regionIndex] =
                    GeneratePatch(
                        request,
                        request.regions[
                            regionIndex]);
            }
        }

        return results;
    }

    jobs::JobGroup group;

    for (std::size_t requestIndex = 0;
         requestIndex < requests.size();
         ++requestIndex)
    {
        const TerrainSampleRequest* request =
            &requests[requestIndex];

        TerrainSampleResult* result =
            &results[requestIndex];

        for (std::size_t regionIndex = 0;
             regionIndex <
                request->regions.size();
             ++regionIndex)
        {
            jobSystem_.Submit(
                group,
                [this,
                 request,
                 result,
                 regionIndex]
                {
                    result->patches[regionIndex] =
                        GeneratePatch(
                            *request,
                            request->regions[
                                regionIndex]);
                });
        }
    }

    jobSystem_.Wait(group);
    return results;
}

TerrainSamplePatch
TerrainSampleStreamer::GeneratePatch(
    const TerrainSampleRequest& request,
    const PhysicalRegion& region) const
{
    TerrainSamplePatch patch{};
    patch.region = region;

    patch.elevations.resize(
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

            const f32 elevation =
                static_cast<f32>(
                    terrainSource_.Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            request.
                                footprintMeters
                    }).elevationMeters);

            patch.elevations[
                static_cast<std::size_t>(
                    localY) *
                    region.width +
                localX] =
                    elevation;
        }
    }

    return patch;
}
} // namespace orbit::terrain_stream
