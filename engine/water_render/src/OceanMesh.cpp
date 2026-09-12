#include <orbit/water_render/OceanMesh.hpp>

#include <limits>
#include <stdexcept>

namespace orbit::water_render
{
namespace
{
void Validate(
    const OceanMeshConfig& config)
{
    if (config.radialRings < 2 ||
        config.angularSegments < 3)
    {
        throw std::invalid_argument(
            "Orbit ocean mesh requires at least two radial rings and three angular segments.");
    }

    const u64 vertexCount =
        1ULL +
        static_cast<u64>(
            config.radialRings) *
        config.angularSegments;

    if (vertexCount >
        static_cast<u64>(
            std::numeric_limits<u32>::max()))
    {
        throw std::overflow_error(
            "Orbit ocean mesh vertex count exceeds 32-bit indexing.");
    }
}

[[nodiscard]] u32 RingVertex(
    const OceanMeshConfig& config,
    const u32 ring,
    const u32 segment) noexcept
{
    return
        1U +
        ring *
            config.angularSegments +
        segment;
}
} // namespace

u32 OceanVertexCount(
    const OceanMeshConfig& config)
{
    Validate(config);

    return
        1U +
        config.radialRings *
            config.angularSegments;
}

u32 OceanIndexCount(
    const OceanMeshConfig& config)
{
    Validate(config);

    const u64 count =
        static_cast<u64>(
            config.angularSegments) *
        3ULL +
        static_cast<u64>(
            config.radialRings - 1U) *
        config.angularSegments *
        6ULL;

    if (count >
        static_cast<u64>(
            std::numeric_limits<u32>::max()))
    {
        throw std::overflow_error(
            "Orbit ocean mesh index count exceeds 32-bit draw limits.");
    }

    return static_cast<u32>(count);
}

std::vector<u32> BuildOceanIndices(
    const OceanMeshConfig& config)
{
    Validate(config);

    std::vector<u32> indices;
    indices.reserve(
        OceanIndexCount(config));

    // Center fan.
    for (u32 segment = 0;
         segment <
            config.angularSegments;
         ++segment)
    {
        const u32 next =
            (segment + 1U) %
            config.angularSegments;

        indices.push_back(0U);
        indices.push_back(
            RingVertex(
                config,
                0U,
                segment));
        indices.push_back(
            RingVertex(
                config,
                0U,
                next));
    }

    // Crack-free annuli. Every ring uses the same angular segmentation,
    // so the wrap seam is topologically identical to all other sectors.
    for (u32 ring = 0;
         ring + 1U <
            config.radialRings;
         ++ring)
    {
        for (u32 segment = 0;
             segment <
                config.angularSegments;
             ++segment)
        {
            const u32 next =
                (segment + 1U) %
                config.angularSegments;

            const u32 innerA =
                RingVertex(
                    config,
                    ring,
                    segment);

            const u32 innerB =
                RingVertex(
                    config,
                    ring,
                    next);

            const u32 outerA =
                RingVertex(
                    config,
                    ring + 1U,
                    segment);

            const u32 outerB =
                RingVertex(
                    config,
                    ring + 1U,
                    next);

            indices.push_back(innerA);
            indices.push_back(outerA);
            indices.push_back(innerB);

            indices.push_back(innerB);
            indices.push_back(outerA);
            indices.push_back(outerB);
        }
    }

    return indices;
}
} // namespace orbit::water_render
