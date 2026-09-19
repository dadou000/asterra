#include <orbit/terrain_debug/TerrainDebugSeam.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace orbit::terrain_debug
{
namespace
{
[[nodiscard]] std::size_t EdgeIndex(
    const u32 width,
    const u32 height,
    const world::TileEdge edge,
    const u32 sample)
{
    switch (edge)
    {
    case world::TileEdge::North:
        return sample;
    case world::TileEdge::East:
        return
            static_cast<std::size_t>(sample) *
                width +
            (width - 1U);
    case world::TileEdge::South:
        return
            static_cast<std::size_t>(height - 1U) *
                width +
            sample;
    case world::TileEdge::West:
        return
            static_cast<std::size_t>(sample) *
            width;
    }

    return 0;
}

[[nodiscard]] u32 EdgeLength(
    const TerrainDebugRasterView& view,
    const world::TileEdge edge) noexcept
{
    return
        edge == world::TileEdge::North ||
        edge == world::TileEdge::South
            ? view.width
            : view.height;
}

void RequireCompatibleViews(
    const TerrainDebugRasterView& page,
    const TerrainDebugRasterView& neighbor,
    const world::TileEdge sourceEdge,
    const world::TileEdge receivingEdge)
{
    if (page.field != neighbor.field)
    {
        throw std::invalid_argument(
            "Terrain debug seam views must use the same field.");
    }

    if (page.width == 0U ||
        page.height == 0U ||
        neighbor.width == 0U ||
        neighbor.height == 0U)
    {
        throw std::invalid_argument(
            "Terrain debug seam views require non-zero dimensions.");
    }

    if (EdgeLength(page, sourceEdge) !=
        EdgeLength(neighbor, receivingEdge))
    {
        throw std::invalid_argument(
            "Terrain debug seam edge sample counts do not match.");
    }
}

[[nodiscard]] f64 ScalarDifference(
    const f32 a,
    const f32 b) noexcept
{
    if (!std::isfinite(a) ||
        !std::isfinite(b))
    {
        return a == b
            ? 0.0
            : std::numeric_limits<f64>::infinity();
    }

    return std::abs(
        static_cast<f64>(a) -
        static_cast<f64>(b));
}

[[nodiscard]] f64 VectorDifference(
    const TerrainDebugVector2 a,
    const TerrainDebugVector2 b) noexcept
{
    if (!std::isfinite(a.x) ||
        !std::isfinite(a.y) ||
        !std::isfinite(b.x) ||
        !std::isfinite(b.y))
    {
        return
            a.x == b.x &&
            a.y == b.y
                ? 0.0
                : std::numeric_limits<f64>::infinity();
    }

    const f64 dx =
        static_cast<f64>(a.x) -
        static_cast<f64>(b.x);
    const f64 dy =
        static_cast<f64>(a.y) -
        static_cast<f64>(b.y);

    return std::sqrt(dx * dx + dy * dy);
}
} // namespace

bool TerrainDebugSeamComparison::Comparable() const noexcept
{
    return provenance.IsContinuousCandidate() &&
        samplesCompared > 0U;
}

bool TerrainDebugSeamComparison::ValuesContinuous() const noexcept
{
    return Comparable() &&
        mismatchedSamples == 0U;
}

TerrainDebugSeamComparison CompareSeamValues(
    const TerrainDebugPageStamp& page,
    const TerrainDebugRasterView& pageView,
    const world::TileEdge edge,
    const TerrainDebugPageStamp* const neighbor,
    const TerrainDebugRasterView* const neighborView,
    const f64 numericTolerance)
{
    if (!std::isfinite(numericTolerance) ||
        numericTolerance < 0.0)
    {
        throw std::invalid_argument(
            "Terrain debug seam tolerance must be finite and non-negative.");
    }

    TerrainDebugSeamComparison result{
        .provenance =
            ProbeSeam(
                page,
                edge,
                neighbor)
    };

    if (!result.provenance.IsContinuousCandidate() ||
        neighborView == nullptr)
    {
        return result;
    }

    const auto mapping =
        world::NeighborAcrossTileEdge(
            page.address.tile,
            edge);

    RequireCompatibleViews(
        pageView,
        *neighborView,
        edge,
        mapping.edge);

    const auto& descriptor =
        Descriptor(pageView.field);
    const u32 samples =
        EdgeLength(pageView, edge);

    result.samplesCompared = samples;

    for (u32 i = 0; i < samples; ++i)
    {
        const u32 neighborSample =
            world::RemapTileEdgeSampleIndex(
                mapping,
                i,
                samples);

        const std::size_t a =
            EdgeIndex(
                pageView.width,
                pageView.height,
                edge,
                i);
        const std::size_t b =
            EdgeIndex(
                neighborView->width,
                neighborView->height,
                mapping.edge,
                neighborSample);

        bool mismatch = false;

        switch (descriptor.valueClass)
        {
        case TerrainDebugValueClass::Scalar:
        case TerrainDebugValueClass::SignedScalar:
        {
            if (a >= pageView.scalar.size() ||
                b >= neighborView->scalar.size())
            {
                throw std::invalid_argument(
                    "Terrain debug scalar seam source is incomplete.");
            }

            const f64 difference =
                ScalarDifference(
                    pageView.scalar[a],
                    neighborView->scalar[b]);
            result.maximumDifference =
                std::max(
                    result.maximumDifference,
                    difference);
            mismatch =
                difference > numericTolerance;
            break;
        }

        case TerrainDebugValueClass::Vector:
        {
            if (a >= pageView.vector.size() ||
                b >= neighborView->vector.size())
            {
                throw std::invalid_argument(
                    "Terrain debug vector seam source is incomplete.");
            }

            const f64 difference =
                VectorDifference(
                    pageView.vector[a],
                    neighborView->vector[b]);
            result.maximumDifference =
                std::max(
                    result.maximumDifference,
                    difference);
            mismatch =
                difference > numericTolerance;
            break;
        }

        case TerrainDebugValueClass::Category:
            if (a >= pageView.category.size() ||
                b >= neighborView->category.size())
            {
                throw std::invalid_argument(
                    "Terrain debug category seam source is incomplete.");
            }
            mismatch =
                pageView.category[a] !=
                neighborView->category[b];
            break;

        case TerrainDebugValueClass::Boolean:
            if (a >= pageView.boolean.size() ||
                b >= neighborView->boolean.size())
            {
                throw std::invalid_argument(
                    "Terrain debug boolean seam source is incomplete.");
            }
            mismatch =
                pageView.boolean[a] !=
                neighborView->boolean[b];
            break;

        case TerrainDebugValueClass::Revision:
            if (a >= pageView.revision.size() ||
                b >= neighborView->revision.size())
            {
                throw std::invalid_argument(
                    "Terrain debug revision seam source is incomplete.");
            }
            mismatch =
                pageView.revision[a] !=
                neighborView->revision[b];
            break;

        case TerrainDebugValueClass::Lod:
            if (a >= pageView.lod.size() ||
                b >= neighborView->lod.size())
            {
                throw std::invalid_argument(
                    "Terrain debug LOD seam source is incomplete.");
            }
            mismatch =
                pageView.lod[a] !=
                neighborView->lod[b];
            break;
        }

        if (mismatch)
        {
            ++result.mismatchedSamples;
        }
    }

    return result;
}
} // namespace orbit::terrain_debug
