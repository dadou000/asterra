#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>

#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kSurfaceLiftMeters = 12.0;

[[nodiscard]] math::Float4 StateColor(
    const studio_session::TerrainRebuildState state) noexcept
{
    using State =
        studio_session::TerrainRebuildState;

    switch (state)
    {
    case State::Clean:
        return {0.45F, 0.45F, 0.45F, 0.90F};
    case State::Dirty:
        return {1.00F, 0.42F, 0.10F, 1.00F};
    case State::Queued:
        return {1.00F, 0.82F, 0.12F, 1.00F};
    case State::BuildingCpu:
        return {0.25F, 0.55F, 1.00F, 1.00F};
    case State::BuildingGpu:
        return {0.05F, 0.90F, 1.00F, 1.00F};
    case State::Uploading:
        return {0.72F, 0.35F, 1.00F, 1.00F};
    case State::Ready:
        return {0.25F, 0.95F, 0.38F, 0.90F};
    case State::Failed:
        return {1.00F, 0.08F, 0.08F, 1.00F};
    case State::StaleReplaced:
        return {1.00F, 0.15F, 0.78F, 1.00F};
    }

    return {1.0F, 1.0F, 1.0F, 1.0F};
}

[[nodiscard]] bool IsDirtyLike(
    const studio_session::TerrainRebuildState state) noexcept
{
    using State =
        studio_session::TerrainRebuildState;

    return
        state == State::Dirty ||
        state == State::Queued ||
        state == State::BuildingCpu ||
        state == State::BuildingGpu ||
        state == State::Uploading ||
        state == State::Failed ||
        state == State::StaleReplaced;
}

[[nodiscard]] math::Double3 GridDirection(
    const terrain::PhysicalTerrainPageAddress& address,
    const f64 gridX,
    const f64 gridY,
    const u32 width,
    const u32 height) noexcept
{
    const auto bounds =
        world::TileBounds(
            address.tile);

    const f64 denominatorX =
        static_cast<f64>(
            std::max(width - 1U, 1U));

    const f64 denominatorY =
        static_cast<f64>(
            std::max(height - 1U, 1U));

    const f64 u =
        bounds.minimumUv.x +
        (bounds.maximumUv.x -
         bounds.minimumUv.x) *
            (gridX / denominatorX);

    const f64 v =
        bounds.minimumUv.y +
        (bounds.maximumUv.y -
         bounds.minimumUv.y) *
            (gridY / denominatorY);

    return world::CubeToUnitDirection({
        .face = address.tile.face,
        .uv = {u, v}
    });
}

[[nodiscard]] math::Double3 TileDirection(
    const world::PlanetTileId& tile,
    const f64 x,
    const f64 y) noexcept
{
    const auto bounds =
        world::TileBounds(tile);

    return world::CubeToUnitDirection({
        .face = tile.face,
        .uv = {
            bounds.minimumUv.x +
                (bounds.maximumUv.x -
                 bounds.minimumUv.x) * x,
            bounds.minimumUv.y +
                (bounds.maximumUv.y -
                 bounds.minimumUv.y) * y
        }
    });
}

[[nodiscard]] std::optional<f64>
PhysicalElevation(
    const studio_session::StudioTerrainPhysicalPageSnapshot* page,
    const f64 gridX,
    const f64 gridY,
    const u32 width,
    const u32 height)
{
    if (page == nullptr ||
        page->material == nullptr ||
        width < 2U ||
        height < 2U ||
        page->material->Resolution() != width ||
        page->material->Resolution() != height)
    {
        return std::nullopt;
    }

    const f64 x =
        std::clamp(
            gridX,
            0.0,
            static_cast<f64>(width - 1U));

    const f64 y =
        std::clamp(
            gridY,
            0.0,
            static_cast<f64>(height - 1U));

    const u32 x0 =
        static_cast<u32>(std::floor(x));
    const u32 y0 =
        static_cast<u32>(std::floor(y));
    const u32 x1 =
        std::min(x0 + 1U, width - 1U);
    const u32 y1 =
        std::min(y0 + 1U, height - 1U);

    const f64 tx =
        x - static_cast<f64>(x0);
    const f64 ty =
        y - static_cast<f64>(y0);

    const f64 a =
        std::lerp(
            static_cast<f64>(
                page->material->At(
                    x0,
                    y0).
                    SurfaceHeightMeters()),
            static_cast<f64>(
                page->material->At(
                    x1,
                    y0).
                    SurfaceHeightMeters()),
            tx);

    const f64 b =
        std::lerp(
            static_cast<f64>(
                page->material->At(
                    x0,
                    y1).
                    SurfaceHeightMeters()),
            static_cast<f64>(
                page->material->At(
                    x1,
                    y1).
                    SurfaceHeightMeters()),
            tx);

    return std::lerp(a, b, ty);
}

