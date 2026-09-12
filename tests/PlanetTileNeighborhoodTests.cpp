#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <array>
#include <iostream>
#include <vector>

namespace
{
using orbit::u32;
using orbit::world::CubeFace;
using orbit::world::PlanetTileId;

[[nodiscard]] bool ContainsFace(
    const std::vector<PlanetTileId>& tiles,
    const CubeFace face)
{
    for (const PlanetTileId& tile : tiles)
    {
        if (tile.face == face)
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool HasDuplicates(
    const std::vector<PlanetTileId>& tiles)
{
    for (std::size_t first = 0;
         first < tiles.size();
         ++first)
    {
        for (std::size_t second = first + 1;
             second < tiles.size();
             ++second)
        {
            if (tiles[first] == tiles[second])
            {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]] bool VerifyLevel(
    const std::vector<PlanetTileId>& tiles,
    const orbit::u8 level)
{
    for (const PlanetTileId& tile : tiles)
    {
        if (tile.level != level)
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool VerifyCrossFaceNeighborhood(
    const PlanetTileId tile)
{
    const std::vector<PlanetTileId> neighbors =
        orbit::world::TileNeighborhood(tile, 1U);

    return
        VerifyLevel(neighbors, tile.level) &&
        !HasDuplicates(neighbors) &&
        neighbors.size() >= 6U &&
        [&]()
        {
            for (const PlanetTileId& neighbor : neighbors)
            {
                if (neighbor.face != tile.face)
                {
                    return true;
                }
            }

            return false;
        }();
}
} // namespace

int main()
{
    constexpr orbit::u8 level = 3;
    constexpr u32 tileCount = 1U << level;
    constexpr u32 last = tileCount - 1U;
    constexpr u32 middle = tileCount / 2U - 1U;

    const PlanetTileId interior{
        .face = CubeFace::PositiveX,
        .level = level,
        .x = middle,
        .y = middle
    };

    const std::vector<PlanetTileId> interiorNeighborhood =
        orbit::world::TileNeighborhood(interior, 1U);

    if (interiorNeighborhood.size() != 9U ||
        HasDuplicates(interiorNeighborhood) ||
        !VerifyLevel(interiorNeighborhood, level))
    {
        std::cerr << "Interior 3x3 neighborhood failed.\n";
        return 1;
    }

    for (const PlanetTileId& tile : interiorNeighborhood)
    {
        if (tile.face != interior.face)
        {
            std::cerr << "Interior neighborhood crossed a cube face.\n";
            return 1;
        }
    }

    if (orbit::world::OffsetTile(interior, 0, 0) != interior)
    {
        std::cerr << "Zero tile offset did not preserve the tile.\n";
        return 1;
    }

    const PlanetTileId positiveXLeft{
        .face = CubeFace::PositiveX,
        .level = level,
        .x = 0U,
        .y = middle
    };

    const PlanetTileId positiveXRight{
        .face = CubeFace::PositiveX,
        .level = level,
        .x = last,
        .y = middle
    };

    if (orbit::world::OffsetTile(
            positiveXLeft,
            -1,
            0).face != CubeFace::PositiveZ)
    {
        std::cerr << "+X to +Z seam traversal failed.\n";
        return 1;
    }

    if (orbit::world::OffsetTile(
            positiveXRight,
            1,
            0).face != CubeFace::NegativeZ)
    {
        std::cerr << "+X to -Z seam traversal failed.\n";
        return 1;
    }

    constexpr std::array<CubeFace, 6> faces{
        CubeFace::PositiveX,
        CubeFace::NegativeX,
        CubeFace::PositiveY,
        CubeFace::NegativeY,
        CubeFace::PositiveZ,
        CubeFace::NegativeZ
    };

    for (const CubeFace face : faces)
    {
        const std::array<PlanetTileId, 4> edgeTiles{
            PlanetTileId{face, level, 0U, middle},
            PlanetTileId{face, level, last, middle},
            PlanetTileId{face, level, middle, 0U},
            PlanetTileId{face, level, middle, last}
        };

        for (const PlanetTileId edgeTile : edgeTiles)
        {
            if (!VerifyCrossFaceNeighborhood(edgeTile))
            {
                std::cerr << "Cube-face edge neighborhood failed.\n";
                return 1;
            }
        }

        const std::array<PlanetTileId, 4> cornerTiles{
            PlanetTileId{face, level, 0U, 0U},
            PlanetTileId{face, level, 0U, last},
            PlanetTileId{face, level, last, 0U},
            PlanetTileId{face, level, last, last}
        };

        for (const PlanetTileId cornerTile : cornerTiles)
        {
            const std::vector<PlanetTileId> neighborhood =
                orbit::world::TileNeighborhood(
                    cornerTile,
                    1U);

            if (HasDuplicates(neighborhood) ||
                !VerifyLevel(neighborhood, level))
            {
                std::cerr << "Cube-face corner neighborhood failed.\n";
                return 1;
            }
        }
    }

    const PlanetTileId northPolarEdge{
        .face = CubeFace::PositiveY,
        .level = level,
        .x = middle,
        .y = last
    };

    const PlanetTileId southPolarEdge{
        .face = CubeFace::NegativeY,
        .level = level,
        .x = middle,
        .y = 0U
    };

    if (orbit::world::OffsetTile(
            northPolarEdge,
            0,
            1).face != CubeFace::NegativeZ ||
        orbit::world::OffsetTile(
            southPolarEdge,
            0,
            -1).face != CubeFace::NegativeZ)
    {
        std::cerr << "Polar face seam traversal failed.\n";
        return 1;
    }

    const std::vector<PlanetTileId> cornerNeighborhood =
        orbit::world::TileNeighborhood(
            PlanetTileId{
                CubeFace::PositiveX,
                level,
                last,
                last},
            1U);

    if (!ContainsFace(
            cornerNeighborhood,
            CubeFace::PositiveX) ||
        !ContainsFace(
            cornerNeighborhood,
            CubeFace::PositiveY) ||
        !ContainsFace(
            cornerNeighborhood,
            CubeFace::NegativeZ) ||
        HasDuplicates(cornerNeighborhood))
    {
        std::cerr << "Three-face corner traversal failed.\n";
        return 1;
    }

    const std::vector<PlanetTileId> zeroRadius =
        orbit::world::TileNeighborhood(
            interior,
            0U);

    if (zeroRadius.size() != 1U ||
        zeroRadius.front() != interior)
    {
        std::cerr << "Zero-radius neighborhood failed.\n";
        return 1;
    }

    return 0;
}
