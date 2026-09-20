#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 Pi =
    3.1415926535897932384626433832795;

[[nodiscard]] math::Double3 Unit(
    const math::Double3& value)
{
    const f64 length =
        math::Length(value);

    if (!std::isfinite(length) ||
        length <= 1.0e-12)
    {
        throw std::invalid_argument(
            "Terrain overlay contains an invalid surface direction.");
    }

    return value / length;
}

[[nodiscard]] math::Double3 Slerp(
    const math::Double3& a,
    const math::Double3& b,
    const f64 t) noexcept
{
    const f64 dot =
        std::clamp(
            math::Dot(a, b),
            -1.0,
            1.0);

    const f64 angle =
        std::acos(dot);

    if (angle <= 1.0e-12)
    {
        return a;
    }

    const f64 sine =
        std::sin(angle);

    if (std::abs(sine) <= 1.0e-12)
    {
        return math::Normalize(
            a * (1.0 - t) +
            b * t);
    }

    return math::Normalize(
        a *
            (std::sin((1.0 - t) * angle) /
             sine) +
        b *
            (std::sin(t * angle) /
             sine));
}

[[nodiscard]] math::Float3 SurfacePoint(
    const math::Double3& directionValue,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    const math::Double3 direction =
        Unit(directionValue);

    const auto sample =
        source.Sample({
            .unitDirection = direction,
            .footprintMeters = 1.0,
            .planet = runtime.planet.id,
            .radialOffsetMeters = 0.0
        });

    const f64 radius =
        runtime.planet.radiusMeters +
        sample.elevationMeters +
        5.0;

    const math::Double3 relative =
        direction * radius -
        camera.localPositionMeters;

    return {
        static_cast<f32>(relative.x),
        static_cast<f32>(relative.y),
        static_cast<f32>(relative.z)
    };
}

void AppendLine(
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
            SurfacePoint(
                a,
                runtime,
                source,
                camera),
        .end =
            SurfacePoint(
                b,
                runtime,
                source,
                camera),
        .color = color
    });
}

void AppendGreatCircle(
    std::vector<editor_ui::PreviewLine>& result,
    const math::Double3& startValue,
    const math::Double3& endValue,
    const math::Float4& color,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    const math::Double3 start =
        Unit(startValue);
    const math::Double3 end =
        Unit(endValue);

    const f64 angle =
        std::acos(
            std::clamp(
                math::Dot(start, end),
                -1.0,
                1.0));

    const u32 segments =
        std::clamp<u32>(
            static_cast<u32>(
                std::ceil(
                    angle / 0.005)),
            1U,
            128U);

    math::Double3 previous =
        start;

    for (u32 index = 1U;
         index <= segments;
         ++index)
    {
        const math::Double3 current =
            Slerp(
                start,
                end,
                static_cast<f64>(index) /
                    static_cast<f64>(
                        segments));

        AppendLine(
            result,
            previous,
            current,
            color,
            runtime,
            source,
            camera);

        previous = current;
    }
}

void AppendControlCross(
    std::vector<editor_ui::PreviewLine>& result,
    const math::Double3& directionValue,
    const f64 sizeMeters,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    const math::Double3 direction =
        Unit(directionValue);

    const auto frame =
        world::MakeSurfaceFrame(
            direction);

    const f64 angular =
        std::clamp(
            sizeMeters /
                runtime.planet.radiusMeters,
            1.0e-9,
            0.05);

    const auto offsetDirection =
        [&](const math::Double3& tangent,
            const f64 sign)
        {
            return math::Normalize(
                direction +
                tangent *
                    (angular * sign));
        };

    const math::Float4 color{
        1.0F,
        1.0F,
        1.0F,
        1.0F
    };

    AppendLine(
        result,
        offsetDirection(frame.east, -1.0),
        offsetDirection(frame.east, 1.0),
        color,
        runtime,
        source,
        camera);

    AppendLine(
        result,
        offsetDirection(frame.north, -1.0),
        offsetDirection(frame.north, 1.0),
        color,
      runtime,
      source,
        camera);
}
} // namespace

std::vector<editor_ui::PreviewLine>
BuildTerrainAuthoringOverlayLines(
    const StudioTerrainAuthoringOverlay& overlay,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera)
{
    if (overlay.body != runtime.body ||
        !truntime.planet.id.IsValid() ||
        runtime.planet.radiusMeters <= 0.0 ||
        overlay.controlUnitDirections.empty() ||
        !std::isfinite(
            overlay.influenceRadiusMeters) ||
        overlay.influenceRadiusMeters < 0.0)
    {
        return {};
    }

    std::vector<editor_ui::PreviewLine> result;

    const math::Float4 footprintColor{
        1.0F,
        0.72F,
        0.18F,
        1.0F
    };

    const math::Float4 splineColor{
        0.18F,
        0.88F,
        1.0F,
        1.0F
    };

    const f64 crossSize =
        std::clamp(
            overlay.influenceRadiusMeters *
                0.10,
            15.0,
            500.0);

    if (overlay.kind ==
        StudioTerrainOverlayKind::Brush)
    {
        const math::Double3 center =
            Unit(
                overlay.
                    controlUnitDirections.
                    front());

        const auto frame =
            world::MakeSurfaceFrame(
                center);

        const f64 angular =
            std::clamp(
                overlay.influenceRadiusMeters /
                    runtime.planet.radiusMeters,
                0.0,
                Pi * 0.45);

        constexpr u32 Segments = 64U;

        math::Double3 previous{};

        for (u32 index = 0U;
             index <= Segments;
             ++index)
        {
            const f64 angle =
                2.0 * Pi *
                static_cast<f64>(index) /
                static_cast<f64>(Segments);

            const math::Double3 tangent =
                frame.east *
                    std::cos(angle) +
                frame.north *
                    std::sin(angle);

            const math::Double3 current =
                math::Normalize(
                    center *
                        std::cos(angular) +
                    tangent *
                        std::sin(angular));

            if (index == 0U)
            {
                previous = current;
                continue;
            }

            AppendLine(
                result,
                previous,
                current,
                footprintColor,
                runtime,
                source,
                camera);

            previous = current;
        }

        AppendControlCross(
            result,
            center,
            crossSize,
            runtime,
            source,
            camera);

        return result;
    }

    for (std::size_t index = 1U;
         index <
             overlay.
                 controlUnitDirections.
                 size();
         ++index)
    {
        AppendGreatCircle(
            result,
            overlay.
                controlUnitDirections[
                    index - 1U],
            overlay.
                controlUnitDirections[
                    index],
            splineColor,
            runtime,
            source,
            camera);
    }

    for (const auto& point :
         overlay.controlUnitDirections)
    {
        AppendControlCross(
            result,
            point,
            crossSize,
            runtime,
            source,
            camera);
    }

    return result;
}
} // namespace orbit::studio_ui