[[nodiscard]] math::Float3 CameraRelativeSurfacePoint(
    const math::Double3& direction,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera,
    const std::optional<f64> physicalElevation = std::nullopt)
{
    const f64 elevation =
        physicalElevation.value_or(
            source.Sample({
                .unitDirection = direction,
                .footprintMeters = 1.0,
                .planet = runtime.planet.id
            }).elevationMeters);

    const math::Double3 relative =
        direction *
            (runtime.planet.radiusMeters +
             elevation +
             kSurfaceLiftMeters) -
        camera.localPositionMeters;

    return {
        static_cast<f32>(relative.x),
        static_cast<f32>(relative.y),
        static_cast<f32>(relative.z)
    };
}

void AppendDirectionLine(
    std::vector<editor_ui::PreviewLine>& result,
    const math::Double3& a,
    const math::Double3& b,
    const math::Float4& color,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    result.push_back({
        .start =
            CameraRelativeSurfacePoint(
                a,
                runtime,
                source,
                camera),
        .end =
            CameraRelativeSurfacePoint(
                b,
                runtime,
                source,
                camera),
        .color = color
    });
}

void AppendGridLine(
    std::vector<editor_ui::PreviewLine>& result,
    const StudioTerrainDiagnosticPage& page,
    const f64 ax,
    const f64 ay,
    const f64 bx,
    const f64 by,
    const u32 width,
    const u32 height,
    const math::Float4& color,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    const auto a =
        GridDirection(
            page.status.address,
            ax,
            ay,
            width,
            height);

    const auto b =
        GridDirection(
            page.status.address,
            bx,
            by,
            width,
            height);

    result.push_back({
        .start =
            CameraRelativeSurfacePoint(
                a,
                runtime,
                source,
                camera,
                PhysicalElevation(
                    page.snapshot.get(),
                    ax,
                    ay,
                    width,
                    height)),
        .end =
            CameraRelativeSurfacePoint(
                b,
                runtime,
                source,
                camera,
                PhysicalElevation(
                    page.snapshot.get(),
                    bx,
                    by,
                    width,
                    height)),
        .color = color
    });
}

void AppendTileBounds(
    std::vector<editor_ui::PreviewLine>& result,
    const world::PlanetTileId& tile,
    const math::Float4& color,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    constexpr u32 Segments = 12U;

    const auto appendEdge =
        [&](const f64 x0,
            const f64 y0,
            const f64 x1,
            const f64 y1)
        {
            math::Double3 previous =
                TileDirection(
                    tile,
                    x0,
                    y0);

            for (u32 index = 1U;
                 index <= Segments;
                 ++index)
            {
                const f64 t =
                    static_cast<f64>(index) /
                    static_cast<f64>(Segments);

                const math::Double3 current =
                    TileDirection(
                        tile,
                        std::lerp(x0, x1, t),
                        std::lerp(y0, y1, t));

                AppendDirectionLine(
                    result,
                    previous,
                    current,
                    color,
                    runtime,
                    source,
                    camera);

                previous = current;
            }
        };

    appendEdge(0.0, 0.0, 1.0, 0.0);
    appendEdge(1.0, 0.0, 1.0, 1.0);
    appendEdge(1.0, 1.0, 0.0, 1.0);
    appendEdge(0.0, 1.0, 0.0, 0.0);
}

