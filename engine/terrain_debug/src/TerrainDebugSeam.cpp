#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_region/SurfaceBoundaryExchange.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

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

            const auto transformed =
                terrain_region::
                    TransformBoundaryVectorAcrossEdge(
                        edge,
                        mapping,
                        {
                            static_cast<f64>(
                                pageView.vector[a].x),
                            static_cast<f64>(
                                pageView.vector[a].y)
                        });

            const f64 difference =
                VectorDifference(
                    {
                        static_cast<f32>(
                            transformed.x),
                        static_cast<f32>(
                            transformed.y)
                    },
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

std::string_view TerrainDebugSeamStateName(
    const TerrainDebugSeamState state) noexcept
{
    switch (state)
    {
    case TerrainDebugSeamState::Continuous:
        return "continuous";
    case TerrainDebugSeamState::MissingNeighbor:
        return "missing neighbor";
    case TerrainDebugSeamState::PhysicalLodMismatch:
        return "physical LOD mismatch";
    case TerrainDebugSeamState::RevisionMismatch:
        return "revision mismatch";
    case TerrainDebugSeamState::FieldUnavailable:
        return "field unavailable";
    case TerrainDebugSeamState::ValueMismatch:
        return "value mismatch";
    }

    return "unknown";
}

std::array<TerrainDebugSeamInspection, 4>
InspectTerrainDebugSeams(
    const TerrainDebugPageData& page,
    const TerrainDebugField field,
    const TerrainDebugLivePages& livePages,
    const f64 numericTolerance)
{
    if (!std::isfinite(numericTolerance) ||
        numericTolerance < 0.0)
    {
        throw std::invalid_argument(
            "Terrain debug seam inspection tolerance must be finite and non-negative.");
    }

    std::array<TerrainDebugSeamInspection, 4> result{};

    const auto edgeLength =
        [](const TerrainDebugPageData& data,
           const world::TileEdge edge) noexcept
        {
            return
                edge == world::TileEdge::North ||
                edge == world::TileEdge::South
                    ? data.Width()
                    : data.Height();
        };

    for (u8 raw = 0U; raw < 4U; ++raw)
    {
        const auto edge =
            static_cast<world::TileEdge>(raw);

        auto& inspection =
            result[raw];
        inspection.edge = edge;

        const auto expected =
            ExpectedNeighbor(
                page.Stamp().address,
                edge);
        const auto neighbor =
            livePages.Find(expected);

        inspection.comparison.provenance =
            ProbeSeam(
                page.Stamp(),
                edge,
                neighbor != nullptr
                    ? &neighbor->Stamp()
                    : nullptr);

        const auto& probe =
            inspection.comparison.provenance;

        if (!probe.neighborPresent)
        {
            inspection.state =
                TerrainDebugSeamState::MissingNeighbor;
            continue;
        }

        if (!probe.matchingPhysicalLod)
        {
            inspection.state =
                TerrainDebugSeamState::PhysicalLodMismatch;
            continue;
        }

        if (!probe.matchingRevisions)
        {
            inspection.state =
                TerrainDebugSeamState::RevisionMismatch;
            continue;
        }

        if (!page.Has(field) ||
            neighbor == nullptr ||
            !neighbor->Has(field))
        {
            inspection.state =
                TerrainDebugSeamState::FieldUnavailable;
            continue;
        }

        const auto mapping =
            world::NeighborAcrossTileEdge(
                page.Stamp().address.tile,
                edge);

        if (edgeLength(page, edge) !=
            edgeLength(*neighbor, mapping.edge))
        {
            inspection.state =
                TerrainDebugSeamState::ValueMismatch;
            inspection.comparison.samplesCompared = 0U;
            inspection.comparison.mismatchedSamples = 1U;
            inspection.comparison.maximumDifference =
                std::numeric_limits<f64>::infinity();
            continue;
        }

        const auto pageView =
            page.View(field);
        const auto neighborView =
            neighbor->View(field);

        inspection.comparison =
            CompareSeamValues(
                page.Stamp(),
                pageView,
                edge,
                &neighbor->Stamp(),
                &neighborView,
                numericTolerance);

        inspection.state =
            inspection.comparison.ValuesContinuous()
                ? TerrainDebugSeamState::Continuous
                : TerrainDebugSeamState::ValueMismatch;
    }

    return result;
}

u64 TerrainDebugSeamOverlayFingerprint(
    const std::span<const TerrainDebugSeamInspection> seams) noexcept
{
    u64 value =
        0x4D32395345414D4FULL; // "M29SEAMO"

    for (const auto& seam : seams)
    {
        value =
            terrain::StableCombine64(
                value,
                static_cast<u64>(seam.edge));
        value =
            terrain::StableCombine64(
                value,
                static_cast<u64>(seam.state));
        value =
            terrain::StableCombine64(
                value,
                seam.comparison.samplesCompared);
        value =
            terrain::StableCombine64(
                value,
                seam.comparison.mismatchedSamples);
    }

    return value;
}

void ApplyTerrainDebugSeamOverlayRgba8(
    std::vector<u8>& rgba,
    const u32 width,
    const u32 height,
    const std::span<const TerrainDebugSeamInspection> seams,
    const u32 thickness)
{
    const u64 expectedBytes =
        static_cast<u64>(width) *
        static_cast<u64>(height) *
        4ULL;

    if (width == 0U ||
        height == 0U ||
        rgba.size() != expectedBytes)
    {
        throw std::invalid_argument(
            "Terrain debug seam overlay requires a complete RGBA8 image.");
    }

    if (seams.empty() ||
        thickness == 0U)
    {
        return;
    }

    const u32 border =
        std::min({
            thickness,
            width,
            height
        });

    const auto color =
        [](const TerrainDebugSeamState state)
            -> std::array<u8, 4>
        {
            switch (state)
            {
            case TerrainDebugSeamState::Continuous:
                return {48U, 208U, 96U, 255U};
            case TerrainDebugSeamState::MissingNeighbor:
                return {96U, 96U, 96U, 255U};
            case TerrainDebugSeamState::PhysicalLodMismatch:
                return {64U, 160U, 255U, 255U};
            case TerrainDebugSeamState::RevisionMismatch:
                return {255U, 196U, 48U, 255U};
            case TerrainDebugSeamState::FieldUnavailable:
                return {224U, 64U, 224U, 255U};
            case TerrainDebugSeamState::ValueMismatch:
                return {255U, 64U, 64U, 255U};
            }

            return {255U, 255U, 255U, 255U};
        };

    const auto write =
        [&](const u32 x,
            const u32 y,
            const std::array<u8, 4>& value)
        {
            const std::size_t index =
                (static_cast<std::size_t>(y) *
                     width +
                 x) *
                4U;

            rgba[index + 0U] = value[0];
            rgba[index + 1U] = value[1];
            rgba[index + 2U] = value[2];
            rgba[index + 3U] = value[3];
        };

    for (const auto& seam : seams)
    {
        const auto value =
            color(seam.state);

        switch (seam.edge)
        {
        case world::TileEdge::North:
            for (u32 y = 0U; y < border; ++y)
            {
                for (u32 x = 0U; x < width; ++x)
                {
                    write(x, y, value);
                }
            }
            break;

        case world::TileEdge::East:
            for (u32 x = width - border; x < width; ++x)
            {
                for (u32 y = 0U; y < height; ++y)
                {
                    write(x, y, value);
                }
            }
            break;

        case world::TileEdge::South:
            for (u32 y = height - border; y < height; ++y)
            {
                for (u32 x = 0U; x < width; ++x)
                {
                    write(x, y, value);
                }
            }
            break;

        case world::TileEdge::West:
            for (u32 x = 0U; x < border; ++x)
            {
                for (u32 y = 0U; y < height; ++y)
                {
                    write(x, y, value);
                }
            }
            break;
        }
    }
}

} // namespace orbit::terrain_debug
