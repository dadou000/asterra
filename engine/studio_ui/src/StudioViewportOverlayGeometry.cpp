#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui::viewport_detail
{

[[nodiscard]] std::vector<editor_ui::PreviewLine>
SelectedMagnetosphereDiagnosticLines(
    studio_session::StudioSession& session,
    const world_model::ResolvedMagnetosphere& resolved,
    const f64 referenceRadiusMeters,
    const render_view::CameraState& camera)
{
    std::vector<editor_ui::PreviewLine> lines;

    if (!session.World().HasWorld() ||
        session.World().Selection().Ordered().size() != 1U ||
        referenceRadiusMeters <= 0.0)
    {
        return lines;
    }

    const auto selected =
        session.World().Selection().Ordered().front();
    const auto selectedRecord =
        session.World().Objects().Find(selected);

    if (!selectedRecord.has_value() ||
        selectedRecord->type !=
            world_model::kMagnetosphereCapabilityType)
    {
        return lines;
    }

    const auto toRelative =
        [&](const math::Double3 p)
        {
            return math::Float3{
                static_cast<f32>(p.x - camera.localPositionMeters.x),
                static_cast<f32>(p.y - camera.localPositionMeters.y),
                static_cast<f32>(p.z - camera.localPositionMeters.z)
            };
        };

    auto axis = resolved.parameters.dipoleAxis;
    if (math::LengthSquared(axis) <= 1.0e-20)
        axis = {0.0, 0.0, 1.0};
    axis = math::Normalize(axis);

    math::Double3 reference{1.0, 0.0, 0.0};
    if (std::abs(math::Dot(axis, reference)) > 0.9)
        reference = {0.0, 1.0, 0.0};

    const auto basisU =
        math::Normalize(
            reference - axis * math::Dot(axis, reference));
    const auto basisV =
        math::Normalize(math::Cross(axis, basisU));

    constexpr math::Float4 kFieldColor{
        0.28F, 0.72F, 1.0F, 0.8F};
    constexpr math::Float4 kMagnetopauseColor{
        0.85F, 0.36F, 1.0F, 0.85F};

    constexpr u32 kFieldShells = 6U;
    constexpr u32 kFieldSteps = 64U;
    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    for (u32 shell = 0U; shell < kFieldShells; ++shell)
    {
        const f64 azimuth =
            2.0 * std::numbers::pi_v<f64> *
            static_cast<f64>(shell) /
            static_cast<f64>(kFieldShells);

        const auto equatorial =
            basisU * std::cos(azimuth) +
            basisV * std::sin(azimuth);

        const f64 lShell =
            2.0 + static_cast<f64>(shell % 3U) * 0.85;

        std::optional<math::Double3> previous;

        for (u32 step = 0U; step <= kFieldSteps; ++step)
        {
            const f64 latitude =
                (-70.0 +
                 140.0 * static_cast<f64>(step) /
                     static_cast<f64>(kFieldSteps)) *
                kDegreesToRadians;

            const f64 cosLatitude = std::cos(latitude);
            const f64 radiusBodyRadii =
                lShell * cosLatitude * cosLatitude;

            const auto direction =
                math::Normalize(
                    axis * std::sin(latitude) +
                    equatorial * cosLatitude);

            const auto point =
                direction *
                (radiusBodyRadii * referenceRadiusMeters);

            if (radiusBodyRadii > 1.03)
            {
                if (previous.has_value())
                {
                    lines.push_back({
                        .start = toRelative(*previous),
                        .end = toRelative(point),
                        .color = kFieldColor
                    });
                }
                previous = point;
            }
            else
            {
                previous.reset();
            }
        }
    }

    auto wind = resolved.parameters.solarWindDirection;
    if (math::LengthSquared(wind) <= 1.0e-20)
        wind = {-1.0, 0.0, 0.0};

    const auto sunward = math::Normalize(wind) * -1.0;

    math::Double3 windReference{0.0, 1.0, 0.0};
    if (std::abs(math::Dot(sunward, windReference)) > 0.9)
        windReference = {0.0, 0.0, 1.0};

    const auto windU =
        math::Normalize(
            windReference -
            sunward * math::Dot(sunward, windReference));
    const auto windV =
        math::Normalize(math::Cross(sunward, windU));

    constexpr u32 kEnvelopeSteps = 72U;

    const auto addEnvelope =
        [&](const math::Double3 transverse)
        {
            std::optional<math::Double3> previousPositive;
            std::optional<math::Double3> previousNegative;

            for (u32 step = 0U; step <= kEnvelopeSteps; ++step)
            {
                const f64 theta =
                    (std::numbers::pi_v<f64> - 0.08) *
                    static_cast<f64>(step) /
                    static_cast<f64>(kEnvelopeSteps);

                const auto positiveDirection =
                    math::Normalize(
                        sunward * std::cos(theta) +
                        transverse * std::sin(theta));

                const auto negativeDirection =
                    math::Normalize(
                        sunward * std::cos(theta) -
                        transverse * std::sin(theta));

                const auto positivePoint =
                    positiveDirection *
                    celestial_magnetosphere::
                        MagnetopauseRadiusMeters(
                            resolved.parameters,
                            referenceRadiusMeters,
                            positiveDirection);

                const auto negativePoint =
                    negativeDirection *
                    celestial_magnetosphere::
                        MagnetopauseRadiusMeters(
                            resolved.parameters,
                            referenceRadiusMeters,
                            negativeDirection);

                if (previousPositive.has_value())
                {
                    lines.push_back({
                        .start = toRelative(*previousPositive),
                        .end = toRelative(positivePoint),
                        .color = kMagnetopauseColor
                    });
                }

                if (previousNegative.has_value())
                {
                    lines.push_back({
                        .start = toRelative(*previousNegative),
                        .end = toRelative(negativePoint),
                        .color = kMagnetopauseColor
                    });
                }

                previousPositive = positivePoint;
                previousNegative = negativePoint;
            }
        };

    addEnvelope(windU);
    addEnvelope(windV);

    return lines;
}

[[nodiscard]] std::vector<editor_ui::PreviewLine>
SelectedLocalLightGizmoLines(
    studio_session::StudioSession& session,
    const render_view::CameraState& camera)
{
    std::vector<editor_ui::PreviewLine> lines;

    if (!session.World().HasWorld() ||
        session.World().Selection().Ordered().size() != 1U)
    {
        return lines;
    }

    const auto selected =
        session.World().Selection().Ordered().front();

    const auto record =
        session.World().Objects().Find(
            selected);

    if (!record.has_value() ||
        (record->type != world_model::kPointLightType &&
         record->type != world_model::kSpotLightType))
    {
        return lines;
    }

    const auto resolved =
        world_model::ResolveAuthoredLocalLights(
            session.World().Objects(),
            selected);

    if (resolved.empty())
    {
        return lines;
    }

    const auto& light = resolved.front();

    const math::Float3 center{
        static_cast<f32>(
            light.positionMeters.x -
            camera.localPositionMeters.x),
        static_cast<f32>(
            light.positionMeters.y -
            camera.localPositionMeters.y),
        static_cast<f32>(
            light.positionMeters.z -
            camera.localPositionMeters.z)
    };

    const f32 range =
        static_cast<f32>(
            std::max(
                light.rangeMeters,
                0.001));

    const math::Float4 color =
        light.kind ==
                world_model::AuthoredLightKind::Spot
            ? math::Float4{
                  1.0F, 0.55F, 0.15F, 1.0F}
            : math::Float4{
                  1.0F, 0.82F, 0.25F, 1.0F};

    const auto addLine =
        [&lines, color](
            const math::Float3 a,
            const math::Float3 b)
        {
            lines.push_back({
                .start = a,
                .end = b,
                .color = color
            });
        };

    if (light.kind ==
        world_model::AuthoredLightKind::Point)
    {
        addLine(
            center + math::Float3{-range, 0.0F, 0.0F},
            center + math::Float3{ range, 0.0F, 0.0F});
        addLine(
            center + math::Float3{0.0F, -range, 0.0F},
            center + math::Float3{0.0F,  range, 0.0F});
        addLine(
            center + math::Float3{0.0F, 0.0F, -range},
            center + math::Float3{0.0F, 0.0F,  range});
        return lines;
    }

    math::Float3 direction{
        static_cast<f32>(light.direction.x),
        static_cast<f32>(light.direction.y),
        static_cast<f32>(light.direction.z)
    };

    if (math::LengthSquared(direction) <= 1.0e-8F)
    {
        direction = {0.0F, -1.0F, 0.0F};
    }
    else
    {
        direction = math::Normalize(direction);
    }

    math::Float3 upHint{0.0F, 1.0F, 0.0F};

    if (std::abs(
            math::Dot(
                direction,
                upHint)) >
        0.95F)
    {
        upHint = {1.0F, 0.0F, 0.0F};
    }

    const math::Float3 right =
        math::Normalize(
            math::Cross(
                upHint,
                direction));
    const math::Float3 coneUp =
        math::Normalize(
            math::Cross(
                direction,
                right));

    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    const f32 outerRadians =
        static_cast<f32>(
            std::clamp(
                light.outerConeDegrees,
                0.0,
                89.5) *
            kDegreesToRadians);

    const f32 coneRadius =
        std::tan(outerRadians) *
        range;

    const math::Float3 tip =
        center +
        direction * range;

    const std::array rim{
        tip + right * coneRadius,
        tip + coneUp * coneRadius,
        tip - right * coneRadius,
        tip - coneUp * coneRadius
    };

    addLine(center, tip);

    for (const auto& point : rim)
    {
        addLine(center, point);
    }

    for (std::size_t index = 0U;
         index < rim.size();
         ++index)
    {
        addLine(
            rim[index],
            rim[(index + 1U) %
                rim.size()]);
    }

    return lines;
}

[[nodiscard]] std::vector<editor_ui::PreviewLine>
VolumeInputGizmoLines(
    studio_session::StudioSession& session,
    const render_view::CameraState& camera,
    const bool showAll)
{
    std::vector<editor_ui::PreviewLine> lines;

    if (!session.World().HasWorld() ||
        session.World().Selection().Ordered().size() != 1U)
    {
        return lines;
    }

    const auto selected =
        session.World().Selection().Ordered().front();
    const auto selectedRecord =
        session.World().Objects().Find(
            selected);

    if (!selectedRecord.has_value())
    {
        return lines;
    }

    std::optional<scene::ObjectId> volumeId;

    if (selectedRecord->type ==
        world_model::kVolumeType)
    {
        volumeId =
            selectedRecord->id;
    }
    else if ((selectedRecord->type ==
                  world_model::kVolumeSourceType ||
              selectedRecord->type ==
                  world_model::kVolumeEffectorType) &&
             selectedRecord->parent.has_value())
    {
        volumeId =
            selectedRecord->parent;
    }

    if (!volumeId.has_value())
    {
        return lines;
    }

    const auto inputs =
        world_model::ResolveVolumeInputs(
            session.World().Objects(),
            *volumeId);

    const auto relative =
        [&](const math::Double3 point)
        {
            return math::Float3{
                static_cast<f32>(
                    point.x -
                    camera.localPositionMeters.x),
                static_cast<f32>(
                    point.y -
                    camera.localPositionMeters.y),
                static_cast<f32>(
                    point.z -
                    camera.localPositionMeters.z)
            };
        };

    for (const auto& input :
         inputs)
    {
        if (!showAll &&
            input.object != selected)
        {
            continue;
        }

        const math::Float4 color =
            input.role ==
                    world_model::
                        VolumeInputRole::Source
                ? math::Float4{
                      0.20F, 0.95F, 0.72F, 0.95F}
                : math::Float4{
                      1.0F, 0.48F, 0.18F, 0.95F};

        const auto center =
            relative(
                input.positionMeters);
        const f32 marker =
            static_cast<f32>(
                std::max(
                    input.radiusMeters * 0.2,
                    0.25));

        lines.push_back({
            .start = center + math::Float3{-marker,0.0F,0.0F},
            .end = center + math::Float3{marker,0.0F,0.0F},
            .color = color
        });
        lines.push_back({
            .start = center + math::Float3{0.0F,-marker,0.0F},
            .end = center + math::Float3{0.0F,marker,0.0F},
            .color = color
        });
        lines.push_back({
            .start = center + math::Float3{0.0F,0.0F,-marker},
            .end = center + math::Float3{0.0F,0.0F,marker},
            .color = color
        });

        if (input.shape ==
                world_model::
                    VolumeSourceShape::Point)
        {
            continue;
        }

        if (input.shape ==
                world_model::
                    VolumeSourceShape::Sphere)
        {
            constexpr u32 segments = 24U;
            const f64 radius =
                std::max(
                    input.radiusMeters,
                    0.001);

            for (u32 axis = 0U;
                 axis < 3U;
                 ++axis)
            {
                for (u32 segment = 0U;
                     segment < segments;
                     ++segment)
                {
                    const f64 a =
                        2.0 *
                        std::numbers::pi_v<f64> *
                        static_cast<f64>(segment) /
                        static_cast<f64>(segments);
                    const f64 b =
                        2.0 *
                        std::numbers::pi_v<f64> *
                        static_cast<f64>(segment + 1U) /
                        static_cast<f64>(segments);

                    math::Double3 pa =
                        input.positionMeters;
                    math::Double3 pb =
                        input.positionMeters;

                    if (axis == 0U)
                    {
                        pa.y += std::cos(a) * radius;
                        pa.z += std::sin(a) * radius;
                        pb.y += std::cos(b) * radius;
                        pb.z += std::sin(b) * radius;
                    }
                    else if (axis == 1U)
                    {
                        pa.x += std::cos(a) * radius;
                        pa.z += std::sin(a) * radius;
                        pb.x += std::cos(b) * radius;
                        pb.z += std::sin(b) * radius;
                    }
                    else
                    {
                        pa.x += std::cos(a) * radius;
                        pa.y += std::sin(a) * radius;
                        pb.x += std::cos(b) * radius;
                        pb.y += std::sin(b) * radius;
                    }

                    const auto ra = relative(pa);
                    const auto rb = relative(pb);

                    lines.push_back({
                        .start = ra,
                        .end = rb,
                        .color = color
                    });
                }
            }
        }
        else
        {
            const auto& bounds =
                input.bounds;

            const std::array<math::Double3,8> worldPoints{{
                {bounds.minimumMeters.x,bounds.minimumMeters.y,bounds.minimumMeters.z},
                {bounds.maximumMeters.x,bounds.minimumMeters.y,bounds.minimumMeters.z},
                {bounds.maximumMeters.x,bounds.maximumMeters.y,bounds.minimumMeters.z},
                {bounds.minimumMeters.x,bounds.maximumMeters.y,bounds.minimumMeters.z},
                {bounds.minimumMeters.x,bounds.minimumMeters.y,bounds.maximumMeters.z},
                {bounds.maximumMeters.x,bounds.minimumMeters.y,bounds.maximumMeters.z},
                {bounds.maximumMeters.x,bounds.maximumMeters.y,bounds.maximumMeters.z},
                {bounds.minimumMeters.x,bounds.maximumMeters.y,bounds.maximumMeters.z}
            }};

            constexpr std::array<std::array<u32,2>,12> edges{{
                {{0,1}},{{1,2}},{{2,3}},{{3,0}},
                {{4,5}},{{5,6}},{{6,7}},{{7,4}},
                {{0,4}},{{1,5}},{{2,6}},{{3,7}}
            }};

            for (const auto& edge :
                 edges)
            {
                lines.push_back({
                    .start =
                        relative(
                            worldPoints[
                                edge[0]]),
                    .end =
                        relative(
                            worldPoints[
                                edge[1]]),
                    .color = color
                });
            }
        }

        if (input.shape ==
                world_model::
                    VolumeSourceShape::Spline ||
            math::LengthSquared(
                input.vectorValue) >
                1.0e-12)
        {
            const auto vectorEnd =
                math::Double3{
                    input.positionMeters.x +
                        input.vectorValue.x,
                    input.positionMeters.y +
                        input.vectorValue.y,
                    input.positionMeters.z +
                        input.vectorValue.z
                };

            lines.push_back({
                .start = center,
                .end = relative(vectorEnd),
                .color = {
                    color.x,
                    color.y,
                    color.z,
                    1.0F
                }
            });
        }
    }

    return lines;
}

[[nodiscard]] std::vector<editor_ui::PreviewLine>
VolumeSlicePlaneLines(
    const world_model::ResolvedVolumeDomain& volume,
    const render_view::CameraState& camera,
    const volume_solver::VolumeSliceAxis axis,
    const u32 sliceIndex)
{
    std::vector<editor_ui::PreviewLine> lines;

    if (volume.solverPolicy !=
        world_model::VolumeSolverPolicy::Local3D ||
        volume.resolution == 0U)
    {
        return lines;
    }

    const auto relative =
        [&](const math::Double3 point)
        {
            return math::Float3{
                static_cast<f32>(
                    point.x -
                    camera.localPositionMeters.x),
                static_cast<f32>(
                    point.y -
                    camera.localPositionMeters.y),
                static_cast<f32>(
                    point.z -
                    camera.localPositionMeters.z)
            };
        };

    const u32 clamped =
        std::min(
            sliceIndex,
            volume.resolution - 1U);

    const f64 fraction =
        (static_cast<f64>(clamped) +
         0.5) /
        static_cast<f64>(
            volume.resolution);

    const auto minimum =
        math::Double3{
            volume.centerMeters.x -
                volume.halfExtentsMeters.x,
            volume.centerMeters.y -
                volume.halfExtentsMeters.y,
            volume.centerMeters.z -
                volume.halfExtentsMeters.z
        };

    const auto maximum =
        math::Double3{
            volume.centerMeters.x +
                volume.halfExtentsMeters.x,
            volume.centerMeters.y +
                volume.halfExtentsMeters.y,
            volume.centerMeters.z +
                volume.halfExtentsMeters.z
        };

    std::array<math::Double3,4>
        points{};

    if (axis ==
        volume_solver::VolumeSliceAxis::X)
    {
        const f64 x =
            minimum.x +
            (maximum.x - minimum.x) *
                fraction;
        points = {{
            {x,minimum.y,minimum.z},
            {x,maximum.y,minimum.z},
            {x,maximum.y,maximum.z},
            {x,minimum.y,maximum.z}
        }};
    }
    else if (axis ==
             volume_solver::VolumeSliceAxis::Y)
    {
        const f64 y =
            minimum.y +
            (maximum.y - minimum.y) *
                fraction;
        points = {{
            {minimum.x,y,minimum.z},
            {maximum.x,y,minimum.z},
            {maximum.x,y,maximum.z},
            {minimum.x,y,maximum.z}
        }};
    }
    else
    {
        const f64 z =
            minimum.z +
            (maximum.z - minimum.z) *
                fraction;
        points = {{
            {minimum.x,minimum.y,z},
            {maximum.x,minimum.y,z},
            {maximum.x,maximum.y,z},
            {minimum.x,maximum.y,z}
        }};
    }

    const math::Float4 color{
        0.95F, 0.35F, 0.85F, 0.95F};

    for (u32 index = 0U;
         index < 4U;
         ++index)
    {
        lines.push_back({
            .start =
                relative(
                    points[index]),
            .end =
                relative(
                    points[
                        (index + 1U) %
                        4U]),
            .color = color
        });
    }

    return lines;
}

[[nodiscard]] std::vector<editor_ui::PreviewLine>
SelectedVolumeDomainLines(
    studio_session::StudioSession& session,
    const render_view::CameraState& camera)
{
    std::vector<editor_ui::PreviewLine> lines;

    if (!session.World().HasWorld() ||
        session.World().Selection().Ordered().size() != 1U)
    {
        return lines;
    }

    const auto selected =
        session.World().Selection().Ordered().front();

    scene::ObjectId volumeObject =
        selected;

    const auto selectedRecord =
        session.World().Objects().Find(
            selected);

    if (selectedRecord.has_value() &&
        (selectedRecord->type ==
             world_model::kVolumeSourceType ||
         selectedRecord->type ==
             world_model::kVolumeEffectorType) &&
        selectedRecord->parent.has_value())
    {
        volumeObject =
            *selectedRecord->parent;
    }

    const auto volume =
        world_model::ResolveVolumeDomain(
            session.World().Objects(),
            volumeObject);

    if (!volume.has_value())
    {
        return lines;
    }

    const auto center =
        volume->centerMeters;
    const auto half =
        volume->halfExtentsMeters;

    const auto relative =
        [&](const f64 x,
            const f64 y,
            const f64 z)
        {
            return math::Float3{
                static_cast<f32>(
                    center.x + x -
                    camera.localPositionMeters.x),
                static_cast<f32>(
                    center.y + y -
                    camera.localPositionMeters.y),
                static_cast<f32>(
                    center.z + z -
                    camera.localPositionMeters.z)
            };
        };

    const std::array<math::Float3, 8> p{
        relative(-half.x,-half.y,-half.z),
        relative( half.x,-half.y,-half.z),
        relative( half.x, half.y,-half.z),
        relative(-half.x, half.y,-half.z),
        relative(-half.x,-half.y, half.z),
        relative( half.x,-half.y, half.z),
        relative( half.x, half.y, half.z),
        relative(-half.x, half.y, half.z)
    };

    constexpr std::array<std::array<u32,2>,12> edges{{
        {{0,1}},{{1,2}},{{2,3}},{{3,0}},
        {{4,5}},{{5,6}},{{6,7}},{{7,4}},
        {{0,4}},{{1,5}},{{2,6}},{{3,7}}
    }};

    const math::Float4 color{
        0.25F, 0.78F, 1.0F, 0.95F};

    for (const auto& edge : edges)
    {
        lines.push_back({
            .start = p[edge[0]],
            .end = p[edge[1]],
            .color = color
        });
    }

    if (volume->solverPolicy ==
        world_model::
            VolumeSolverPolicy::Local3D)
    {
        const f32 handleSize =
            static_cast<f32>(
                std::max({
                    half.x,
                    half.y,
                    half.z,
                    1.0}) *
                0.035);

        const math::Float4 handleColor{
            1.0F, 0.72F, 0.18F, 1.0F};

        const auto addHandle =
            [&](const math::Float3 point)
            {
                lines.push_back({
                    .start =
                        point +
                        math::Float3{
                            -handleSize,
                            0.0F,
                            0.0F},
                    .end =
                        point +
                        math::Float3{
                            handleSize,
                            0.0F,
                            0.0F},
                    .color = handleColor
                });
                lines.push_back({
                    .start =
                        point +
                        math::Float3{
                            0.0F,
                            -handleSize,
                            0.0F},
                    .end =
                        point +
                        math::Float3{
                            0.0F,
                            handleSize,
                            0.0F},
                    .color = handleColor
                });
                lines.push_back({
                    .start =
                        point +
                        math::Float3{
                            0.0F,
                            0.0F,
                            -handleSize},
                    .end =
                        point +
                        math::Float3{
                            0.0F,
                            0.0F,
                            handleSize},
                    .color = handleColor
                });
            };

        for (const auto& point :
             p)
        {
            addHandle(point);
        }

        const std::array<math::Float3,6>
            faceHandles{
                relative( half.x,0.0,0.0),
                relative(-half.x,0.0,0.0),
                relative(0.0, half.y,0.0),
                relative(0.0,-half.y,0.0),
                relative(0.0,0.0, half.z),
                relative(0.0,0.0,-half.z)
            };

        for (const auto& point :
             faceHandles)
        {
            addHandle(point);
        }
    }

    return lines;
}

[[nodiscard]] std::optional<StudioTerrainAuthoringOverlay>
SelectedTerrainAuthoringOverlay(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime)
{
    if (!session.World().HasWorld())
    {
        return std::nullopt;
    }

    auto& world = session.World();
    const auto& selected = world.Selection().Ordered();

    if (selected.size() != 1U)
    {
        return std::nullopt;
    }

    scene::ObjectId constraintId = selected.front();
    const auto selectedRecord = world.Objects().Find(constraintId);

    if (!selectedRecord.has_value())
    {
        return std::nullopt;
    }

    if (selectedRecord->type ==
        world_model::kBiomeAuthoredMaskType)
    {
        if (!selectedRecord->parent.has_value())
        {
            return std::nullopt;
        }

        editor_model::SurfaceAuthoringModel model(
            world.Objects(),
            world.Commands(),
            world.Selection());

        const auto body =
            model.SelectedRockyBody();

        if (!body.has_value() ||
            body->terrain != runtime.terrainObject)
        {
            return std::nullopt;
        }

        const auto masks =
            model.Masks(
                *selectedRecord->parent);

        const auto found =
            std::find_if(
                masks.begin(),
                masks.end(),
                [&](const editor_model::SurfaceBiomeMaskDetail& item)
                {
                    return item.id ==
                        selectedRecord->id;
                });

        if (found == masks.end())
        {
            return std::nullopt;
        }

        return StudioTerrainAuthoringOverlay{
            .body = runtime.body,
            .kind =
                StudioTerrainOverlayKind::Brush,
            .controlUnitDirections = {
                found->centerUnitDirection
            },
            .influenceRadiusMeters =
                found->outerRadiusMeters
        };
    }

    if (selectedRecord->type ==
        world_model::kTerrainConstraintControlPointType)
    {
        if (!selectedRecord->parent.has_value())
        {
            return std::nullopt;
        }

        constraintId = *selectedRecord->parent;
    }

    const auto constraintRecord =
        world.Objects().Find(constraintId);

    if (!constraintRecord.has_value() ||
        constraintRecord->type !=
            world_model::kTerrainConstraintType)
    {
        return std::nullopt;
    }

    editor_model::SurfaceAuthoringModel model(
        world.Objects(),
        world.Commands(),
        world.Selection());

    const auto body =
        model.SelectedRockyBody();

    if (!body.has_value() ||
        body->terrain != runtime.terrainObject)
    {
        return std::nullopt;
    }

    const auto constraints =
        model.TerrainConstraints(body->terrain);

    const auto found =
        std::find_if(
            constraints.begin(),
            constraints.end(),
            [&](const editor_model::SurfaceTerrainConstraintDetail& item)
            {
                return item.id == constraintId;
            });

    if (found == constraints.end())
    {
        return std::nullopt;
    }

    StudioTerrainAuthoringOverlay overlay{
        .body = runtime.body,
        .kind =
            found->shape ==
                    editor_model::SurfaceTerrainConstraintShape::Spline
                ? StudioTerrainOverlayKind::Spline
                : StudioTerrainOverlayKind::Brush,
        .influenceRadiusMeters =
            found->shape ==
                    editor_model::SurfaceTerrainConstraintShape::Spline
                ? found->halfWidthMeters + found->falloffMeters
                : found->outerRadiusMeters
    };

    if (found->shape ==
        editor_model::SurfaceTerrainConstraintShape::Spline)
    {
        overlay.controlUnitDirections =
            found->controlUnitDirections;
    }
    else
    {
        overlay.controlUnitDirections.push_back(
            found->centerUnitDirection);
    }

    return overlay;
}

[[nodiscard]] std::vector<StudioTerrainAuthoringOverlay>
TerrainConstraintDiagnosticOverlays(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime)
{
    std::vector<StudioTerrainAuthoringOverlay>
        result;

    if (!session.World().HasWorld())
    {
        return result;
    }

    auto& world =
        session.World();

    editor_model::SurfaceAuthoringModel model(
        world.Objects(),
        world.Commands(),
        world.Selection());

    const auto constraints =
        model.TerrainConstraints(
            runtime.terrainObject);

    result.reserve(
        constraints.size());

    for (const auto& constraint :
         constraints)
    {
        StudioTerrainAuthoringOverlay overlay{
            .body = runtime.body,
            .kind =
                constraint.shape ==
                        editor_model::
                            SurfaceTerrainConstraintShape::
                                Spline
                    ? StudioTerrainOverlayKind::Spline
                    : StudioTerrainOverlayKind::Brush,
            .influenceRadiusMeters =
                constraint.shape ==
                        editor_model::
                            SurfaceTerrainConstraintShape::
                                Spline
                    ? constraint.halfWidthMeters +
                        constraint.falloffMeters
                    : constraint.outerRadiusMeters
        };

        if (constraint.shape ==
            editor_model::
                SurfaceTerrainConstraintShape::
                    Spline)
        {
            overlay.controlUnitDirections =
                constraint.controlUnitDirections;
        }
        else
        {
            overlay.controlUnitDirections.
                push_back(
                    constraint.
                        centerUnitDirection);
        }

        if (!overlay.
                controlUnitDirections.
                empty())
        {
            result.push_back(
                std::move(overlay));
        }
    }

    return result;
}
} // namespace orbit::studio_ui::viewport_detail