void AppendOffsetSquare(
    std::vector<editor_ui::PreviewLine>& result,
    const world::SurfaceFrame& frame,
    const math::Double2& center,
    const f64 halfExtent,
    const math::Float4& color,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    if (!std::isfinite(halfExtent) ||
        halfExtent <= 0.0)
    {
        return;
    }

    constexpr u32 Segments = 16U;

    const auto direction =
        [&](const f64 x,
            const f64 y)
        {
            return world::DirectionAtSurfaceOffset(
                runtime.planet,
                frame,
                {
                    center.x + x,
                    center.y + y
                });
        };

    const auto appendEdge =
        [&](const f64 x0,
            const f64 y0,
            const f64 x1,
            const f64 y1)
        {
            auto previous =
                direction(x0, y0);

            for (u32 index = 1U;
                 index <= Segments;
                 ++index)
            {
                const f64 t =
                    static_cast<f64>(index) /
                    static_cast<f64>(Segments);

                const auto current =
                    direction(
                        std::lerp(x0, x1, t),
                        std::lerp(y0, y1, t));

                AppendDirectionLine(
                    result,
                    previous,
                    current,
                    color,
                    runtime,
                    source,
                    camera);

                previous = current;
            }
        };

    appendEdge(
        -halfExtent,
        -halfExtent,
         halfExtent,
        -halfExtent);
    appendEdge(
         halfExtent,
        -halfExtent,
         halfExtent,
         halfExtent);
    appendEdge(
         halfExtent,
         halfExtent,
        -halfExtent,
         halfExtent);
    appendEdge(
        -halfExtent,
         halfExtent,
        -halfExtent,
        -halfExtent);
}

void AppendClipmapRings(
    std::vector<editor_ui::PreviewLine>& result,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    const std::size_t count =
        std::min(
            runtime.layout.levels.size(),
            runtime.motion.levels.size());

    for (std::size_t index = 0U;
         index < count;
         ++index)
    {
        const auto& level =
            runtime.layout.levels[index];

        const auto& motion =
            runtime.motion.levels[index];

        const f32 t =
            count > 1U
                ? static_cast<f32>(index) /
                    static_cast<f32>(count - 1U)
                : 0.0F;

        const math::Float4 color{
            0.10F + 0.65F * t,
            0.82F - 0.45F * t,
            1.00F,
            0.80F
        };

        AppendOffsetSquare(
            result,
            motion.surfaceFrame,
            motion.centerOffsetMeters,
            level.outerHalfExtentMeters,
            color,
            runtime,
            source,
            camera);

        if (level.innerHoleHalfExtentMeters >
            0.0)
        {
            AppendOffsetSquare(
                result,
                motion.surfaceFrame,
                motion.centerOffsetMeters,
                level.innerHoleHalfExtentMeters,
                {
                    color.x,
                    color.y,
                    color.z,
                    0.45F
                },
                runtime,
                source,
                camera);
        }

        // The ring is spatial clipmap coverage; this marker visualizes the
        // toroidal storage origin owned by ToroidalResidency for the same LOD.
        if (index <
                runtime.residency.levels.size() &&
            level.gridResolution > 1U)
        {
            const auto& residency =
                runtime.residency.levels[index];

            const f64 resolution =
                static_cast<f64>(
                    level.gridResolution);

            const f64 normalizedX =
                static_cast<f64>(
                    residency.originX) /
                    resolution -
                0.5;

            const f64 normalizedY =
                static_cast<f64>(
                    residency.originY) /
                    resolution -
                0.5;

            const f64 markerScale =
                level.outerHalfExtentMeters *
                0.55;

            const math::Double2 markerOffset{
                motion.centerOffsetMeters.x +
                    normalizedX * markerScale,
                motion.centerOffsetMeters.y +
                    normalizedY * markerScale
            };

            const auto centerDirection =
                world::DirectionAtSurfaceOffset(
                    runtime.planet,
                    motion.surfaceFrame,
                    motion.centerOffsetMeters);

            const auto markerDirection =
                world::DirectionAtSurfaceOffset(
                    runtime.planet,
                    motion.surfaceFrame,
                    markerOffset);

            AppendDirectionLine(
                result,
                centerDirection,
                markerDirection,
                {
                    1.00F,
                    0.82F,
                    0.18F,
                    0.90F
                },
                runtime,
                source,
                camera);

            const f64 tick =
                std::max(
                    level.sampleSpacingMeters *
                        2.0,
                    level.outerHalfExtentMeters *
                        0.008);

            const auto markerA =
                world::DirectionAtSurfaceOffset(
                    runtime.planet,
                    motion.surfaceFrame,
                    {
                        markerOffset.x - tick,
                        markerOffset.y
                    });

            const auto markerB =
                world::DirectionAtSurfaceOffset(
                    runtime.planet,
                    motion.surfaceFrame,
                    {
                        markerOffset.x + tick,
                        markerOffset.y
                    });

            AppendDirectionLine(
                result,
                markerA,
                markerB,
                {
                    1.00F,
                    0.82F,
                    0.18F,
                    1.00F
                },
                runtime,
                source,
                camera);
        }
    }
}

