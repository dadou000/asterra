#include <orbit/water_render/OceanMesh.hpp>

#include <algorithm>
#include <iostream>

int main()
{
    const orbit::water_render::OceanMeshConfig config{
        .radialRings = 4,
        .angularSegments = 8
    };

    const orbit::u32 vertexCount =
        orbit::water_render::OceanVertexCount(
            config);

    const orbit::u32 indexCount =
        orbit::water_render::OceanIndexCount(
            config);

    if (vertexCount != 33)
    {
        std::cerr
            << "Ocean mesh vertex count is incorrect.\n";
        return 1;
    }

    if (indexCount !=
        8U * 3U +
        3U * 8U * 6U)
    {
        std::cerr
            << "Ocean mesh index count is incorrect.\n";
        return 1;
    }

    const auto indices =
        orbit::water_render::BuildOceanIndices(
            config);

    if (indices.size() !=
        indexCount)
    {
        std::cerr
            << "Ocean mesh index vector size is incorrect.\n";
        return 1;
    }

    if (indices.empty() ||
        *std::max_element(
            indices.begin(),
            indices.end()) >=
            vertexCount)
    {
        std::cerr
            << "Ocean mesh contains an out-of-range vertex index.\n";
        return 1;
    }

    // The center fan must close from the last angular sector back to
    // the first sector of ring zero.
    const std::size_t lastFan =
        static_cast<std::size_t>(
            config.angularSegments - 1U) *
        3U;

    if (indices[lastFan + 0U] != 0U ||
        indices[lastFan + 1U] != 8U ||
        indices[lastFan + 2U] != 1U)
    {
        std::cerr
            << "Ocean mesh center fan does not close its angular seam.\n";
        return 1;
    }

    // Last quad on the outer annulus must also wrap to angular segment 0.
    const std::size_t lastQuad =
        indices.size() -
        6U;

    const orbit::u32 innerLast =
        1U +
        2U * 8U +
        7U;

    const orbit::u32 innerZero =
        1U +
        2U * 8U;

    const orbit::u32 outerLast =
        1U +
        3U * 8U +
        7U;

    const orbit::u32 outerZero =
        1U +
        3U * 8U;

    if (indices[lastQuad + 0U] !=
            innerLast ||
        indices[lastQuad + 1U] !=
            outerLast ||
        indices[lastQuad + 2U] !=
            innerZero ||
        indices[lastQuad + 3U] !=
            innerZero ||
        indices[lastQuad + 4U] !=
            outerLast ||
        indices[lastQuad + 5U] !=
            outerZero)
    {
        std::cerr
            << "Ocean mesh outer annulus does not close its angular seam.\n";
        return 1;
    }

    return 0;
}