constexpr std::array<u8, 10U>
    kDigitSegments{
        0b00111111U,
        0b00000110U,
        0b01011011U,
        0b01001111U,
        0b01100110U,
        0b01101101U,
        0b01111101U,
        0b00000111U,
        0b01111111U,
        0b01101111U
    };

void AppendLocalSurfaceLine(
    std::vector<editor_ui::PreviewLine>& result,
    const world::SurfaceFrame& frame,
    const math::Double2& center,
    const math::Double2& a,
    const math::Double2& b,
    const math::Float4& color,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    AppendDirectionLine(
        result,
        world::DirectionAtSurfaceOffset(
            runtime.planet,
            frame,
            {
                center.x + a.x,
                center.y + a.y
            }),
        world::DirectionAtSurfaceOffset(
            runtime.planet,
            frame,
            {
                center.x + b.x,
                center.y + b.y
            }),
        color,
        runtime,
        source,
        camera);
}

void AppendLodGlyph(
    std::vector<editor_ui::PreviewLine>& result,
    const studio_session::StudioTerrainPhysicalPageSnapshot& page,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    const auto centerCoordinate =
        world::TileCenter(
            page.address.tile);

    const auto centerDirection =
        world::CubeToUnitDirection(
            centerCoordinate);

    const auto frame =
        world::MakeSurfaceFrame(
            centerDirection);

    const f64 tileWidth =
        world::ApproximateTileWidthMeters(
            runtime.planet,
            page.address.tile);

    const f64 height =
        std::clamp(
            tileWidth * 0.08,
            25.0,
            8'000.0);

    const f64 width =
        height * 0.52;

    const f64 gap =
        width * 0.28;

    const std::string text =
        std::to_string(
            static_cast<u32>(
                page.physicalLod));

    const f64 totalWidth =
        static_cast<f64>(text.size()) *
            width +
        static_cast<f64>(
            text.size() > 0U
                ? text.size() - 1U
                : 0U) *
            gap;

    const math::Float4 color{
        0.22F,
        0.95F,
        1.00F,
        1.00F
    };

    const std::array<
        std::pair<math::Double2, math::Double2>,
        7U>
        segments{{
            {{-0.5,  1.0}, { 0.5,  1.0}},
            {{ 0.5,  1.0}, { 0.5,  0.0}},
            {{ 0.5,  0.0}, { 0.5, -1.0}},
            {{-0.5, -1.0}, { 0.5, -1.0}},
            {{-0.5,  0.0}, {-0.5, -1.0}},
            {{-0.5,  1.0}, {-0.5,  0.0}},
            {{-0.5,  0.0}, { 0.5,  0.0}}
        }};

    for (std::size_t digitIndex = 0U;
         digitIndex < text.size();
         ++digitIndex)
    {
        const u32 digit =
            static_cast<u32>(
                text[digitIndex] - '0');

        if (digit > 9U)
        {
            continue;
        }

        const f64 xCenter =
            -0.5 * totalWidth +
            0.5 * width +
            static_cast<f64>(digitIndex) *
                (width + gap);

        const u8 mask =
            kDigitSegments[digit];

        for (u32 segment = 0U;
             segment < segments.size();
             ++segment)
        {
            if ((mask &
                 static_cast<u8>(
                     1U << segment)) == 0U)
            {
                continue;
            }

            const auto& [a, b] =
                segments[segment];

            AppendLocalSurfaceLine(
                result,
                frame,
                {},
                {
                    xCenter +
                        a.x * width,
                    a.y *
                        height * 0.5
                },
                {
                    xCenter +
                        b.x * width,
                    b.y *
                        height * 0.5
                },
                color,
                runtime,
                source,
                camera);
        }
    }
}

[[nodiscard]] const StudioTerrainDiagnosticPage*
ObserverPage(
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const std::span<const StudioTerrainDiagnosticPage> pages) noexcept
{
    const auto found =
        std::find_if(
            pages.begin(),
            pages.end(),
            [&](const StudioTerrainDiagnosticPage& page)
            {
                return page.status.address ==
                    runtime.observerPhysicalPage;
            });

    return found != pages.end()
        ? &*found
        : nullptr;
}

void AppendBiomeContours(
    std::vector<editor_ui::PreviewLine>& result,
    const StudioTerrainDiagnosticPage& page,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    if (page.snapshot == nullptr ||
        page.snapshot->debugPage == nullptr ||
        !page.snapshot->debugPage->Has(
            terrain_debug::TerrainDebugField::
                BiomeWeights))
    {
        return;
    }

    const auto view =
        page.snapshot->debugPage->View(
            terrain_debug::TerrainDebugField::
                BiomeWeights);

    if (view.width < 2U ||
        view.height < 2U ||
        view.scalar.empty())
    {
        return;
    }

    constexpr std::array<f32, 3U>
        thresholds{
            0.25F,
            0.50F,
            0.75F
        };

    constexpr std::array<math::Float4, 3U>
        colors{{
            {0.25F, 0.95F, 0.35F, 0.55F},
            {0.15F, 1.00F, 0.70F, 0.78F},
            {0.10F, 0.85F, 1.00F, 1.00F}
        }};

    const auto valueAt =
        [&](const u32 x,
            const u32 y)
        {
            return view.scalar[
                static_cast<std::size_t>(y) *
                    view.width +
                x];
        };

    struct Crossing
    {
        f64 x{0.0};
        f64 y{0.0};
    };

    const auto edgeCrossing =
        [](const f32 a,
           const f32 b,
           const f32 threshold,
           const f64 ax,
           const f64 ay,
           const f64 bx,
           const f64 by)
            -> std::optional<Crossing>
        {
            if (!std::isfinite(a) ||
                !std::isfinite(b) ||
                (a < threshold &&
                 b < threshold) ||
                (a >= threshold &&
                 b >= threshold) ||
                a == b)
            {
                return std::nullopt;
            }

            const f64 t =
                std::clamp(
                    static_cast<f64>(
                        (threshold - a) /
                        (b - a)),
                    0.0,
                    1.0);

            return Crossing{
                .x = std::lerp(ax, bx, t),
                .y = std::lerp(ay, by, t)
            };
        };

    for (std::size_t thresholdIndex = 0U;
         thresholdIndex <
             thresholds.size();
         ++thresholdIndex)
    {
        const f32 threshold =
            thresholds[thresholdIndex];

        for (u32 y = 0U;
             y + 1U < view.height;
             ++y)
        {
            for (u32 x = 0U;
                 x + 1U < view.width;
                 ++x)
            {
                const f32 v00 =
                    valueAt(x, y);
                const f32 v10 =
                    valueAt(x + 1U, y);
                const f32 v11 =
                    valueAt(
                        x + 1U,
                        y + 1U);
                const f32 v01 =
                    valueAt(x, y + 1U);

                std::array<Crossing, 4U>
                    crossings{};
                u32 count = 0U;

                const auto append =
                    [&](const std::optional<Crossing>& item)
                    {
                        if (item.has_value() &&
                            count <
                                crossings.size())
                        {
                            crossings[count++] =
                                *item;
                        }
                    };

                append(
                    edgeCrossing(
                        v00,
                        v10,
                        threshold,
                        x,
                        y,
                        x + 1.0,
                        y));

                append(
                    edgeCrossing(
                        v10,
                        v11,
                        threshold,
                        x + 1.0,
                        y,
                        x + 1.0,
                        y + 1.0));

                append(
                    edgeCrossing(
                        v11,
                        v01,
                        threshold,
                        x + 1.0,
                        y + 1.0,
                        x,
                        y + 1.0));

                append(
                    edgeCrossing(
                        v01,
                        v00,
                        threshold,
                        x,
                        y + 1.0,
                        x,
                        y));

                if (count >= 2U)
                {
                    AppendGridLine(
                        result,
                        page,
                        crossings[0].x,
                        crossings[0].y,
                        crossings[1].x,
                        crossings[1].y,
                        view.width,
                        view.height,
                        colors[thresholdIndex],
                        runtime,
                        source,
                        camera);
                }

                if (count == 4U)
                {
                    AppendGridLine(
                        result,
                        page,
                        crossings[2].x,
                        crossings[2].y,
                        crossings[3].x,
                        crossings[3].y,
                        view.width,
                        view.height,
                        colors[thresholdIndex],
                        runtime,
                        source,
                        camera);
                }
            }
        }
    }
}

void AppendProcessEffects(
    std::vector<editor_ui::PreviewLine>& result,
    const StudioTerrainDiagnosticPage& page,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    if (page.snapshot == nullptr ||
        page.snapshot->debugPage == nullptr ||
        !page.snapshot->debugPage->Has(
            terrain_debug::TerrainDebugField::
                ErosionDeposition))
    {
        return;
    }

    const auto view =
        page.snapshot->debugPage->View(
            terrain_debug::TerrainDebugField::
                ErosionDeposition);

    if (view.scalar.empty() ||
        view.width < 2U ||
        view.height < 2U)
    {
        return;
    }

    f32 maximum = 0.0F;

    for (const f32 value :
         view.scalar)
    {
        if (std::isfinite(value))
        {
            maximum =
                std::max(
                    maximum,
                    std::abs(value));
        }
    }

    if (maximum <= 0.0F)
    {
        return;
    }

    const u32 stride =
        std::max<u32>(
            1U,
            std::min(
                view.width,
                view.height) /
                16U);

    const f64 half =
        std::max(
            static_cast<f64>(stride) *
                0.18,
            0.18);

    for (u32 y = 0U;
         y < view.height;
         y += stride)
    {
        for (u32 x = 0U;
             x < view.width;
             x += stride)
        {
            const f32 value =
                view.scalar[
                    static_cast<std::size_t>(y) *
                        view.width +
                    x];

            if (!std::isfinite(value) ||
                std::abs(value) <
                    maximum * 0.05F)
            {
                continue;
            }

            const math::Float4 color =
                value < 0.0F
                    ? math::Float4{
                        1.00F,
                        0.30F,
                        0.10F,
                        0.90F}
                    : math::Float4{
                        0.30F,
                        1.00F,
                        0.35F,
                        0.90F};

            AppendGridLine(
                result,
                page,
                static_cast<f64>(x) -
                    half,
                y,
                static_cast<f64>(x) +
                    half,
                y,
                view.width,
                view.height,
                color,
                runtime,
                source,
                camera);

            AppendGridLine(
                result,
                page,
                x,
                static_cast<f64>(y) -
                    half,
                x,
                static_cast<f64>(y) +
                    half,
                view.width,
                view.height,
                color,
                runtime,
                source,
                camera);
        }
    }
}

void AppendDrainageVectors(
    std::vector<editor_ui::PreviewLine>& result,
    const StudioTerrainDiagnosticPage& page,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    if (page.snapshot == nullptr ||
        page.snapshot->debugPage == nullptr ||
        !page.snapshot->debugPage->Has(
            terrain_debug::TerrainDebugField::
                Drainage))
    {
        return;
    }

    const auto view =
        page.snapshot->debugPage->View(
            terrain_debug::TerrainDebugField::
                Drainage);

    if (view.vector.empty() ||
        view.width < 2U ||
        view.height < 2U)
    {
        return;
    }

    f64 maximum = 0.0;

    for (const auto& vector :
         view.vector)
    {
        if (std::isfinite(vector.x) &&
            std::isfinite(vector.y))
        {
            maximum =
                std::max(
                    maximum,
                    std::hypot(
                        static_cast<f64>(
                            vector.x),
                        static_cast<f64>(
                            vector.y)));
        }
    }

    if (maximum <= 0.0)
    {
        return;
    }

    const u32 stride =
        std::max<u32>(
            1U,
            std::min(
                view.width,
                view.height) /
                16U);

    const math::Float4 color{
        0.20F,
        0.62F,
        1.00F,
        0.92F
    };

    for (u32 y = 0U;
         y < view.height;
         y += stride)
    {
        for (u32 x = 0U;
             x < view.width;
             x += stride)
        {
            const auto vector =
                view.vector[
                    static_cast<std::size_t>(y) *
                        view.width +
                    x];

            const f64 magnitude =
                std::hypot(
                    static_cast<f64>(
                        vector.x),
                    static_cast<f64>(
                        vector.y));

            if (!std::isfinite(magnitude) ||
                magnitude <=
                    maximum * 0.01)
            {
                continue;
            }

            const f64 nx =
                static_cast<f64>(
                    vector.x) /
                magnitude;

            const f64 ny =
                static_cast<f64>(
                    vector.y) /
                magnitude;

            const f64 length =
                static_cast<f64>(stride) *
                (0.35 +
                 0.55 *
                    std::sqrt(
                        std::clamp(
                            magnitude /
                                maximum,
                            0.0,
                            1.0)));

            const f64 endX =
                static_cast<f64>(x) +
                nx * length;

            const f64 endY =
                static_cast<f64>(y) +
                ny * length;

            AppendGridLine(
                result,
                page,
                x,
                y,
                endX,
                endY,
                view.width,
                view.height,
                color,
                runtime,
                source,
                camera);

            const f64 head =
                length * 0.28;

            AppendGridLine(
                result,
                page,
                endX,
                endY,
                endX -
                    nx * head -
                    ny * head * 0.55,
                endY -
                    ny * head +
                    nx * head * 0.55,
                view.width,
                view.height,
                color,
                runtime,
                source,
                camera);

            AppendGridLine(
                result,
                page,
                endX,
                endY,
                endX -
                    nx * head +
                    ny * head * 0.55,
                endY -
                    ny * head -
                    nx * head * 0.55,
                view.width,
                view.height,
                color,
                runtime,
                source,
                camera);
        }
    }
}
} // namespace

std::vector<editor_ui::PreviewLine>
BuildTerrainDiagnosticOverlayLines(
    const StudioTerrainDiagnosticOverlayOptions& options,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const std::span<const StudioTerrainDiagnosticPage> pages,
    const render_view::CameraState& camera)
{
    std::vector<editor_ui::PreviewLine>
        result;

    if (!options.Any() ||
        !runtime.body.IsValid() ||
        !runtime.planet.id.IsValid() ||
        runtime.planet.radiusMeters <= 0.0)
    {
        return result;
    }

    if (options.clipmapRings)
    {
        AppendClipmapRings(
            result,
            runtime,
            source,
            camera);
    }

    for (const auto& page :
         pages)
    {
        bool drawBounds = false;
        math::Float4 color{
            0.65F,
            0.65F,
            0.65F,
            0.85F
        };

        if (options.buildStates)
        {
            drawBounds = true;
            color =
                StateColor(
                    page.status.state);
        }
        else if (options.cacheStatus &&
                 page.snapshot != nullptr)
        {
            drawBounds = true;
            color =
                page.snapshot->cacheResident
                    ? math::Float4{
                        0.22F,
                        0.95F,
                        0.38F,
                        0.90F}
                    : math::Float4{
                        1.00F,
                        0.42F,
                        0.12F,
                        0.90F};
        }
        else if (options.dirtyPageBounds &&
                 IsDirtyLike(
                     page.status.state))
        {
            drawBounds = true;
            color =
                StateColor(
                    page.status.state);
        }

        if (drawBounds)
        {
            AppendTileBounds(
                result,
                page.status.address.tile,
                color,
                runtime,
                source,
                camera);
        }

        if (options.physicalLod &&
            page.snapshot != nullptr)
        {
            AppendLodGlyph(
                result,
                *page.snapshot,
                runtime,
                source,
                camera);
        }
    }

    const auto* observerPage =
        ObserverPage(
            runtime,
            pages);

    if (observerPage != nullptr)
    {
        if (options.biomeWeights)
        {
            AppendBiomeContours(
                result,
                *observerPage,
                runtime,
                source,
                camera);
        }

        if (options.processMasks)
        {
            AppendProcessEffects(
                result,
                *observerPage,
                runtime,
                source,
                camera);
        }

        if (options.drainageVectors)
        {
            AppendDrainageVectors(
                result,
                *observerPage,
                runtime,
                source,
                camera);
        }
    }

    return result;
}
} // namespace orbit::studio_ui
