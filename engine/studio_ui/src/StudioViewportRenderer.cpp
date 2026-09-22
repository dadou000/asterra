#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/celestial_radiometry/Radiometry.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>
#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>
#include <orbit/world_model/CelestialAtmosphereBinding.hpp>
#include <orbit/world_model/CelestialCloudBinding.hpp>
#include <orbit/world_model/CelestialGiantBinding.hpp>
#include <orbit/world_model/CelestialCompactObjectBinding.hpp>
#include <orbit/world_model/CelestialMagnetosphereBinding.hpp>
#include <orbit/world_model/CelestialSmallBodyBinding.hpp>
#include <orbit/world_model/CelestialOceanBinding.hpp>
#include <orbit/world_model/CelestialRadiometryBinding.hpp>
#include <orbit/world_model/CelestialRingBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/world_model/LocalLightBinding.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/lighting/AnalyticBodyVisibility.hpp>
#include <orbit/lighting/TerrainHeightfieldVisibility.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuPhysicalPageComposite.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] u64 CombineFingerprint(
    const u64 seed,
    const u64 value) noexcept
{
    return
        seed ^
        (value +
         0x9e3779b97f4a7c15ULL +
         (seed << 6U) +
         (seed >> 2U));
}

[[nodiscard]] u64 StableViewportHash(
    const std::string_view value) noexcept
{
    u64 hash =
        1469598103934665603ULL;

    for (const unsigned char ch :
         value)
    {
        hash ^= static_cast<u64>(ch);
        hash *=
            1099511628211ULL;
    }

    return hash;
}

[[nodiscard]]
celestial_scheduler::WorkKey
CelestialWorkKeyFor(
    const std::string_view viewportId,
    const universe::BodyId body,
    const celestial_scheduler::WorkKind kind) noexcept
{
    const u64 viewportHash =
        StableViewportHash(
            viewportId);

    return {
        .subjectHigh =
            body.high ^
            viewportHash,
        .subjectLow =
            body.low ^
            (viewportHash << 1U),
        .kind = kind
    };
}

[[nodiscard]] u64 QuantizedLightingFingerprintValue(
    const f32 value,
    const f32 quantum) noexcept
{
    if (!std::isfinite(value) ||
        quantum <= 0.0F)
    {
        return 0U;
    }

    const i64 quantized =
        static_cast<i64>(
            std::llround(
                static_cast<f64>(value) /
                static_cast<f64>(quantum)));

    return static_cast<u64>(quantized);
}

[[nodiscard]] math::Float3 AtmosphereSkyIrradianceSummary(
    const celestial_atmosphere::AtmosphereSkyView* sky) noexcept
{
    if (sky == nullptr ||
        sky->skyView.texels.empty())
    {
        return {};
    }

    math::Double3 sum{};

    for (const auto& texel : sky->skyView.texels)
    {
        sum.x += std::max(
            static_cast<f64>(texel.x),
            0.0);
        sum.y += std::max(
            static_cast<f64>(texel.y),
            0.0);
        sum.z += std::max(
            static_cast<f64>(texel.z),
            0.0);
    }

    const f64 inverseCount =
        1.0 /
        static_cast<f64>(
            sky->skyView.texels.size());

    // Convert average sky radiance to an approximate hemispherical
    // irradiance and normalize by Orbit's solar reference irradiance.
    constexpr f64 kPi =
        3.14159265358979323846;
    constexpr f64 kReferenceIrradiance =
        1361.0;

    const f64 scale =
        inverseCount *
        kPi /
        kReferenceIrradiance;

    return {
        static_cast<f32>(sum.x * scale),
        static_cast<f32>(sum.y * scale),
        static_cast<f32>(sum.z * scale)
    };
}

[[nodiscard]] bool SameClipmapConfig(
    const terrain_view::ClipmapConfig& a,
    const terrain_view::ClipmapConfig& b) noexcept
{
    return
        a.levelCount == b.levelCount &&
        a.gridResolution == b.gridResolution &&
        a.baseSpacingMeters == b.baseSpacingMeters &&
        a.levelScale == b.levelScale &&
        a.overlapCells == b.overlapCells;
}

[[nodiscard]] math::Double3x3 EulerDegreesToRotation(
    const math::Double3& eulerDegrees) noexcept
{
    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    const f64 rx =
        eulerDegrees.x * kDegreesToRadians;
    const f64 ry =
        eulerDegrees.y * kDegreesToRadians;
    const f64 rz =
        eulerDegrees.z * kDegreesToRadians;

    const f64 cx = std::cos(rx);
    const f64 sx = std::sin(rx);
    const f64 cy = std::cos(ry);
    const f64 sy = std::sin(ry);
    const f64 cz = std::cos(rz);
    const f64 sz = std::sin(rz);

    // Intrinsic XYZ (equivalent parent-space Rz * Ry * Rx).
    return {
        .xAxis = {
            cz * cy,
            sz * cy,
            -sy
        },
        .yAxis = {
            cz * sy * sx - sz * cx,
            sz * sy * sx + cz * cx,
            cy * sx
        },
        .zAxis = {
            cz * sy * cx + sz * sx,
            sz * sy * cx - cz * sx,
            cy * cx
        }
    };
}

[[nodiscard]] std::vector<lighting::VisibilityProxy>
BuildVisibilityProxies(
    const std::vector<
        world_model::ResolvedVisibilityProxy>& resolved,
    const universe::BodyId body,
    const frames::FrameId bodyFrame)
{
    std::vector<lighting::VisibilityProxy> proxies;
    proxies.reserve(resolved.size());

    for (const auto& source : resolved)
    {
        proxies.push_back({
            .stableId =
                source.object.high ^
                source.object.low,
            .body = body,
            .frame = bodyFrame,
            .frameFromProxy = {
                .rotation =
                    EulerDegreesToRotation(
                        source.eulerDegrees),
                .translation =
                    source.positionMeters
            },
            .shape =
                source.shape ==
                        world_model::
                            ResolvedVisibilityProxyShape::
                                Box
                    ? lighting::
                        VisibilityProxyShape::Box
                    : lighting::
                        VisibilityProxyShape::Sphere,
            .sphereRadiusMeters =
                source.radiusMeters,
            .boxHalfExtentsMeters =
                source.halfExtentsMeters,
            .materialId =
                source.materialId,
            .instanceId =
                source.instanceId,
            .nominalErrorMeters =
                source.
                    maximumApproximationErrorMeters,
            .dynamic =
                source.dynamic
        });
    }

    return proxies;
}

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

[[nodiscard]] terrain_render::TerrainPreviewCamera
TerrainCameraFromBodyCamera(
    const render_view::CameraState& camera,
    const world::WorldPosition& observer)
{
    const auto observerDirection =
        math::Normalize(observer.meters);

    const auto frame =
        world::MakeSurfaceFrame(
            observerDirection);

    const math::Double3 forward{
        static_cast<f64>(camera.forward.x),
        static_cast<f64>(camera.forward.y),
        static_cast<f64>(camera.forward.z)
    };

    const math::Double3 up{
        static_cast<f64>(camera.up.x),
        static_cast<f64>(camera.up.y),
        static_cast<f64>(camera.up.z)
    };

    auto localForward =
        math::Float3{
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.east)),
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.up)),
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.north))
        };

    auto localUp =
        math::Float3{
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.east)),
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.up)),
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.north))
        };

    if (math::LengthSquared(localForward) <=
        1.0e-8F)
    {
        localForward = {
            0.0F,
            -0.28F,
            1.0F
        };
    }

    if (math::LengthSquared(localUp) <=
        1.0e-8F)
    {
        localUp = {
            0.0F,
            1.0F,
            0.0F
        };
    }

    return {
        .forward =
            math::Normalize(
                localForward),
        .up =
            math::Normalize(
                localUp)
    };
}

struct StudioPhysicalRenderPages
{
    std::vector<
        terrain_gpu::GpuPhysicalSurfacePage>
        pages;
    u64 generation{
        0x4D31325048595352ULL};
};

[[nodiscard]] StudioPhysicalRenderPages
BuildPhysicalRenderPages(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::AnalyticTerrainSource& analytic,
    rhi::Device& device)
{
    StudioPhysicalRenderPages result{};

    auto* services =
        session.World().
            Surfaces().
            ServicesForBody(
                runtime.body);

    if (services == nullptr)
    {
        return result;
    }

    std::array<
        terrain::PhysicalTerrainPageAddress,
        5U>
        addresses{};

    addresses[0] =
        runtime.observerPhysicalPage;

    constexpr std::array<
        world::TileEdge,
        4U>
        edges{
            world::TileEdge::North,
            world::TileEdge::East,
            world::TileEdge::South,
            world::TileEdge::West
        };

    for (u32 index = 0U;
         index < edges.size();
         ++index)
    {
        const auto neighbor =
            world::NeighborAcrossTileEdge(
                runtime.
                    observerPhysicalPage.
                    tile,
                edges[index]);

        addresses[index + 1U] = {
            .planet =
                runtime.
                    observerPhysicalPage.
                    planet,
            .tile =
                neighbor.tile
        };
    }

    auto& cache =
        services->Cache();

    const f64 seaLevelMeters =
        analytic.
            Description().
            global.
            seaLevelMeters;

    for (const auto& address :
         addresses)
    {
        const auto snapshot =
            session.
                TerrainPhysicalPages().
                Find(
                    address);

        if (snapshot == nullptr ||
            snapshot->material == nullptr)
        {
            continue;
        }

        const auto status =
            session.
                TerrainPhysicalPages().
                PageStatus(
                    address);

        if (!status.has_value() ||
            status->revisionFingerprint !=
                snapshot->
                    revisionFingerprint)
        {
            continue;
        }

        const terrain_gpu::
            PersistentGpuTerrainCacheKey
            key{
                .address =
                    address,
                .physicalLod =
                    snapshot->
                        physicalLod,
                .revisions =
                    snapshot->
                        revisions
            };

        auto cached =
            cache.Find(
                key);

        const auto physicalProduct =
            terrain_gpu::
                ProductBit(
                    terrain_gpu::
                        CachedTerrainProduct::
                            PhysicalSurface);

        if (cached == nullptr ||
            (cached->products &
             physicalProduct) == 0U ||
            cached->physicalSurface ==
                nullptr)
        {
            const auto uploadRevision =
                session.
                    TerrainPhysicalPages().
                    BeginUpload(
                        address);

            if (!uploadRevision.has_value() ||
                *uploadRevision !=
                    snapshot->
                        revisionFingerprint)
            {
                if (uploadRevision.has_value())
                {
                    static_cast<void>(
                        session.
                            TerrainPhysicalPages().
                            CompleteUpload(
                                address,
                                *uploadRevision,
                                false,
                                "M12 physical snapshot changed before GPU upload."));
                }

                continue;
            }

            try
            {
                const u32 resolution =
                    snapshot->material->
                        Resolution();

                std::vector<
                    terrain_gpu::
                        GpuPhysicalSurfaceTexel>
                    texels(
                        static_cast<
                            std::size_t>(
                                resolution) *
                        resolution);

                for (u32 y = 0U;
                     y < resolution;
                     ++y)
                {
                    for (u32 x = 0U;
                         x < resolution;
                         ++x)
                    {
                        const auto& cell =
                            snapshot->
                                material->
                                At(
                                    x,
                                    y);

                        const f32 elevation =
                            cell.
                                SurfaceHeightMeters();

                        texels[
                            static_cast<
                                std::size_t>(
                                    y) *
                                resolution +
                            x] = {
                                .elevationMeters =
                                    elevation,
                                .standingWaterDepthMeters =
                                    static_cast<f32>(
                                        std::max(
                                            seaLevelMeters -
                                                static_cast<f64>(
                                                    elevation),
                                            0.0))
                            };
                    }
                }

                auto uniqueBuffer =
                    device.CreateBuffer({
                        .sizeBytes =
                            static_cast<u64>(
                                texels.size()) *
                            sizeof(
                                terrain_gpu::
                                    GpuPhysicalSurfaceTexel),
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostVisible,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    });

                std::shared_ptr<
                    rhi::Buffer>
                    buffer{
                        std::move(
                            uniqueBuffer)};

                std::byte* mapped =
                    buffer->Map();

                std::memcpy(
                    mapped,
                    texels.data(),
                    texels.size() *
                        sizeof(
                            terrain_gpu::
                                GpuPhysicalSurfaceTexel));

                buffer->Unmap();

                auto augmented =
                    cached != nullptr
                        ? std::make_shared<
                              terrain_gpu::
                                  CachedGpuTerrainPage>(
                                      *cached)
                        : std::make_shared<
                              terrain_gpu::
                                  CachedGpuTerrainPage>();

                augmented->products |=
                    physicalProduct;

                augmented->physicalSurface =
                    std::move(
                        buffer);

                cache.Insert(
                    key,
                    augmented);

                cached =
                    std::move(
                        augmented);

                if (!session.
                        TerrainPhysicalPages().
                        CompleteUpload(
                            address,
                            *uploadRevision,
                            true))
                {
                    static_cast<void>(
                        cache.Erase(
                            key));
                    cached.reset();
                    continue;
                }
            }
            catch (const std::exception& error)
            {
                static_cast<void>(
                    session.
                        TerrainPhysicalPages().
                        CompleteUpload(
                            address,
                            *uploadRevision,
                            false,
                            error.what()));
                continue;
            }
        }

        if ((cached->products &
             physicalProduct) == 0U ||
            cached->physicalSurface ==
                nullptr)
        {
            continue;
        }

        result.pages.push_back({
            .address =
                address,
            .resolution =
                snapshot->
                    material->
                    Resolution(),
            .samples =
                cached->
                    physicalSurface
        });

        result.generation =
            terrain::StableCombine64(
                result.generation,
                terrain_gpu::
                    PersistentGpuTerrainCacheFingerprint(
                        key));

        result.generation =
            terrain::StableCombine64(
                result.generation,
                snapshot->
                    revisionFingerprint);
    }

    return result;
}
[[nodiscard]] f64 ReferenceRadiusForShape(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                return std::max({
                    value.radiiMeters.x,
                    value.radiiMeters.y,
                    value.radiiMeters.z
                });
            }
        },
        shape);
}
struct ResolvedStudioDirectLight
{
    math::Float3 directionBody{
        0.55F, 0.72F, -0.48F};
    f32 irradianceScale{0.0F};
    std::optional<
        world_model::DirectBodyLighting>
        direct;
};

[[nodiscard]] ResolvedStudioDirectLight
ResolveStudioDirectLight(
    studio_session::StudioSession& session,
    const universe::BodyId receiver,
    const time::SimulationTime atTime)
{
    ResolvedStudioDirectLight result;

    auto& universe =
        session.World().
            Universe();

    const auto* receiverBody =
        universe.Bodies().
            FindBody(receiver);

    if (receiverBody == nullptr)
    {
        return result;
    }

    const auto candidates =
        universe.Bodies().
            Bodies(
                receiverBody->system);

    world_model::CelestialLightingService
        lighting(
            session.World().Objects(),
            universe);

    f64 bestIrradiance = -1.0;

    for (const auto emitter :
         candidates)
    {
        if (emitter == receiver)
        {
            continue;
        }

        const auto emitterObject =
            universe.ObjectForBody(
                emitter);

        if (!emitterObject.has_value() ||
            !world_model::
                ResolveRadiativeBody(
                    session.World().
                        Objects(),
                    *emitterObject).
                has_value())
        {
            continue;
        }

        std::vector<universe::BodyId>
            occluders;

        for (const auto other :
             candidates)
        {
            if (other != receiver &&
                other != emitter)
            {
                occluders.push_back(
                    other);
            }
        }

        const auto direct =
            lighting.DirectLightingAtBody(
                receiver,
                emitter,
                occluders,
                atTime);

        if (!direct.has_value() ||
            direct->
                irradianceWattsPerSquareMeter <=
                bestIrradiance)
        {
            continue;
        }

        bestIrradiance =
            direct->
                irradianceWattsPerSquareMeter;

        const auto direction =
            math::Normalize(
                direct->
                    receiverBodyFixedToEmitterMeters);

        result.directionBody = {
            static_cast<f32>(direction.x),
            static_cast<f32>(direction.y),
            static_cast<f32>(direction.z)
        };

        result.irradianceScale =
            static_cast<f32>(
                direct->
                    irradianceWattsPerSquareMeter /
                1361.0);

        result.direct =
            direct;
    }

    return result;
}


[[nodiscard]] std::vector<lighting::ResolvedLocalLight>
ResolveStudioLocalLights(
    studio_session::StudioSession& session,
    const lighting::LightingView& lightingView,
    const std::optional<scene::ObjectId> root)
{
    const auto authored =
        world_model::ResolveAuthoredLocalLights(
            session.World().Objects(),
            root);

    std::vector<lighting::ResolvedLocalLight>
        result;
    result.reserve(
        authored.size());

    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    for (const auto& item : authored)
    {
        const lighting::LocalLight light{
            .type =
                item.kind ==
                        world_model::
                            AuthoredLightKind::Spot
                    ? lighting::
                          LocalLightType::Spot
                    : lighting::
                          LocalLightType::Point,
            .positionInFrameMeters =
                item.positionMeters,
            .direction = {
                static_cast<f32>(
                    item.direction.x),
                static_cast<f32>(
                    item.direction.y),
                static_cast<f32>(
                    item.direction.z)
            },
            .colorLinear = {
                static_cast<f32>(
                    item.colorLinear.x),
                static_cast<f32>(
                    item.colorLinear.y),
                static_cast<f32>(
                    item.colorLinear.z)
            },
            .luminousFluxLumens =
                static_cast<f32>(
                    item.luminousFluxLumens),
            .rangeMeters =
                static_cast<f32>(
                    item.rangeMeters),
            .innerConeRadians =
                static_cast<f32>(
                    item.innerConeDegrees *
                    kDegreesToRadians),
            .outerConeRadians =
                static_cast<f32>(
                    item.outerConeDegrees *
                    kDegreesToRadians),
            .stableId =
                item.object.high ^
                item.object.low
        };

        result.push_back(
            lighting::ResolveLocalLight(
                light,
                lightingView));
    }

    return result;
}

[[nodiscard]] std::vector<scene::ObjectId>
CollectVolumeObjects(
    const scene::ObjectStore& objects)
{
    std::vector<scene::ObjectId>
        result;
    std::vector<scene::ObjectRecord>
        pending =
            objects.Roots();

    while (!pending.empty())
    {
        const auto current =
            pending.back();
        pending.pop_back();

        if (current.type ==
            world_model::kVolumeType)
        {
            result.push_back(
                current.id);
        }

        const auto children =
            objects.Children(
                current.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return result;
}

[[nodiscard]] std::vector<lighting::EmissiveVolumeSource>
ResolveAuthoredEmissiveVolumes(
    const scene::ObjectStore& objects)
{
    std::vector<lighting::EmissiveVolumeSource>
        result;

    constexpr u64 kEmissionBit =
        static_cast<u64>(
            world_model::
                VolumeField::Emission);

    for (const auto volumeId :
         CollectVolumeObjects(objects))
    {
        const auto domain =
            world_model::ResolveVolumeDomain(
                objects,
                volumeId);

        if (!domain.has_value() ||
            !domain->enabled ||
            !domain->renderEnabled ||
            (domain->fieldMask &
             kEmissionBit) == 0U ||
            domain->emissionScale <= 0.0F ||
            domain->giEmissionScale <= 0.0F)
        {
            continue;
        }

        f64 authoredEmission = 0.0;

        for (const auto& input :
             world_model::ResolveVolumeInputs(
                 objects,
                 volumeId))
        {
            if (!input.enabled ||
                input.role !=
                    world_model::
                        VolumeInputRole::Source ||
                (input.fieldMask &
                 kEmissionBit) == 0U)
            {
                continue;
            }

            authoredEmission +=
                std::max(
                    input.scalarValue,
                    0.0);
        }

        if (authoredEmission <= 0.0 &&
            domain->preset == "Fire")
        {
            authoredEmission = 1.0;
        }

        if (authoredEmission <= 0.0)
        {
            continue;
        }

        const f64 radius =
            std::sqrt(
                domain->halfExtentsMeters.x *
                    domain->halfExtentsMeters.x +
                domain->halfExtentsMeters.y *
                    domain->halfExtentsMeters.y +
                domain->halfExtentsMeters.z *
                    domain->halfExtentsMeters.z);

        result.push_back({
            .centerInFrameMeters =
                domain->centerMeters,
            .radiusMeters =
                static_cast<f32>(
                    std::max(
                        radius,
                        0.05)),
            .emissionLinear =
                domain->emissionColor,
            .intensityScale =
                static_cast<f32>(
                    authoredEmission) *
                domain->emissionScale *
                domain->giEmissionScale,
            .influenceRangeMeters =
                static_cast<f32>(
                    std::max(
                        radius * 12.0,
                        1.0)),
            .stableId =
                volumeId.high ^
                volumeId.low
        });
    }

    return result;
}

[[nodiscard]] editor_ui::PreviewMaterial
ResolveRuntimeMaterialAsset(
    content::ContentService& content,
    const content::AssetRecord* asset,
    const u32 depth = 0U)
{
    editor_ui::PreviewMaterial result{};

    if (asset == nullptr || depth > 8U)
    {
        return result;
    }

    if (asset->kind ==
            content::AssetKind::Material &&
        asset->material.has_value())
    {
        result.roughness =
            static_cast<f32>(
                std::clamp(
                    asset->material->
                        roughnessFactor,
                    0.0,
                    1.0));
        result.metallic =
            static_cast<f32>(
                std::clamp(
                    asset->material->
                        metallicFactor,
                    0.0,
                    1.0));
    }
    else if (
        asset->kind ==
            content::AssetKind::MaterialInstance &&
        asset->materialInstance.has_value())
    {
        const auto parentPath =
            asset->sourcePath.parent_path() /
            asset->materialInstance->parent;

        result =
            ResolveRuntimeMaterialAsset(
                content,
                content.FindByPath(
                    parentPath),
                depth + 1U);

        if (asset->materialInstance->
                roughnessFactor.has_value())
        {
            result.roughness =
                static_cast<f32>(
                    std::clamp(
                        *asset->materialInstance->
                            roughnessFactor,
                        0.0,
                        1.0));
        }

        if (asset->materialInstance->
                metallicFactor.has_value())
        {
            result.metallic =
                static_cast<f32>(
                    std::clamp(
                        *asset->materialInstance->
                            metallicFactor,
                        0.0,
                        1.0));
        }
    }
    else
    {
        return result;
    }

    const auto emission =
        content.ResolveMaterialEmission(
            asset->id);

    const auto evaluated =
        lighting::EvaluateMaterialEmission({
            .colorLinear = {
                static_cast<f32>(
                    emission.colorLinear[0]),
                static_cast<f32>(
                    emission.colorLinear[1]),
                static_cast<f32>(
                    emission.colorLinear[2])
            },
            .luminanceNits =
                static_cast<f32>(
                    emission.luminanceNits),
            .contributesToGi =
                emission.contributesToGi,
            .giScale =
                static_cast<f32>(
                    emission.giScale)
        });

    result.emissionRadiance =
        evaluated.visibleRadiance;
    result.emissionGiScale =
        emission.contributesToGi
            ? static_cast<f32>(
                  std::max(
                      emission.giScale,
                      0.0))
            : 0.0F;

    return result;
}

[[nodiscard]] std::optional<editor_ui::PreviewMaterial>
ResolveRuntimeBodyMaterialIfAssigned(
    content::ContentService* content,
    scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> bodyObject)
{
    if (content == nullptr ||
        !bodyObject.has_value())
    {
        return std::nullopt;
    }

    const auto property =
        objects.GetProperty(
            *bodyObject,
            world_model::kBodyMaterialAsset);

    if (!property.has_value())
    {
        return std::nullopt;
    }

    const auto* textValue =
        std::get_if<std::string>(
            &*property);

    if (textValue == nullptr ||
        textValue->empty())
    {
        return std::nullopt;
    }

    const auto assetId =
        content::AssetId::Parse(
            *textValue);

    if (!assetId.has_value())
    {
        return std::nullopt;
    }

    const auto* asset =
        content->Find(*assetId);

    if (asset == nullptr ||
        (asset->kind != content::AssetKind::Material &&
         asset->kind != content::AssetKind::MaterialInstance))
    {
        return std::nullopt;
    }

    return ResolveRuntimeMaterialAsset(
        *content,
        asset);
}

[[nodiscard]] editor_ui::PreviewMaterial
ResolveRuntimeBodyMaterial(
    content::ContentService* content,
    scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> bodyObject)
{
    if (content == nullptr ||
        !bodyObject.has_value())
    {
        return {};
    }

    const auto property =
        objects.GetProperty(
            *bodyObject,
            world_model::kBodyMaterialAsset);

    if (!property.has_value())
    {
        return {};
    }

    const auto* textValue =
        std::get_if<std::string>(
            &*property);

    if (textValue == nullptr ||
        textValue->empty())
    {
        return {};
    }

    const auto assetId =
        content::AssetId::Parse(
            *textValue);

    if (!assetId.has_value())
    {
        return {};
    }

    return ResolveRuntimeMaterialAsset(
        *content,
        content->Find(*assetId));
}


} // namespace

StudioViewportRenderer::StudioViewportRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
    : device_(&device),
      compiler_(&compiler),
      framesInFlight_(framesInFlight),
      bodyRenderer_(device, compiler),
      macroGlobeRenderer_(device, compiler),
      farBodyRenderer_(device, compiler),
      ringRenderer_(device, compiler),
      auroraRenderer_(device, compiler),
      compactObjectRenderer_(device, compiler),
      pathRenderer_(device, compiler),
      surfaceVolumeDebugRenderer_(device, compiler),
      universalVolumeRenderer_(device, compiler),
      debugComposite_(device, compiler),
      directLightingRenderer_(device, compiler),
      materialEmissionSurfaceOverride_(device, compiler),
      finalGatherRenderer_(device, compiler),
      radianceCacheSampler_(device, compiler),
      hybridReflectionRenderer_(device, compiler),
      exactReflectionQueryRenderer_(device, compiler),
      surfaceDebugRenderer_(device, compiler),
      luminanceHistogramRenderer_(device, compiler),
      highlightEffectsRenderer_(device, compiler),
      displayResolveRenderer_(device, compiler),
      colorLutRenderer_(device, compiler),
      outputTransformRenderer_(device, compiler),
      colorLut_(
          std::make_unique<
              post_process::GpuColorLut>(
                  device,
                  post_process::
                      BuildIdentityColorLut()))
{
    if (framesInFlight_ == 0U)
    {
        throw std::invalid_argument(
            "Studio terrain rendering requires at least one frame-in-flight slot.");
    }
}

void StudioViewportRenderer::SetContentService(
    content::ContentService* const content) noexcept
{
    content_ = content;
}

void StudioViewportRenderer::SetVolumeFieldStorageService(
    volume_fields::VolumeFieldStorageService* const fields) noexcept
{
    volumeFields_ = fields;
}

void StudioViewportRenderer::SetSurfaceVolumeSolverService(
    volume_solver::SurfaceVolumeSolverService* const solver) noexcept
{
    surfaceVolumeSolver_ = solver;
}

volume_render::VolumeRenderRuntimeSettings&
StudioViewportRenderer::VolumeRenderSettings(
    const scene::ObjectId volume)
{
    return
        universalVolumeRenderer_.
            Settings(volume);
}

volume_render::VolumeRenderDiagnostics
StudioViewportRenderer::VolumeRenderDiagnostics(
    const scene::ObjectId volume) const noexcept
{
    return
        universalVolumeRenderer_.
            Diagnostics(volume);
}

void StudioViewportRenderer::SetVolumeSourceDebugVisualization(
    const bool enabled) noexcept
{
    volumeSourceDebugVisualization_ =
        enabled;
}

bool StudioViewportRenderer::VolumeSourceDebugVisualization() const noexcept
{
    return volumeSourceDebugVisualization_;
}

void StudioViewportRenderer::SetColorLut(
    post_process::ColorLutData lut)
{
    if (device_ == nullptr)
    {
        throw std::logic_error(
            "Studio viewport renderer has no device for LUT replacement.");
    }

    if (!post_process::
            IsDisplayLutCompatible(
                lut))
    {
        throw std::invalid_argument(
            "Selected LUT is not compatible with the display-linear correction stage.");
    }

    colorLut_ =
        std::make_unique<
            post_process::GpuColorLut>(
                *device_,
                lut);
    colorLutData_ =
        std::move(lut);
    colorLutSourcePath_ =
        colorLutData_.metadata.title ==
                "Orbit Identity"
            ? "<identity>"
            : "<runtime>";
    colorLutDiagnostic_.clear();
}

std::vector<std::string>
StudioViewportRenderer::ColorLutAssetPaths() const
{
    std::vector<std::string> result;

    if (content_ == nullptr)
    {
        return result;
    }

    for (const auto& asset :
         content_->Search(
             {},
             content::AssetKind::ColorLut))
    {
        result.push_back(
            asset.sourcePath.
                generic_string());
    }

    return result;
}

bool StudioViewportRenderer::SelectColorLutAsset(
    const std::string_view projectRelativePath)
{
    colorLutDiagnostic_.clear();

    if (content_ == nullptr)
    {
        colorLutDiagnostic_ =
            "Content service is unavailable.";
        return false;
    }

    const auto* asset =
        content_->FindByPath(
            std::filesystem::path(
                projectRelativePath));

    if (asset == nullptr ||
        asset->kind !=
            content::AssetKind::ColorLut)
    {
        colorLutDiagnostic_ =
            "Selected path is not an indexed .cube LUT asset.";
        return false;
    }

    try
    {
        std::ifstream stream{
            content_->AbsolutePath(
                asset->id),
            std::ios::binary};

        if (!stream)
        {
            throw std::runtime_error(
                "Unable to open LUT asset.");
        }

        std::ostringstream text;
        text << stream.rdbuf();

        auto imported =
            post_process::
                ParseCubeColorLut(
                    text.str());

        if (!post_process::
                IsDisplayLutCompatible(
                    imported.lut))
        {
            colorLutDiagnostic_ =
                "Rejected LUT: Display correction accepts only DisplayLinear / None-shaper assets. Imported metadata is " +
                std::string(
                    post_process::
                        ColorLutDomainName(
                            imported.lut.metadata.domain)) +
                " / " +
                std::string(
                    post_process::
                        ColorLutShaperName(
                            imported.lut.metadata.shaper)) +
                ".";
            return false;
        }

        SetColorLut(
            std::move(imported.lut));
        colorLutSourcePath_ =
            asset->sourcePath.
                generic_string();
        return true;
    }
    catch (const std::exception& exception)
    {
        colorLutDiagnostic_ =
            exception.what();
        return false;
    }
}

bool StudioViewportRenderer::ImportColorLutFile(
    const std::string_view sourcePath)
{
    colorLutDiagnostic_.clear();

    if (content_ == nullptr)
    {
        colorLutDiagnostic_ =
            "Content service is unavailable.";
        return false;
    }

    try
    {
        const auto importedId =
            content_->ImportFile(
                std::filesystem::path(
                    sourcePath));

        const auto* asset =
            content_->Find(
                importedId);

        if (asset == nullptr ||
            asset->kind !=
                content::AssetKind::ColorLut)
        {
            colorLutDiagnostic_ =
                "Imported file is not a supported .cube LUT.";
            return false;
        }

        return SelectColorLutAsset(
            asset->sourcePath.
                generic_string());
    }
    catch (const std::exception& exception)
    {
        colorLutDiagnostic_ =
            exception.what();
        return false;
    }
}

StudioColorLutDiagnostics
StudioViewportRenderer::ColorLutDiagnostics() const
{
    return {
        .sourcePath =
            colorLutSourcePath_,
        .title =
            colorLutData_.metadata.title,
        .size =
            colorLutData_.size,
        .domain =
            colorLutData_.metadata.domain,
        .shaper =
            colorLutData_.metadata.shaper,
        .compatible =
            post_process::
                IsDisplayLutCompatible(
                    colorLutData_),
        .explicitMetadata =
            colorLutData_.metadata.
                explicitOrbitMetadata,
        .diagnostic =
            colorLutDiagnostic_
    };
}

post_process::ColorLutSettings
StudioViewportRenderer::ColorLutSettings() const noexcept
{
    return colorLutSettings_;
}

void StudioViewportRenderer::SetColorLutSettings(
    post_process::ColorLutSettings settings) noexcept
{
    settings.strength =
        std::clamp(
            settings.strength,
            0.0F,
            1.0F);
    colorLutSettings_ = settings;
}

StudioOutputTransformDiagnostics
StudioViewportRenderer::OutputTransformDiagnostics() const noexcept
{
    return {
        .settings =
            outputTransformSettings_,
        .capabilities =
            outputDisplayCapabilities_,
        .resolved =
            post_process::
                ResolveOutputTransform(
                    outputTransformSettings_,
                    outputDisplayCapabilities_)
    };
}

void StudioViewportRenderer::SetOutputTransformSettings(
    post_process::OutputTransformSettings settings) noexcept
{
    settings.referenceWhiteNits =
        std::max(
            settings.referenceWhiteNits,
            1.0F);
    settings.requestedPeakNits =
        std::max(
            settings.requestedPeakNits,
            settings.referenceWhiteNits);

    outputTransformSettings_ =
        settings;
}

void StudioViewportRenderer::SetOutputDisplayCapabilities(
    post_process::OutputDisplayCapabilities capabilities) noexcept
{
    capabilities.reportedPeakNits =
        std::max(
            capabilities.reportedPeakNits,
            0.0F);

    outputDisplayCapabilities_ =
        capabilities;
}

void StudioViewportRenderer::SetDisplayResolveSettings(
    post_process::DisplayResolveSettings settings) noexcept
{
    settings.exposureScale =
        std::max(
            settings.exposureScale,
            0.0F);
    displayResolveSettings_ = settings;
}

celestial_globe::GpuMacroGlobeProduct*
StudioViewportRenderer::EnsureMacroGlobePresentation(
    const std::string_view viewportId,
    studio_session::StudioSession& session,
    const universe::BodyId body,
    const universe::BodyShape& shape,
    const terrain::TerrainSource& terrainSource,
    const std::function<bool(u64)>& acquireGrant,
    const std::function<void(u64)>& completeGrant)
{
    if (device_ == nullptr)
    {
        throw std::logic_error(
            "Macro-globe presentation requires a render device.");
    }

    auto& presentation =
        macroGlobePresentations_[
            std::string(viewportId)];

    const celestial_globe::MacroGlobeConfig
        globeConfig{
            .faceResolution = 33U,
            .footprintScale = 1.5
        };

    const u64 geometryFingerprint =
        celestial_globe::
            MacroGlobeFingerprint(
                terrainSource,
                shape,
                globeConfig);

    const u64 sourceRevision =
        terrainSource.Revision();

    const auto sphericalPlanet =
        session.World().
            Surfaces().
            Registry().
            SphericalPlanetDefinition(
                body);

    if (!sphericalPlanet.has_value())
    {
        throw std::logic_error(
            "Planetary appearance currently requires the spherical terrain body contract.");
    }

    const celestial_appearance::
        AppearanceConfig
        appearanceConfig{
            .faceResolution =
                globeConfig.faceResolution,
            .footprintScale =
                globeConfig.footprintScale
        };

    const u64 appearanceFingerprint =
        celestial_appearance::
            PlanetaryAppearanceFingerprint(
                terrainSource,
                sphericalPlanet->
                    radiusMeters,
                appearanceConfig);

    const auto bodyObject =
        session.World().
            Universe().
            ObjectForBody(body);

    const auto resolvedOcean =
        bodyObject.has_value()
            ? world_model::
                  ResolveOceanBody(
                      session.World().
                          Objects(),
                      *bodyObject)
            : std::nullopt;

    const u64 activeOceanFingerprint =
        resolvedOcean.has_value()
            ? celestial_ocean::
                  OceanOpticalFingerprint(
                      resolvedOcean->optical)
            : 0U;

    const auto cloudFound =
        cloudPresentations_.find(
            viewportId);

    const celestial_clouds::CloudFieldProduct*
        activeCloudField =
            cloudFound !=
                    cloudPresentations_.end() &&
                cloudFound->second.field !=
                    nullptr
                ? cloudFound->second.field.get()
                : nullptr;

    const u64 activeCloudFingerprint =
        activeCloudField != nullptr
            ? activeCloudField->fingerprint
            : 0U;

    u64 derivedRevision =
        CombineFingerprint(
            geometryFingerprint,
            appearanceFingerprint);
    derivedRevision =
        CombineFingerprint(
            derivedRevision,
            sourceRevision);
    derivedRevision =
        CombineFingerprint(
            derivedRevision,
            activeOceanFingerprint);
    derivedRevision =
        CombineFingerprint(
            derivedRevision,
            activeCloudFingerprint);

    const bool recreate =
        presentation.product == nullptr ||
        presentation.appearanceProduct ==
            nullptr ||
        presentation.cachedDisc ==
            nullptr ||
        presentation.body != body ||
        presentation.sourceRevision !=
            sourceRevision ||
        presentation.fingerprint !=
            geometryFingerprint ||
        presentation.baseAppearanceFingerprint !=
            appearanceFingerprint ||
        presentation.oceanFingerprint !=
            activeOceanFingerprint ||
        presentation.cloudFingerprint !=
            activeCloudFingerprint;

    if (recreate &&
        acquireGrant(derivedRevision))
    {
        const auto mesh =
            celestial_globe::
                BuildMacroGlobe(
                    terrainSource,
                    shape,
                    globeConfig);

        auto appearance =
            celestial_appearance::
                BuildPlanetaryAppearance(
                    terrainSource,
                    sphericalPlanet->
                        radiusMeters,
                    appearanceConfig);

        if (resolvedOcean.has_value())
        {
            celestial_ocean::
                ApplyOrbitalOceanAppearance(
                    appearance,
                    resolvedOcean->optical);
        }

        if (activeCloudField != nullptr)
        {
            celestial_clouds::
                CompositeOrbitalCloudAppearance(
                    *activeCloudField,
                    appearance);
        }

        presentation.appearanceSummary =
            celestial_far_render::
                SummarizeAppearance(
                    appearance);

        const auto cachedDisc =
            celestial_far_render::
                BuildCachedDisc(
                    appearance,
                    {.resolution = 64U});

        presentation.cachedDisc =
            std::make_unique<
                celestial_far_render::
                    GpuCachedDiscProduct>(
                        *device_,
                        cachedDisc);

        presentation.cachedDiscFingerprint =
            cachedDisc.fingerprint;

        presentation.appearanceProduct =
            std::make_unique<
                celestial_appearance::
                    GpuPlanetaryAppearanceProduct>(
                        *device_,
                        appearance);

        presentation.product =
            std::make_unique<
                celestial_globe::
                    GpuMacroGlobeProduct>(
                        *device_,
                        mesh,
                        &appearance);

        presentation.body =
            body;
        presentation.sourceRevision =
            sourceRevision;
        presentation.fingerprint =
            geometryFingerprint;
        presentation.baseAppearanceFingerprint =
            appearanceFingerprint;
        presentation.appearanceFingerprint =
            appearance.fingerprint;
        presentation.oceanFingerprint =
            activeOceanFingerprint;
        presentation.cloudFingerprint =
            activeCloudFingerprint;
        presentation.appearanceTexels =
            static_cast<u32>(
                appearance.texels.size());

        completeGrant(
            derivedRevision);
    }

    if (resolvedOcean.has_value())
    {
        oceanDiagnostics_.insert_or_assign(
            std::string(viewportId),
            StudioOceanDiagnostics{
                .body = body,
                .opticalFingerprint =
                    activeOceanFingerprint,
                .refractiveIndex =
                    resolvedOcean->
                        optical.refractiveIndex,
                .orbitalRoughness =
                    resolvedOcean->
                        optical.orbitalRoughness,
                .glintStrength =
                    resolvedOcean->
                        optical.glintStrength,
                .oceanFraction =
                    presentation.
                        appearanceSummary.
                        oceanFraction
            });
    }
    else
    {
        oceanDiagnostics_.erase(
            viewportId);
    }

    return presentation.product.get();
}

std::optional<StudioMacroGlobeDiagnostics>
StudioViewportRenderer::MacroGlobeDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        macroGlobePresentations_.find(
            viewportId);

    if (found ==
        macroGlobePresentations_.end())
    {
        return std::nullopt;
    }

    const auto& presentation =
        found->second;

    return StudioMacroGlobeDiagnostics{
        .body = presentation.body,
        .terrainRevision =
            presentation.sourceRevision,
        .geometryFingerprint =
            presentation.fingerprint,
        .appearanceFingerprint =
            presentation.appearanceFingerprint,
        .appearanceTexels =
            presentation.appearanceTexels
    };
}

std::optional<
    StudioSurfaceGlobeTransitionDiagnostics>
StudioViewportRenderer::
SurfaceGlobeTransitionDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        transitionDiagnostics_.find(
            viewportId);

    return found ==
            transitionDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioLuminanceHistogramDiagnostics>
StudioViewportRenderer::
LuminanceHistogramDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    return found ==
            luminanceHistogramPresentations_.end()
        ? std::nullopt
        : std::optional(
              found->second.diagnostics);
}

rhi::Texture*
StudioViewportRenderer::LuminanceMeteringMask(
    const std::string_view viewportId) noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    return found ==
                luminanceHistogramPresentations_.end()
            ? nullptr
            : found->second.meteringMask.get();
}

void StudioViewportRenderer::SetLuminanceHistogramConfig(
    const std::string_view viewportId,
    post_process::LuminanceHistogramConfig config)
{
    if (!std::isfinite(config.minimumLog2) ||
        !std::isfinite(config.maximumLog2))
    {
        throw std::invalid_argument(
            "Luminance histogram log range must be finite.");
    }

    if (config.maximumLog2 <=
        config.minimumLog2 + 0.01F)
    {
        config.maximumLog2 =
            config.minimumLog2 + 0.01F;
    }

    config.centerWeightStrength =
        std::clamp(
            config.centerWeightStrength,
            0.0F,
            1.0F);
    config.centerWeightRadius =
        std::max(
            config.centerWeightRadius,
            0.05F);

    luminanceHistogramPresentations_[
        std::string(viewportId)].
        diagnostics.config =
            config;
}

void StudioViewportRenderer::SetHumanEyeAdaptationConfig(
    const std::string_view viewportId,
    post_process::HumanEyeAdaptationConfig config)
{
    auto& diagnostics =
        luminanceHistogramPresentations_[
            std::string(viewportId)].
            diagnostics;

    diagnostics.eyeConfig =
        config;
}

void StudioViewportRenderer::ResetHumanEyeAdaptation(
    const std::string_view viewportId) noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    if (found ==
        luminanceHistogramPresentations_.end())
    {
        return;
    }

    post_process::ResetHumanEyeAdaptation(
        found->second.diagnostics.eyeState);
    found->second.hasEyeUpdateTime = false;
}

void StudioViewportRenderer::SetHighlightEffectsConfig(
    const std::string_view viewportId,
    post_process::HighlightEffectsConfig config)
{
    config.bloomThreshold =
        std::max(config.bloomThreshold, 0.0F);
    config.bloomKnee =
        std::max(config.bloomKnee, 1.0e-5F);
    config.bloomStrength =
        std::max(config.bloomStrength, 0.0F);
    config.bloomRadiusPixels =
        std::max(config.bloomRadiusPixels, 0.5F);

    config.glareThreshold =
        std::max(config.glareThreshold, 0.0F);
    config.glareStrength =
        std::max(config.glareStrength, 0.0F);
    config.glareRadiusPixels =
        std::max(config.glareRadiusPixels, 1.0F);

    config.flareThreshold =
        std::max(config.flareThreshold, 0.0F);
    config.flareStrength =
        std::max(config.flareStrength, 0.0F);
    config.flareCompactness =
        std::max(config.flareCompactness, 1.0F);
    config.flareGhostScale =
        std::max(config.flareGhostScale, 0.0F);

    luminanceHistogramPresentations_[
        std::string(viewportId)].
        diagnostics.highlightConfig =
            config;
}

void StudioViewportRenderer::SetToneMappingConfig(
    const std::string_view viewportId,
    post_process::ToneMappingConfig config)
{
    config.referenceWhiteNits =
        std::max(
            config.referenceWhiteNits,
            1.0e-3F);
    config.peakNits =
        std::max(
            config.peakNits,
            config.referenceWhiteNits);
    config.shoulderStart =
        std::max(
            config.shoulderStart,
            0.0F);
    config.shoulderStrength =
        std::max(
            config.shoulderStrength,
            1.0e-3F);

    luminanceHistogramPresentations_[
        std::string(viewportId)].
        diagnostics.toneMapping =
            config;
}

void StudioViewportRenderer::SetLuminanceMeteringOverlay(
    const std::string_view viewportId,
    const bool enabled)
{
    luminanceHistogramPresentations_[
        std::string(viewportId)].
        showMeteringOverlay =
            enabled;
}

bool StudioViewportRenderer::LuminanceMeteringOverlay(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        luminanceHistogramPresentations_.find(
            viewportId);

    return
        found !=
            luminanceHistogramPresentations_.end() &&
        found->second.showMeteringOverlay;
}


std::optional<
    StudioVisibilityProxyDiagnostics>
StudioViewportRenderer::
VisibilityProxyDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        visibilityProxyDiagnostics_.find(
            viewportId);

    return found ==
            visibilityProxyDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioEmissiveGiDiagnostics>
StudioViewportRenderer::
EmissiveGiDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        emissiveGiDiagnostics_.find(
            viewportId);

    return found ==
            emissiveGiDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioCelestialLightingDiagnostics>
StudioViewportRenderer::
CelestialLightingDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        lightingDiagnostics_.find(
            viewportId);

    return found ==
            lightingDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioAtmosphereDiagnostics>
StudioViewportRenderer::AtmosphereDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        atmosphereDiagnostics_.find(
            viewportId);

    return found ==
            atmosphereDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioCloudDiagnostics>
StudioViewportRenderer::CloudDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        cloudDiagnostics_.find(
            viewportId);

    return found ==
            cloudDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioOceanDiagnostics>
StudioViewportRenderer::OceanDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        oceanDiagnostics_.find(
            viewportId);

    return found ==
            oceanDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioRingDiagnostics>
StudioViewportRenderer::RingDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        ringDiagnostics_.find(
            viewportId);

    return found ==
            ringDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioMagnetosphereDiagnostics>
StudioViewportRenderer::MagnetosphereDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        magnetosphereDiagnostics_.find(
            viewportId);

    return found ==
            magnetosphereDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioCompactObjectDiagnostics>
StudioViewportRenderer::CompactObjectDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        compactObjectDiagnostics_.find(
            viewportId);

    return found ==
            compactObjectDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

celestial_scheduler::SchedulerFrameStats
StudioViewportRenderer::CelestialSchedulerStats() const noexcept
{
    return celestialScheduler_.Stats();
}

celestial_scheduler::SchedulerBudget
StudioViewportRenderer::CelestialSchedulerBudget() const noexcept
{
    return celestialScheduler_.Budget();
}

void StudioViewportRenderer::SetCelestialQualityPolicy(
    celestial_representation::QualityPolicy policy) noexcept
{
    policy.productionSurfaceErrorPixels =
        std::max(
            policy.productionSurfaceErrorPixels,
            1.0e-4);
    policy.macroDisplacementErrorPixels =
        std::max(
            policy.macroDisplacementErrorPixels,
            1.0e-4);
    policy.smoothGlobeMinimumRadiusPixels =
        std::max(
            policy.smoothGlobeMinimumRadiusPixels,
            1.0e-4);
    policy.discImpostorMinimumRadiusPixels =
        std::max(
            policy.discImpostorMinimumRadiusPixels,
            1.0e-4);
    policy.qualityScale =
        std::clamp(
            policy.qualityScale,
            0.1,
            8.0);
    policy.hysteresisFraction =
        std::clamp(
            policy.hysteresisFraction,
            0.0,
            0.49);

    celestialQualityPolicy_ =
        policy;
}

celestial_representation::QualityPolicy
StudioViewportRenderer::CelestialQualityPolicy() const noexcept
{
    return celestialQualityPolicy_;
}

std::optional<
    StudioStellarDiagnostics>
StudioViewportRenderer::StellarDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        stellarDiagnostics_.find(
            viewportId);

    return found ==
            stellarDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioGiantDiagnostics>
StudioViewportRenderer::GiantDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        giantDiagnostics_.find(
            viewportId);

    return found ==
            giantDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<
    StudioSmallBodyDiagnostics>
StudioViewportRenderer::SmallBodyDiagnostics(
    const std::string_view viewportId) const noexcept
{
    const auto found =
        smallBodyDiagnostics_.find(
            viewportId);

    return found ==
            smallBodyDiagnostics_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::vector<StudioRenderedView>
StudioViewportRenderer::Compose(
    render_graph::RenderGraph& graph,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    studio_session::StudioRuntimeBinding& runtime,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug,
    const u32 frameIndex,
    const lighting::LightingWorkPlan& lightingPlan,
    lighting::LightingTimestampRecorder* const lightingTimestamps)
{
    static_cast<void>(views.Refresh(snapshot));

    const universe::BodyRegistry* bodies = nullptr;
    const frames::FrameGraph* frames = nullptr;
    std::vector<const path_geometry::PathDerivedProduct*> products;

    if (snapshot.hasWorld)
    {
        bodies = &runtime.Bodies(snapshot);
        frames = &runtime.Frames(snapshot);
        products = runtime.PathProducts(snapshot).Products();
    }

    const auto celestialFrameGrants =
        celestialScheduler_.
            BuildFramePlan();

    std::vector<bool>
        celestialGrantConsumed(
            celestialFrameGrants.size(),
            false);

    const auto acquireCelestialGrant =
        [&](const std::string_view viewportId,
            const universe::BodyId body,
            const celestial_scheduler::WorkKind kind,
            const u64 authorityRevision,
            const celestial_scheduler::WorkBackend backend,
            const u32 costUnits,
            const i32 priority,
            const bool visible)
        {
            const auto key =
                CelestialWorkKeyFor(
                    viewportId,
                    body,
                    kind);

            for (std::size_t index = 0U;
                 index <
                     celestialFrameGrants.size();
                 ++index)
            {
                if (celestialGrantConsumed[index])
                {
                    continue;
                }

                const auto& grant =
                    celestialFrameGrants[index];

                if (grant.key == key &&
                    grant.authorityRevision ==
                        authorityRevision)
                {
                    celestialGrantConsumed[index] =
                        true;
                    return true;
                }
            }

            celestialScheduler_.Enqueue({
                .key = key,
                .authorityRevision =
                    authorityRevision,
                .backend = backend,
                .costUnits =
                    std::max(
                        costUnits,
                        1U),
                .priority = priority,
                .visible = visible
            });

            return false;
        };

    const auto completeCelestialGrant =
        [&](const std::string_view viewportId,
            const universe::BodyId body,
            const celestial_scheduler::WorkKind kind,
            const u64 authorityRevision)
        {
            return
                celestialScheduler_.Complete(
                    CelestialWorkKeyFor(
                        viewportId,
                        body,
                        kind),
                    authorityRevision);
        };

    std::vector<StudioRenderedView> rendered;
    const auto catalog = views.Catalog();
    rendered.reserve(catalog.size());

    for (const auto& info : catalog)
    {
        auto* view = views.Find(info.id);

        if (view == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView catalog contains a missing view.");
        }

        const std::string prefix =
            "StudioViewport." + info.id;
        const auto targets =
            view->Import(
                graph,
                prefix.c_str());

        const auto* logicalTarget =
            session.Viewports().Find(info.id);

        if (logicalTarget == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView has no logical viewport target.");
        }

        // Per-view diagnostics are rebuilt from the current target every frame.
        stellarDiagnostics_.erase(
            info.id);
        smallBodyDiagnostics_.erase(
            info.id);
        compactObjectDiagnostics_.erase(
            info.id);

        std::optional<universe::BodyShape> shape;

        if (logicalTarget->target.has_value())
        {
            if (!snapshot.hasWorld || bodies == nullptr ||
                logicalTarget->target->universeGeneration !=
                    snapshot.universeGeneration)
            {
                throw std::logic_error(
                    "Studio viewport render target is stale for the current universe generation.");
            }

            const auto* body =
                bodies->FindBody(
                    logicalTarget->target->body);

            if (body == nullptr)
            {
                throw std::logic_error(
                    "Studio viewport target body is missing from the current BodyRegistry.");
            }

            shape = body->shape;
        }

        {
            const auto previousLightingView =
                view->Lighting();

            lighting::LightingView currentLightingView =
                previousLightingView;

            currentLightingView.frame =
                view->Camera().frame;
            currentLightingView.body =
                logicalTarget->target.has_value()
                    ? logicalTarget->target->body
                    : universe::BodyId{};
            currentLightingView.cameraPositionInFrameMeters =
                view->Camera().localPositionMeters;

            // Orbit's current renderers are camera-relative. Moving this
            // presentation origin with the camera must never alter stable GI
            // cache identity; gpuOriginRevision is reserved for a discrete
            // floating-origin rebase event when that service is introduced.
            currentLightingView.gpuOriginInFrameMeters =
                view->Camera().localPositionMeters;
            currentLightingView.forward =
                view->Camera().forward;
            currentLightingView.up =
                view->Camera().up;
            currentLightingView.verticalFovRadians =
                view->Camera().verticalFovRadians;
            currentLightingView.nearPlaneMeters =
                view->Camera().nearPlaneMeters;
            currentLightingView.farPlaneMeters =
                view->Camera().farPlaneMeters;

            currentLightingView.change =
                lighting::ClassifyLightingViewChange(
                    previousLightingView,
                    currentLightingView,
                    snapshot.viewportTargetsChanged,
                    false);

            view->Lighting() =
                currentLightingView;
        }

        if (logicalTarget->target.has_value() &&
            snapshot.hasWorld &&
            bodies != nullptr &&
            frames != nullptr)
        {
            const auto targetBody =
                logicalTarget->target->body;

            const auto* bodyRecord =
                bodies->FindBody(
                    targetBody);

            const auto bodyObject =
                session.World().
                    Universe().
                    ObjectForBody(
                        targetBody);

            if (bodyRecord != nullptr &&
                bodyObject.has_value())
            {
                const u64 semanticRevision =
                    session.World().
                        Objects().
                        Revision();

                auto& proxyPresentation =
                    visibilityProxyPresentations_[
                        info.id];

                const bool requiresRebuild =
                    proxyPresentation.provider ==
                        nullptr ||
                    proxyPresentation.body !=
                        targetBody ||
                    proxyPresentation.frame !=
                        view->Lighting().frame ||
                    proxyPresentation.
                            semanticRevision !=
                        semanticRevision ||
                    proxyPresentation.hasDynamic;

                if (requiresRebuild)
                {
                    const auto resolved =
                        world_model::
                            ResolveVisibilityProxies(
                                session.World().
                                    Objects(),
                                *bodyObject);

                    const auto proxies =
                        BuildVisibilityProxies(
                            resolved,
                            targetBody,
                            bodyRecord->frame);

                    const bool hasDynamic =
                        std::any_of(
                            proxies.begin(),
                            proxies.end(),
                            [](const auto& proxy)
                            {
                                return proxy.dynamic;
                            });

                    proxyPresentation.scene.Rebuild(
                        proxies,
                        view->Lighting().frame,
                        *frames,
                        atTime);

                    proxyPresentation.body =
                        targetBody;
                    proxyPresentation.frame =
                        view->Lighting().frame;
                    proxyPresentation.semanticRevision =
                        semanticRevision;
                    proxyPresentation.hasDynamic =
                        hasDynamic;

                    proxyPresentation.provider =
                        std::make_unique<
                            lighting::
                                SoftwareProxyVisibilityProvider>(
                                    proxyPresentation.scene);

                    if (proxyPresentation.hardware ==
                            nullptr &&
                        device_ != nullptr &&
                        compiler_ != nullptr)
                    {
                        proxyPresentation.hardware =
                            std::make_unique<
                                lighting::
                                    HardwareRayQueryVisibilityBatch>(
                                        *device_,
                                        *compiler_);
                    }

                    if (proxyPresentation.hardware !=
                        nullptr)
                    {
                        proxyPresentation.hardware->
                            RebuildScene(
                                proxyPresentation.scene,
                                view->Lighting().
                                    gpuOriginInFrameMeters);
                    }

                    const auto& stats =
                        proxyPresentation.scene.
                            Stats();

                    visibilityProxyDiagnostics_.
                        insert_or_assign(
                            info.id,
                            StudioVisibilityProxyDiagnostics{
                                .body =
                                    targetBody,
                                .frame =
                                    view->Lighting().
                                        frame,
                                .semanticRevision =
                                    semanticRevision,
                                .proxyCount =
                                    stats.proxyCount,
                                .bvhNodeCount =
                                    stats.nodeCount,
                                .dynamicProxyCount =
                                    stats.dynamicProxyCount,
                                .maximumNominalErrorMeters =
                                    stats.
                                        maximumNominalErrorMeters,
                                .rebuiltThisFrame =
                                    true,
                                .hardwareRayQuerySupported =
                                    proxyPresentation.hardware !=
                                        nullptr &&
                                    proxyPresentation.hardware->
                                        Supported(),
                                .hardwareRayQueryReady =
                                    proxyPresentation.hardware !=
                                        nullptr &&
                                    proxyPresentation.hardware->
                                        Ready(),
                                .hardwarePrimitiveCount =
                                    proxyPresentation.hardware !=
                                        nullptr
                                        ? proxyPresentation.hardware->
                                              PrimitiveCount()
                                        : 0U
                            });
                }
                else
                {
                    auto diagnostics =
                        visibilityProxyDiagnostics_[
                            info.id];

                    diagnostics.rebuiltThisFrame =
                        false;

                    visibilityProxyDiagnostics_.
                        insert_or_assign(
                            info.id,
                            diagnostics);
                }
            }
            else
            {
                visibilityProxyPresentations_.
                    erase(info.id);
                visibilityProxyDiagnostics_.
                    erase(info.id);
            }
        }
        else
        {
            visibilityProxyPresentations_.
                erase(info.id);
            visibilityProxyDiagnostics_.
                erase(info.id);
        }

        const auto terrainRuntime =
            session.TerrainRuntime().
                Capture(
                    info.id);

        if (terrainRuntime.has_value() &&
            !session.TerrainRuntime().
                IsCurrent(
                    *terrainRuntime))
        {
            throw std::logic_error(
                "Studio terrain viewport runtime is stale for the current session generation.");
        }

        const surface::TerrainSurfaceCapability* macroGlobeSurface = nullptr;

        if (logicalTarget->target.has_value() &&
            snapshot.hasWorld)
        {
            macroGlobeSurface =
                session.World().
                    Surfaces().
                    Registry().
                    FindTerrainSurface(
                        logicalTarget->target->body);
        }

        const bool hasMacroGlobe =
            macroGlobeSurface != nullptr &&
            macroGlobeSurface->terrain != nullptr;

        const auto studioDirectLight =
            logicalTarget->target.has_value() &&
                    snapshot.hasWorld
                ? ResolveStudioDirectLight(
                      session,
                      logicalTarget->target->body,
                      atTime)
                : ResolvedStudioDirectLight{};

        if (studioDirectLight.direct.has_value())
        {
            lightingDiagnostics_.insert_or_assign(
                info.id,
                StudioCelestialLightingDiagnostics{
                    .receiver =
                        studioDirectLight.direct->receiver,
                    .emitter =
                        studioDirectLight.direct->emitter,
                    .visibleFraction =
                        studioDirectLight.direct->visibleFraction,
                    .irradianceWattsPerSquareMeter =
                        studioDirectLight.direct->
                            irradianceWattsPerSquareMeter,
                    .contributingOccluders =
                        static_cast<u32>(
                            studioDirectLight.direct->
                                contributingOccluders.size())
                });
        }
        else
        {
            lightingDiagnostics_.erase(
                info.id);
        }

        const auto atmosphereBody =
            logicalTarget->target.has_value() &&
                    snapshot.hasWorld
                ? session.World().
                      Universe().
                      ObjectForBody(
                          logicalTarget->target->body)
                : std::nullopt;
        std::vector<
            world_model::ResolvedCloudLayer>
            resolvedCloudLayers;

        if (atmosphereBody.has_value())
        {
            resolvedCloudLayers =
                world_model::ResolveCloudLayers(
                    session.World().Objects(),
                    *atmosphereBody);
        }

        if (!resolvedCloudLayers.empty() &&
            logicalTarget->target.has_value())
        {
            std::vector<
                celestial_clouds::CloudLayerParameters>
                cloudParameters;
            cloudParameters.reserve(
                resolvedCloudLayers.size());

            bool requiresClimate = false;
            bool requiresExternal = false;

            for (const auto& layer :
                 resolvedCloudLayers)
            {
                cloudParameters.push_back(
                    layer.parameters);

                requiresClimate |=
                    layer.parameters.sourceModel ==
                    celestial_clouds::
                        CloudSourceModel::
                            ClimateProcedural;

                requiresExternal |=
                    layer.parameters.sourceModel ==
                        celestial_clouds::
                            CloudSourceModel::Authored ||
                    layer.parameters.sourceModel ==
                        celestial_clouds::
                            CloudSourceModel::Imported;
            }

            const terrain::TerrainSource*
                cloudClimateSource =
                    hasMacroGlobe
                        ? macroGlobeSurface->terrain.get()
                        : nullptr;

            if (requiresClimate &&
                cloudClimateSource == nullptr)
            {
                cloudPresentations_.erase(
                    info.id);
                cloudDiagnostics_.erase(
                    info.id);
            }
            else if (!requiresExternal)
            {
                const f64 referenceRadius =
                    shape.has_value()
                        ? universe::
                              ReferenceRadiusMeters(
                                  *shape)
                        : 1.0;

                const celestial_clouds::
                    CloudFieldConfig
                    cloudConfig{};

                const u64 cloudFingerprint =
                    celestial_clouds::
                        CloudFieldFingerprint(
                            cloudClimateSource,
                            nullptr,
                            referenceRadius,
                            cloudParameters,
                            atTime,
                            cloudConfig);

                auto& cloudPresentation =
                    cloudPresentations_[info.id];

                const bool cloudNeedsBuild =
                    cloudPresentation.field ==
                        nullptr ||
                    cloudPresentation.body !=
                        logicalTarget->
                            target->body ||
                    cloudPresentation.fingerprint !=
                        cloudFingerprint;

                if (cloudNeedsBuild &&
                    acquireCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::CloudField,
                        cloudFingerprint,
                        celestial_scheduler::
                            WorkBackend::Gpu,
                        3U,
                        70,
                        true))
                {
                    cloudPresentation.field =
                        std::make_unique<
                            celestial_clouds::
                                CloudFieldProduct>(
                                    celestial_clouds::
                                        BuildCloudField(
                                            cloudClimateSource,
                                            nullptr,
                                            referenceRadius,
                                            cloudParameters,
                                            atTime,
                                            cloudConfig));

                    cloudPresentation.gpu =
                        std::make_unique<
                            celestial_clouds::
                                GpuCloudFieldProduct>(
                                    *device_,
                                    *cloudPresentation.
                                        field);

                    cloudPresentation.body =
                        logicalTarget->
                            target->body;
                    cloudPresentation.fingerprint =
                        cloudFingerprint;

                    static_cast<void>(
                        completeCelestialGrant(
                            info.id,
                            logicalTarget->
                                target->body,
                            celestial_scheduler::
                                WorkKind::CloudField,
                            cloudFingerprint));
                }

                const bool cloudCurrent =
                    cloudPresentation.field !=
                        nullptr &&
                    cloudPresentation.body ==
                        logicalTarget->
                            target->body &&
                    cloudPresentation.fingerprint ==
                        cloudFingerprint;

                if (cloudCurrent)
                {
                    f64 coverageSum = 0.0;
                    f64 opticalSum = 0.0;
                    u64 sampleCount = 0U;

                    for (const auto& layer :
                         cloudPresentation.field->layers)
                    {
                        for (const auto& texel :
                             layer.texels)
                        {
                            coverageSum +=
                                texel.coverage;
                            opticalSum +=
                                texel.opticalDepth;
                            ++sampleCount;
                        }
                    }

                    cloudDiagnostics_.insert_or_assign(
                        info.id,
                        StudioCloudDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .fingerprint =
                                cloudPresentation.
                                    fingerprint,
                            .climateRevision =
                                cloudPresentation.
                                    field->
                                    climateRevision,
                            .timeBucket =
                                cloudPresentation.
                                    field->
                                    timeBucket,
                            .layerCount =
                                static_cast<u32>(
                                    cloudPresentation.
                                        field->
                                        layers.size()),
                            .meanCoverage =
                                sampleCount > 0U
                                    ? coverageSum /
                                          static_cast<f64>(
                                              sampleCount)
                                    : 0.0,
                            .meanOpticalDepth =
                                sampleCount > 0U
                                    ? opticalSum /
                                          static_cast<f64>(
                                              sampleCount)
                                    : 0.0,
                            .gpuResident =
                                cloudPresentation.gpu !=
                                nullptr
                        });
                }
                else
                {
                    cloudDiagnostics_.erase(
                        info.id);
                }
            }
            else
            {
                // External authored/imported source selection is semantic
                // authority; Studio waits for the selected source object's
                // coverage adapter rather than silently substituting
                // procedural weather.
                cloudPresentations_.erase(
                    info.id);
                cloudDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            cloudPresentations_.erase(
                info.id);
            cloudDiagnostics_.erase(
                info.id);
        }


        std::optional<
            world_model::ResolvedOceanBody>
            resolvedOceanForView;

        if (atmosphereBody.has_value())
        {
            resolvedOceanForView =
                world_model::
                    ResolveOceanBody(
                        session.World().
                            Objects(),
                        *atmosphereBody);
        }

        std::optional<
            world_model::ResolvedRingSystem>
            resolvedRingSystemForView;

        if (atmosphereBody.has_value())
        {
            resolvedRingSystemForView =
                world_model::
                    ResolveRingSystem(
                        session.World().
                            Objects(),
                        *atmosphereBody);
        }

        celestial_rings::GpuRingMeshProduct*
            activeRingMesh = nullptr;
        bool activeRingNear = false;

        if (resolvedRingSystemForView.has_value() &&
            logicalTarget->target.has_value() &&
            shape.has_value() &&
            !resolvedRingSystemForView->
                parameters.bands.empty())
        {
            const f64 referenceRadius =
                ReferenceRadiusForShape(
                    *shape);

            const f64 outerRadius =
                resolvedRingSystemForView->
                    parameters.bands.back().
                    outerRadiusMeters;

            const f64 cameraDistance =
                math::Length(
                    view->Camera().
                        localPositionMeters);

            const f64 projectedRingRadiusPixels =
                cameraDistance > outerRadius
                    ? std::asin(
                          std::clamp(
                              outerRadius /
                                  cameraDistance,
                              0.0,
                              1.0)) /
                          std::max(
                              static_cast<f64>(
                                  view->Camera().
                                      verticalFovRadians),
                              1.0e-6) *
                          static_cast<f64>(
                              std::max(
                                  view->Height(),
                                  1U))
                    : static_cast<f64>(
                          std::max(
                              view->Height(),
                              1U)) *
                          0.5;

            activeRingNear =
                projectedRingRadiusPixels >=
                180.0;

            auto& rings =
                ringPresentations_[info.id];

            const u64 semanticFingerprint =
                resolvedRingSystemForView->
                    parameters.fingerprint;

            const bool ringsNeedBuild =
                rings.nearMesh == nullptr ||
                rings.farMesh == nullptr ||
                rings.body !=
                    logicalTarget->
                        target->body ||
                rings.fingerprint !=
                    semanticFingerprint ||
                rings.referenceRadiusMeters !=
                    referenceRadius;

            if (ringsNeedBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::RingPresentation,
                    semanticFingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    3U,
                    65,
                    true))
            {
                const auto nearCpu =
                    celestial_rings::
                        BuildRingMesh(
                            resolvedRingSystemForView->
                                parameters,
                            referenceRadius,
                            256U);

                const auto farCpu =
                    celestial_rings::
                        BuildRingMesh(
                            resolvedRingSystemForView->
                                parameters,
                            referenceRadius,
                            64U);

                rings.nearMesh =
                    std::make_unique<
                        celestial_rings::
                            GpuRingMeshProduct>(
                                *device_,
                                nearCpu);
                rings.farMesh =
                    std::make_unique<
                        celestial_rings::
                            GpuRingMeshProduct>(
                                *device_,
                                farCpu);
                rings.farProfile =
                    celestial_rings::
                        BuildFarRingProfile(
                            resolvedRingSystemForView->
                                parameters,
                            referenceRadius,
                            256U);
                rings.body =
                    logicalTarget->
                        target->body;
                rings.fingerprint =
                    semanticFingerprint;
                rings.referenceRadiusMeters =
                    referenceRadius;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::RingPresentation,
                        semanticFingerprint));
            }

            const bool ringsCurrent =
                rings.nearMesh != nullptr &&
                rings.farMesh != nullptr &&
                rings.body ==
                    logicalTarget->
                        target->body &&
                rings.fingerprint ==
                    semanticFingerprint &&
                rings.referenceRadiusMeters ==
                    referenceRadius;

            if (ringsCurrent)
            {
                activeRingMesh =
                    activeRingNear
                        ? rings.nearMesh.get()
                        : rings.farMesh.get();

                ringDiagnostics_.insert_or_assign(
                    info.id,
                    StudioRingDiagnostics{
                        .body =
                            logicalTarget->
                                target->body,
                        .fingerprint =
                            semanticFingerprint,
                        .bandCount =
                            static_cast<u32>(
                                resolvedRingSystemForView->
                                    parameters.bands.size()),
                        .innerRadiusMeters =
                            resolvedRingSystemForView->
                                parameters.bands.front().
                                innerRadiusMeters,
                        .outerRadiusMeters =
                            outerRadius,
                        .projectedOuterRadiusPixels =
                            projectedRingRadiusPixels,
                        .nearRepresentation =
                            activeRingNear,
                        .angularSegments =
                            activeRingNear
                                ? 256U
                                : 64U,
                        .farProfileSamples =
                            rings.farProfile.
                                radialSamples
                    });
            }
            else
            {
                ringDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            ringPresentations_.erase(
                info.id);
            ringDiagnostics_.erase(
                info.id);
        }

        std::optional<
            world_model::ResolvedMagnetosphere>
            resolvedMagnetosphereForView;

        if (atmosphereBody.has_value() &&
            shape.has_value())
        {
            resolvedMagnetosphereForView =
                world_model::
                    ResolveMagnetosphere(
                        session.World().
                            Objects(),
                        *atmosphereBody,
                        ReferenceRadiusForShape(
                            *shape));
        }

        celestial_magnetosphere_render::
            GpuAuroraMeshProduct*
            activeAuroraMesh = nullptr;
        bool activeAuroraNear = false;

        if (resolvedMagnetosphereForView.has_value() &&
            logicalTarget->target.has_value() &&
            shape.has_value())
        {
            const f64 referenceRadius =
                ReferenceRadiusForShape(
                    *shape);
            const f64 outerRadius =
                referenceRadius +
                resolvedMagnetosphereForView->
                    parameters.
                    auroralMaximumAltitudeMeters;
            const f64 cameraDistance =
                math::Length(
                    view->Camera().
                        localPositionMeters);

            const f64 projectedAuroraRadiusPixels =
                cameraDistance > outerRadius
                    ? std::asin(
                          std::clamp(
                              outerRadius /
                                  cameraDistance,
                              0.0,
                              1.0)) /
                          std::max(
                              static_cast<f64>(
                                  view->Camera().
                                      verticalFovRadians),
                              1.0e-6) *
                          static_cast<f64>(
                              std::max(
                                  view->Height(),
                                  1U))
                    : static_cast<f64>(
                          std::max(
                              view->Height(),
                              1U)) *
                          0.5;

            activeAuroraNear =
                projectedAuroraRadiusPixels >=
                160.0;

            auto& presentation =
                magnetospherePresentations_[
                    info.id];

            const bool auroraNeedsBuild =
                presentation.nearAurora ==
                    nullptr ||
                presentation.farAurora ==
                    nullptr ||
                presentation.body !=
                    logicalTarget->
                        target->body ||
                presentation.fingerprint !=
                    resolvedMagnetosphereForView->
                        fingerprint ||
                presentation.referenceRadiusMeters !=
                    referenceRadius;

            if (auroraNeedsBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::AuroraPresentation,
                    resolvedMagnetosphereForView->
                        fingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    3U,
                    60,
                    true))
            {
                const auto nearCpu =
                    celestial_magnetosphere::
                        BuildAuroraCurtainMesh(
                            resolvedMagnetosphereForView->
                                parameters,
                            referenceRadius,
                            256U);

                const auto farCpu =
                    celestial_magnetosphere::
                        BuildAuroraCurtainMesh(
                            resolvedMagnetosphereForView->
                                parameters,
                            referenceRadius,
                            64U);

                presentation.nearAurora =
                    std::make_unique<
                        celestial_magnetosphere_render::
                            GpuAuroraMeshProduct>(
                                *device_,
                                nearCpu);
                presentation.farAurora =
                    std::make_unique<
                        celestial_magnetosphere_render::
                            GpuAuroraMeshProduct>(
                                *device_,
                                farCpu);
                presentation.body =
                    logicalTarget->
                        target->body;
                presentation.fingerprint =
                    resolvedMagnetosphereForView->
                        fingerprint;
                presentation.referenceRadiusMeters =
                    referenceRadius;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::AuroraPresentation,
                        resolvedMagnetosphereForView->
                            fingerprint));
            }

            const bool auroraCurrent =
                presentation.nearAurora !=
                    nullptr &&
                presentation.farAurora !=
                    nullptr &&
                presentation.body ==
                    logicalTarget->
                        target->body &&
                presentation.fingerprint ==
                    resolvedMagnetosphereForView->
                        fingerprint &&
                presentation.referenceRadiusMeters ==
                    referenceRadius;

            if (auroraCurrent)
            {
                activeAuroraMesh =
                    activeAuroraNear
                        ? presentation.
                              nearAurora.get()
                        : presentation.
                              farAurora.get();

                const auto product =
                    celestial_magnetosphere::
                        BuildMagnetosphereProduct(
                            resolvedMagnetosphereForView->
                                parameters,
                            referenceRadius,
                            {.ovalSamples =
                                 activeAuroraNear
                                     ? 256U
                                     : 64U});

                magnetosphereDiagnostics_.
                    insert_or_assign(
                        info.id,
                        StudioMagnetosphereDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .fingerprint =
                                resolvedMagnetosphereForView->
                                    fingerprint,
                            .subsolarStandoffMeters =
                                product.
                                    subsolarStandoffMeters,
                            .tailExtentMeters =
                                product.
                                    tailExtentMeters,
                            .auroralCenterLatitudeDegrees =
                                product.
                                    auroralCenterLatitudeDegrees,
                            .auroralMinimumAltitudeMeters =
                                resolvedMagnetosphereForView->
                                    parameters.
                                    auroralMinimumAltitudeMeters,
                            .auroralMaximumAltitudeMeters =
                                resolvedMagnetosphereForView->
                                    parameters.
                                    auroralMaximumAltitudeMeters,
                            .projectedAuroraRadiusPixels =
                                projectedAuroraRadiusPixels,
                            .activity =
                                resolvedMagnetosphereForView->
                                    parameters.activity,
                            .nearRepresentation =
                                activeAuroraNear,
                            .angularSegments =
                                activeAuroraNear
                                    ? 256U
                                    : 64U
                        });
            }
            else
            {
                magnetosphereDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            magnetospherePresentations_.erase(
                info.id);
            magnetosphereDiagnostics_.erase(
                info.id);
        }

        std::optional<
            world_model::ResolvedAtmosphereBody>
            resolvedAtmosphere;

        if (atmosphereBody.has_value())
        {
            resolvedAtmosphere =
                world_model::
                    ResolveAtmosphereBody(
                        session.World().
                            Objects(),
                        *atmosphereBody);
        }

        if (resolvedAtmosphere.has_value() &&
            logicalTarget->target.has_value())
        {
            const celestial_atmosphere::
                AtmosphereLutConfig
                atmosphereConfig{};

            auto& presentation =
                atmospherePresentations_[
                    info.id];

            const u64 staticFingerprint =
                celestial_atmosphere::
                    AtmosphereFingerprint(
                        resolvedAtmosphere->
                            parameters,
                        atmosphereConfig);

            const bool staticChanged =
                presentation.staticLuts ==
                    nullptr ||
                presentation.body !=
                    logicalTarget->
                        target->body ||
                presentation.staticFingerprint !=
                    staticFingerprint;

            if (staticChanged &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::AtmosphereLut,
                    staticFingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    4U,
                    90,
                    true))
            {
                presentation.staticLuts =
                    std::make_unique<
                        celestial_atmosphere::
                            AtmosphereStaticLuts>(
                                celestial_atmosphere::
                                    BuildStaticLuts(
                                        resolvedAtmosphere->
                                            parameters,
                                        atmosphereConfig));

                presentation.body =
                    logicalTarget->
                        target->body;
                presentation.staticFingerprint =
                    staticFingerprint;
                presentation.parameters =
                    resolvedAtmosphere->
                        parameters;
                presentation.skyView.reset();
                presentation.gpu.reset();
                presentation.skyFingerprint = 0U;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::AtmosphereLut,
                        staticFingerprint));
            }

            const bool atmosphereCurrent =
                presentation.staticLuts !=
                    nullptr &&
                presentation.body ==
                    logicalTarget->
                        target->body &&
                presentation.staticFingerprint ==
                    staticFingerprint;

            if (atmosphereCurrent)
            {
            const f64 rawObserverRadius =
                math::Length(
                    view->Camera().
                        localPositionMeters);

            const f64 observerRadius =
                std::max(
                    rawObserverRadius,
                    resolvedAtmosphere->
                        parameters.
                        bottomRadiusMeters +
                        1.0e-3);

            const f64 irradiance =
                studioDirectLight.
                        direct.has_value()
                    ? studioDirectLight.
                          direct->
                          irradianceWattsPerSquareMeter
                    : 0.0;

            const celestial_atmosphere::
                SkyViewInput
                skyInput{
                    .observerRadiusMeters =
                        observerRadius,
                    .sunDirectionBody = {
                        studioDirectLight.
                            directionBody.x,
                        studioDirectLight.
                            directionBody.y,
                        studioDirectLight.
                            directionBody.z
                    },
                    .incidentIrradianceWattsPerSquareMeter = {
                        irradiance,
                        irradiance,
                        irradiance
                    }
                };

            const u64 skyFingerprint =
                celestial_atmosphere::
                    AtmosphereSkyFingerprint(
                        resolvedAtmosphere->
                            parameters,
                        presentation.
                            staticFingerprint,
                        skyInput,
                        atmosphereConfig);

            const bool skyNeedsBuild =
                presentation.skyView ==
                    nullptr ||
                presentation.skyFingerprint !=
                    skyFingerprint;

            if (skyNeedsBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::AtmosphereSky,
                    skyFingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    2U,
                    95,
                    true))
            {
                presentation.skyView =
                    std::make_unique<
                        celestial_atmosphere::
                            AtmosphereSkyView>(
                                celestial_atmosphere::
                                    BuildSkyView(
                                        resolvedAtmosphere->
                                            parameters,
                                        *presentation.
                                            staticLuts,
                                        skyInput,
                                        atmosphereConfig));

                presentation.skyFingerprint =
                    skyFingerprint;

                if (presentation.gpu ==
                    nullptr)
                {
                    presentation.gpu =
                        std::make_unique<
                            celestial_atmosphere::
                                GpuAtmosphereLuts>(
                                    *device_,
                                    *presentation.
                                        staticLuts,
                                    *presentation.
                                        skyView);
                }
                else
                {
                    presentation.gpu->
                        ReplaceSkyView(
                            *presentation.
                                skyView);
                }

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::AtmosphereSky,
                        skyFingerprint));
            }

            const bool skyCurrent =
                presentation.skyView !=
                    nullptr &&
                presentation.skyFingerprint ==
                    skyFingerprint;

            if (skyCurrent)
            {
            atmosphereDiagnostics_.
                insert_or_assign(
                    info.id,
                    StudioAtmosphereDiagnostics{
                        .body =
                            logicalTarget->
                                target->body,
                        .staticFingerprint =
                            presentation.
                                staticFingerprint,
                        .skyFingerprint =
                            presentation.
                                skyFingerprint,
                        .observerRadiusMeters =
                            rawObserverRadius,
                        .observerAltitudeMeters =
                            rawObserverRadius -
                            resolvedAtmosphere->
                                parameters.
                                bottomRadiusMeters,
                        .directIrradianceWattsPerSquareMeter =
                            irradiance,
                        .transmittanceWidth =
                            presentation.
                                staticLuts->
                                transmittance.width,
                        .transmittanceHeight =
                            presentation.
                                staticLuts->
                                transmittance.height,
                        .multiScatteringWidth =
                            presentation.
                                staticLuts->
                                multiScattering.width,
                        .multiScatteringHeight =
                            presentation.
                                staticLuts->
                                multiScattering.height,
                        .skyViewWidth =
                            presentation.
                                skyView->
                                skyView.width,
                        .skyViewHeight =
                            presentation.
                                skyView->
                                skyView.height
                    });
            }
            else
            {
                atmosphereDiagnostics_.erase(
                    info.id);
            }
            }
            else
            {
                atmosphereDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            atmospherePresentations_.erase(
                info.id);
            atmosphereDiagnostics_.erase(
                info.id);
        }

        if (const auto atmosphereFound =
                atmospherePresentations_.find(
                    info.id);
            atmosphereFound !=
                    atmospherePresentations_.end() &&
                atmosphereFound->second.gpu !=
                    nullptr)
        {
            auto* atmosphereGpu =
                atmosphereFound->
                    second.gpu.get();

            graph.AddPass(
                prefix +
                    ".AtmosphereLutUpload",
                {},
                [atmosphereGpu](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    atmosphereGpu->
                        EnsureUploaded(
                            commands);
                });
        }

        const auto liveDebugPage =
            views.LiveDebugPage(info.id);
        const bool hasDebugField =
            liveDebugPage != nullptr &&
            liveDebugPage->Has(
                info.debugField);

        const auto presentation =
            SelectStudioViewportPresentation(
                logicalTarget->mode,
                shape.has_value(),
                terrainRuntime.has_value(),
                hasMacroGlobe,
                liveDebugPage != nullptr,
                hasDebugField);

        const u32 width = view->Width();
        const u32 height = view->Height();
        auto* color = &view->Color();

        switch (presentation)
        {
        case StudioViewportPresentation::ProductionTerrain:
        {
            if (device_ == nullptr ||
                compiler_ == nullptr ||
                !terrainRuntime.has_value())
            {
                throw std::logic_error(
                    "Studio production-terrain presentation lost its device, compiler or runtime binding.");
            }

            const auto& source =
                session.TerrainRuntime().
                    TerrainSource(
                        *terrainRuntime);

            const auto* analytic =
                dynamic_cast<
                    const terrain::
                        AnalyticTerrainSource*>(
                            &source);

            if (analytic == nullptr)
            {
                throw std::logic_error(
                    "Studio production terrain currently requires the composed AnalyticTerrainSource used by the shared GPU field generator.");
            }

            const auto& terrainDescription =
                analytic->Description();

            const f64 maximumProductionDetailMeters =
                std::max(
                    terrainDescription.detailAmplitudeMeters *
                        2.0,
                    terrainDescription.mountains.reliefMeters);

            const f64 maximumMacroDisplacementMeters =
                std::max(
                    terrainDescription.
                        maximumElevationAboveSeaLevelMeters,
                    std::abs(
                        terrainDescription.
                            global.
                            seaLevelMeters));

            celestial_representation::ResolveInput
                representationInput{
                    .bodyRadiusMeters =
                        terrainRuntime->planet.radiusMeters,
                    .maximumProductionDetailMeters =
                        maximumProductionDetailMeters,
                    .maximumMacroDisplacementMeters =
                        maximumMacroDisplacementMeters,
                    .cameraDistanceToCenterMeters =
                        math::Length(
                            terrainRuntime->
                                observer.meters),
                    .verticalFieldOfViewRadians =
                        static_cast<f64>(
                            view->Camera().
                                verticalFovRadians),
                    .viewportHeightPixels =
                        static_cast<f64>(
                            std::max(
                                height,
                                1U)),
                    .features = {
                        .productionSurfaceAvailable = true,
                        .macroDisplacementAvailable =
                            hasMacroGlobe,
                        .complexFarAppearance =
                            hasMacroGlobe &&
                            !studioDirectLight.
                                direct.has_value(),
                        .radiativeEmitter = false
                    },
                    .policy =
                        celestialQualityPolicy_
                };

            const celestial_representation::
                RepresentationSubjectId
                representationSubject{
                    .high =
                        terrainRuntime->body.high,
                    .low =
                        terrainRuntime->body.low
                };

            const auto representationDecision =
                representationTracker_.ResolveFor(
                    representationSubject,
                    representationInput);

            const auto representationBlend =
                celestial_representation::
                    ResolveRepresentationBlend(
                        representationInput,
                        representationDecision);

            const auto weightFor =
                [&](const celestial_representation::
                        Representation representation)
                {
                    f64 weight = 0.0;

                    if (representationBlend.richer ==
                        representation)
                    {
                        weight +=
                            representationBlend.
                                richerWeight;
                    }

                    if (representationBlend.lower ==
                            representation &&
                        representationBlend.lower !=
                            representationBlend.richer)
                    {
                        weight +=
                            representationBlend.
                                lowerWeight;
                    }

                    return std::clamp(
                        weight,
                        0.0,
                        1.0);
                };

            const f64 productionWeight =
                weightFor(
                    celestial_representation::
                        Representation::
                            ProductionSurface);
            const f64 macroWeight =
                weightFor(
                    celestial_representation::
                        Representation::
                            MacroDisplacedGlobe);

            transitionDiagnostics_.insert_or_assign(
                info.id,
                StudioSurfaceGlobeTransitionDiagnostics{
                    .body =
                        terrainRuntime->body,
                    .representation =
                        representationDecision.
                            representation,
                    .lowerFidelityNeighbor =
                        representationDecision.
                            lowerFidelityNeighbor,
                    .richerWeight =
                        representationBlend.richerWeight,
                    .lowerWeight =
                        representationBlend.lowerWeight,
                    .productionSurfaceWeight =
                        productionWeight,
                    .macroGlobeWeight =
                        macroWeight,
                    .projectedRadiusPixels =
                        representationDecision.
                            projectedRadiusPixels,
                    .productionDetailErrorPixels =
                        representationDecision.
                            productionDetailErrorPixels,
                    .macroDisplacementErrorPixels =
                        representationDecision.
                            macroDisplacementErrorPixels,
                    .hysteresisHeld =
                        representationDecision.
                            hysteresisHeld,
                    .overlapping =
                        representationBlend.
                            overlapping
                });

            auto& terrain =
                terrainPresentations_[
                    info.id];

            const bool recreate =
                terrain.renderer == nullptr ||
                terrain.fieldGenerator == nullptr ||
                terrain.universeGeneration !=
                    terrainRuntime->
                        universeGeneration ||
                terrain.body !=
                    terrainRuntime->body ||
                terrain.planet !=
                    terrainRuntime->planet.id ||
                terrain.terrainSourceRevision !=
                    terrainRuntime->
                        terrainSourceRevision ||
                !SameClipmapConfig(
                    terrain.clipmap,
                    terrainRuntime->clipmap);

            if (recreate)
            {
                terrain_render::
                    TerrainPreviewConfig
                    config{};

                config.clipmap =
                    terrainRuntime->clipmap;
                config.adaptiveCoverage.enabled =
                    false;
                config.nearPlaneMeters =
                    std::max(
                        view->Camera().
                            nearPlaneMeters,
                        0.01F);
                config.farPlaneMeters =
                    std::max(
                        view->Camera().
                            farPlaneMeters,
                        config.nearPlaneMeters *
                            100.0F);
                config.framesInFlight =
                    framesInFlight_;

                terrain.fieldGenerator =
                    std::make_unique<
                        terrain_gpu::
                            GpuFieldGenerator>(
                                *device_,
                                *compiler_,
                                terrainRuntime->
                                    planet,
                                *analytic);

                terrain.renderer =
                    std::make_unique<
                        terrain_render::
                            TerrainPreviewRenderer>(
                                *device_,
                                *compiler_,
                                terrainRuntime->
                                    planet,
                                *terrain.
                                    fieldGenerator,
                                terrainRuntime->
                                    observer,
                                config);

                terrain.universeGeneration =
                    terrainRuntime->
                        universeGeneration;
                terrain.body =
                    terrainRuntime->body;
                terrain.planet =
                    terrainRuntime->planet.id;
                terrain.terrainSourceRevision =
                    terrainRuntime->
                        terrainSourceRevision;
                terrain.clipmap =
                    terrainRuntime->clipmap;
                terrain.observer =
                    terrainRuntime->observer;
            }
            else if (
                terrain.observer.meters !=
                    terrainRuntime->
                        observer.meters)
            {
                terrain.renderer->
                    UpdateObserver(
                        terrainRuntime->
                            observer);

                terrain.observer =
                    terrainRuntime->observer;
            }

            terrain.runtimeGeneration =
                terrainRuntime->
                    runtimeGeneration;

            auto physicalPages =
                BuildPhysicalRenderPages(
                    session,
                    *terrainRuntime,
                    *analytic,
                    *device_);

            terrain.renderer->
                SetPhysicalPages(
                    physicalPages.pages,
                    physicalPages.generation);

            const auto camera =
                TerrainCameraFromBodyCamera(
                    view->Camera(),
                    terrainRuntime->observer);

            auto* terrainRenderer =
                terrain.renderer.get();
            auto* surfaceBaseRoughness =
                &view->SurfaceBaseRoughness();
            auto* surfaceNormalMetallic =
                &view->SurfaceNormalMetallic();
            auto* surfaceEmissionClass =
                &view->SurfaceEmissionClass();
            auto* depth =
                &view->Depth();

            auto* performance =
                &session.TerrainPerformance();

            const std::string
                performanceViewportId =
                    info.id;

            const world::WorldPosition
                performanceObserver =
                    terrainRuntime->observer;

            const auto performanceCacheStats =
                terrainRuntime->cacheStats;

            const std::string
                performanceAdapter =
                    std::string(
                        device_->
                            AdapterName());

            if (productionWeight > 0.0)
            {
                graph.AddPass(
                    prefix + ".ProductionTerrain",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceBaseRoughness,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceNormalMetallic,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceEmissionClass,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.depth,
                            .state = rhi::ResourceState::DepthWrite,
                            .access = render_graph::Access::Write
                        }
                    },
                    [color,
                     surfaceBaseRoughness,
                     surfaceNormalMetallic,
                     surfaceEmissionClass,
                     depth,
                     width,
                     height,
                     terrainRenderer,
                     camera,
                     frameIndex,
                     performance,
                     performanceViewportId,
                     performanceObserver,
                     performanceCacheStats,
                     performanceAdapter,
                     framesInFlight =
                        framesInFlight_](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *color,
                            {
                                .red = 0.008F,
                                .green = 0.012F,
                                .blue = 0.020F,
                                .alpha = 1.0F
                            });
    
                        commands.ClearColorTarget(
                            *surfaceBaseRoughness,
                            {0.0F, 0.0F, 0.0F, 1.0F});
                        commands.ClearColorTarget(
                            *surfaceNormalMetallic,
                            {0.0F, 1.0F, 0.0F, 0.0F});
                        commands.ClearColorTarget(
                            *surfaceEmissionClass,
                            {0.0F, 0.0F, 0.0F, 0.0F});
                        commands.ClearDepthTarget(
                            *depth,
                            0.0F);

                        const std::array<rhi::Texture*, 4> surfaceTargets{
                            color,
                            surfaceBaseRoughness,
                            surfaceNormalMetallic,
                            surfaceEmissionClass
                        };
                        commands.SetRenderTargets(
                            surfaceTargets,
                            depth);
    
                        terrainRenderer->Draw(
                            commands,
                            frameIndex %
                                framesInFlight,
                            width,
                            height,
                            camera);
    
                        const auto& stats =
                            terrainRenderer->
                                StreamingStats();
    
                        performance->
                            RecordViewportStreaming(
                                performanceViewportId,
                                performanceObserver,
                                performanceCacheStats,
                                {
                                    .generatedSamplesLastUpdate =
                                        stats.generatedSamplesLastUpdate,
                                    .refreshedRegionsLastUpdate =
                                        stats.refreshedRegionsLastUpdate,
                                    .levelsTouchedLastUpdate =
                                        stats.levelsTouchedLastUpdate,
                                    .uploadedBytesLastFrame =
                                        stats.uploadedBytesLastFrame,
                                    .drawCallsLastFrame =
                                        stats.drawCallsLastFrame,
                                    .cumulativeGeneratedSamples =
                                        stats.cumulativeGeneratedSamples,
                                    .cumulativeUploadedBytes =
                                        stats.cumulativeUploadedBytes,
                                    .submittedBatches =
                                        stats.submittedBatches,
                                    .committedBatches =
                                        stats.committedBatches,
                                    .supersededBatches =
                                        stats.supersededBatches,
                                    .revisionInvalidations =
                                        stats.revisionInvalidations,
                                    .staleRevisionBatches =
                                        stats.staleRevisionBatches,
                                    .coverageTierChanges =
                                        stats.coverageTierChanges,
                                    .rebaseCount =
                                        stats.rebaseCount,
                                    .adaptiveCoverageTier =
                                        stats.adaptiveCoverageTier,
                                    .activeBaseSpacingMeters =
                                        stats.activeBaseSpacingMeters,
                                    .activeOuterHalfExtentMeters =
                                        stats.activeOuterHalfExtentMeters,
                                    .updatePending =
                                        stats.updatePending
                                },
                                performanceAdapter);
                    });
            }

            const bool hasFarRepresentation =
                representationBlend.richer !=
                    celestial_representation::
                        Representation::
                            ProductionSurface ||
                representationBlend.lower !=
                    celestial_representation::
                        Representation::
                            ProductionSurface;

            if (hasFarRepresentation &&
                hasMacroGlobe &&
                macroGlobeSurface != nullptr &&
                macroGlobeSurface->terrain != nullptr &&
                shape.has_value())
            {
                auto* transitionGlobe =
                    EnsureMacroGlobePresentation(
                        info.id,
                        session,
                        terrainRuntime->body,
                        *shape,
                        *macroGlobeSurface->terrain,
                        [&](const u64 revision)
                        {
                            return acquireCelestialGrant(
                                info.id,
                                terrainRuntime->body,
                                celestial_scheduler::
                                    WorkKind::FarImpostor,
                                revision,
                                celestial_scheduler::
                                    WorkBackend::Gpu,
                                5U,
                                75,
                                true);
                        },
                        [&](const u64 revision)
                        {
                            static_cast<void>(
                                completeCelestialGrant(
                                    info.id,
                                    terrainRuntime->body,
                                    celestial_scheduler::
                                        WorkKind::FarImpostor,
                                    revision));
                        });

                auto* farPresentation =
                    &macroGlobePresentations_[
                        info.id];

                const auto globeCamera =
                    view->Camera();

                const bool clearForFarOnly =
                    productionWeight <= 0.0;

                const auto drawRepresentation =
                    [this,
                     color,
                     width,
                     height,
                     transitionGlobe,
                     farPresentation,
                     surfaceBaseRoughness,
                     surfaceNormalMetallic,
                     surfaceEmissionClass,
                     shape = *shape,
                     globeCamera,
                     studioDirectLight,
                     resolvedOceanForView,
                     projectedRadius =
                        representationDecision.
                            projectedRadiusPixels](
                        rhi::CommandList& commands,
                        const celestial_representation::
                            Representation representation,
                        const f32 opacity)
                    {
                        if (opacity <= 0.0F)
                        {
                            return;
                        }

                        if (representation ==
                            celestial_representation::
                                Representation::
                                    MacroDisplacedGlobe)
                        {
                            if (transitionGlobe == nullptr)
                            {
                                return;
                            }

                            const auto macroLighting =
                                celestial_globe::MacroGlobeLighting{
                                    .directionBody =
                                        studioDirectLight.directionBody,
                                    .irradianceScale =
                                        studioDirectLight.irradianceScale,
                                    .oceanRefractiveIndex =
                                        static_cast<f32>(
                                            resolvedOceanForView.has_value()
                                                ? resolvedOceanForView->optical.refractiveIndex
                                                : 1.333),
                                    .oceanRoughness =
                                        static_cast<f32>(
                                            resolvedOceanForView.has_value()
                                                ? resolvedOceanForView->optical.orbitalRoughness
                                                : 0.12),
                                    .oceanGlintStrength =
                                        static_cast<f32>(
                                            resolvedOceanForView.has_value()
                                                ? resolvedOceanForView->optical.glintStrength
                                                : 1.0),
                                    .oceanEnabled =
                                        resolvedOceanForView.has_value()
                                };

                            if (opacity >= 0.5F)
                            {
                                macroGlobeRenderer_.DrawSurface(
                                    commands,
                                    *color,
                                    *surfaceBaseRoughness,
                                    *surfaceNormalMetallic,
                                    *surfaceEmissionClass,
                                    width,
                                    height,
                                    *transitionGlobe,
                                    globeCamera,
                                    opacity,
                                    macroLighting);
                            }
                            else
                            {
                                macroGlobeRenderer_.Draw(
                                    commands,
                                    *color,
                                    width,
                                    height,
                                    *transitionGlobe,
                                    globeCamera,
                                    opacity,
                                    macroLighting);
                            }
                            return;
                        }

                        celestial_far_render::FarBodyDraw
                            draw{
                                .representation =
                                    representation,
                                .shape = shape,
                                .camera = globeCamera,
                                .appearance =
                                    farPresentation->
                                        appearanceSummary,
                                .projectedRadiusPixels =
                                    projectedRadius,
                                .opacity = opacity,
                                .lightDirectionBody =
                                    studioDirectLight.
                                        directionBody,
                                .incidentLightScale =
                                    studioDirectLight.
                                        irradianceScale,
                                .oceanRefractiveIndex =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.refractiveIndex
                                            : 1.333),
                                .oceanRoughness =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.orbitalRoughness
                                            : 0.12),
                                .oceanGlintStrength =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.glintStrength
                                            : 1.0),
                                .oceanEnabled =
                                    resolvedOceanForView.has_value(),
                                .stellar =
                                    representation ==
                                    celestial_representation::
                                        Representation::
                                            StellarPointProxy
                            };

                        farBodyRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            draw,
                            farPresentation->
                                cachedDisc.get());
                        if (opacity >= 0.5F)
                        {
                            farBodyRenderer_.DrawSurfaceData(
                                commands,
                                *surfaceBaseRoughness,
                                *surfaceNormalMetallic,
                                *surfaceEmissionClass,
                                width,
                                height,
                                draw);
                        }
                    };

                const auto richer =
                    representationBlend.richer;
                const auto lower =
                    representationBlend.lower;
                const f32 lowerOpacity =
                    static_cast<f32>(
                        std::clamp(
                            representationBlend.
                                lowerWeight,
                            0.0,
                            1.0));

                graph.AddPass(
                    prefix +
                        ".CelestialFarTransition",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture = targets.surfaceBaseRoughness,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceNormalMetallic,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceEmissionClass,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [color,
                     surfaceBaseRoughness,
                     surfaceNormalMetallic,
                     surfaceEmissionClass,
                     clearForFarOnly,
                     richer,
                     lower,
                     lowerOpacity,
                     farPresentation,
                     drawRepresentation](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        if ((richer ==
                                 celestial_representation::
                                     Representation::
                                         CachedDiscImpostor ||
                             lower ==
                                 celestial_representation::
                                     Representation::
                                         CachedDiscImpostor) &&
                            farPresentation->cachedDisc !=
                                nullptr)
                        {
                            farPresentation->
                                cachedDisc->
                                EnsureUploaded(
                                    commands);
                        }

                        if (clearForFarOnly)
                        {
                            commands.ClearColorTarget(
                                *color,
                                {
                                    .red = 0.006F,
                                    .green = 0.010F,
                                    .blue = 0.018F,
                                    .alpha = 1.0F
                                });
                        
                            commands.ClearColorTarget(
                                *surfaceBaseRoughness,
                                {0.0F, 0.0F, 0.0F, 1.0F});
                            commands.ClearColorTarget(
                                *surfaceNormalMetallic,
                                {0.0F, 1.0F, 0.0F, 0.0F});
                            commands.ClearColorTarget(
                                *surfaceEmissionClass,
                                {0.0F, 0.0F, 0.0F, 0.0F});
                        }

                        if (richer !=
                            celestial_representation::
                                Representation::
                                    ProductionSurface)
                        {
                            drawRepresentation(
                                commands,
                                richer,
                                1.0F);
                        }

                        if (lower != richer &&
                            lower !=
                                celestial_representation::
                                    Representation::
                                        ProductionSurface)
                        {
                            drawRepresentation(
                                commands,
                                lower,
                                lowerOpacity);
                        }
                    });
            }
            break;
        }

        case StudioViewportPresentation::TerrainDebug:
        {
            transitionDiagnostics_.erase(
                info.id);

            macroGlobePresentations_.erase(
                info.id);

            terrainPresentations_.erase(
                info.id);

            if (device_ == nullptr ||
                liveDebugPage == nullptr)
            {
                throw std::logic_error(
                    "Studio terrain-debug presentation lost its device or live page.");
            }

            auto& debug =
                debugPresentations_[info.id];

            if (debug.texture == nullptr ||
                debug.texture->Width() !=
                    liveDebugPage->Width() ||
                debug.texture->Height() !=
                    liveDebugPage->Height())
            {
                debug.texture =
                    std::make_unique<
                        terrain_debug::TerrainDebugTexture>(
                            *device_,
                            liveDebugPage->Width(),
                            liveDebugPage->Height());
                debug.source.reset();
            }

            const auto seams =
                terrain_debug::InspectTerrainDebugSeams(
                    *liveDebugPage,
                    info.debugField,
                    session.TerrainDebugPages());

            const u64 seamFingerprint =
                terrain_debug::
                    TerrainDebugSeamOverlayFingerprint(
                        seams);

            const bool needsUpload =
                debug.source != liveDebugPage ||
                debug.field != info.debugField ||
                debug.seamFingerprint !=
                    seamFingerprint ||
                !debug.texture->HasContent();

            auto* debugTexture =
                debug.texture.get();
            const auto field =
                info.debugField;

            graph.AddPass(
                prefix + ".TerrainDebug",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 debugTexture,
                 liveDebugPage,
                 field,
                 seams,
                 needsUpload](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    if (needsUpload)
                    {
                        debugTexture->Upload(
                            commands,
                            liveDebugPage->View(field),
                            seams);
                    }

                    debugComposite_.Draw(
                        commands,
                        debugTexture->Texture(),
                        *color,
                        width,
                        height);
                });

            debug.source = liveDebugPage;
            debug.field = field;
            debug.seamFingerprint =
                seamFingerprint;
            break;
        }

        case StudioViewportPresentation::TerrainDebugUnavailable:
        {
            transitionDiagnostics_.erase(
                info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            graph.AddPass(
                prefix + ".TerrainDebugUnavailable",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.055F,
                            .green = 0.018F,
                            .blue = 0.024F,
                            .alpha = 1.0F
                        });
                });
            break;
        }

        case StudioViewportPresentation::MacroGlobe:
        {
            transitionDiagnostics_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            if (device_ == nullptr ||
                macroGlobeSurface == nullptr ||
                macroGlobeSurface->terrain == nullptr ||
                !logicalTarget->target.has_value() ||
                !shape.has_value())
            {
                throw std::logic_error(
                    "Studio macro-globe presentation lost its terrain authority, device, shape, or target body.");
            }

            auto* globe =
                EnsureMacroGlobePresentation(
                    info.id,
                    session,
                    logicalTarget->target->body,
                    *shape,
                    *macroGlobeSurface->terrain,
                    [&](const u64 revision)
                    {
                        return acquireCelestialGrant(
                            info.id,
                            logicalTarget->target->body,
                            celestial_scheduler::
                                WorkKind::FarImpostor,
                            revision,
                            celestial_scheduler::
                                WorkBackend::Gpu,
                            5U,
                            75,
                            true);
                    },
                    [&](const u64 revision)
                    {
                        static_cast<void>(
                            completeCelestialGrant(
                                info.id,
                                logicalTarget->target->body,
                                celestial_scheduler::
                                    WorkKind::FarImpostor,
                                revision));
                    });

            const auto camera =
                view->Camera();
            auto* macroSurfaceBaseRoughness =
                &view->SurfaceBaseRoughness();
            auto* macroSurfaceNormalMetallic =
                &view->SurfaceNormalMetallic();
            auto* macroSurfaceEmissionClass =
                &view->SurfaceEmissionClass();

            graph.AddPass(
                prefix + ".MacroGlobe",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .texture = targets.surfaceBaseRoughness,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.surfaceNormalMetallic,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.surfaceEmissionClass,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 macroSurfaceBaseRoughness,
                 macroSurfaceNormalMetallic,
                 macroSurfaceEmissionClass,
                 width,
                 height,
                 globe,
                 camera,
                 studioDirectLight,
                 resolvedOceanForView](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.006F,
                            .green = 0.010F,
                            .blue = 0.018F,
                            .alpha = 1.0F
                        });
                    commands.ClearColorTarget(
                        *macroSurfaceBaseRoughness,
                        {0.0F, 0.0F, 0.0F, 1.0F});
                    commands.ClearColorTarget(
                        *macroSurfaceNormalMetallic,
                        {0.0F, 1.0F, 0.0F, 0.0F});
                    commands.ClearColorTarget(
                        *macroSurfaceEmissionClass,
                        {0.0F, 0.0F, 0.0F, 0.0F});

                    if (globe == nullptr)
                    {
                        return;
                    }

                    macroGlobeRenderer_.DrawSurface(
                        commands,
                        *color,
                        *macroSurfaceBaseRoughness,
                        *macroSurfaceNormalMetallic,
                        *macroSurfaceEmissionClass,
                        width,
                        height,
                        *globe,
                        camera,
                        1.0F,
                        celestial_globe::
                            MacroGlobeLighting{
                                .directionBody =
                                    studioDirectLight.
                                        directionBody,
                                .irradianceScale =
                                    studioDirectLight.
                                        irradianceScale,
                                .oceanRefractiveIndex =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.refractiveIndex
                                            : 1.333),
                                .oceanRoughness =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.orbitalRoughness
                                            : 0.12),
                                .oceanGlintStrength =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.glintStrength
                                            : 1.0),
                                .oceanEnabled =
                                    resolvedOceanForView.has_value()
                            });
                });

            break;
        }

        case StudioViewportPresentation::BodyPreview:
        {
            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            const auto camera =
                view->Camera();
            const auto bodyShape =
                *shape;
            auto* bodySurfaceBaseRoughness =
                &view->SurfaceBaseRoughness();
            auto* bodySurfaceNormalMetallic =
                &view->SurfaceNormalMetallic();
            auto* bodySurfaceEmissionClass =
                &view->SurfaceEmissionClass();

            const bool perspective =
                logicalTarget->mode ==
                studio_session::
                    ViewportMode::Perspective;

            const auto compactBodyObject =
                logicalTarget->target.has_value()
                    ? session.World().
                          Universe().
                          ObjectForBody(
                              logicalTarget->
                                  target->body)
                    : std::nullopt;

            const auto bodyMaterial =
                ResolveRuntimeBodyMaterial(
                    content_,
                    session.World().Objects(),
                    compactBodyObject);

            const auto resolvedCompactForView =
                compactBodyObject.has_value()
                    ? world_model::
                          ResolveCompactObject(
                              session.World().
                                  Objects(),
                              *compactBodyObject)
                    : std::nullopt;

            std::optional<
                world_model::ResolvedAccretionFlow>
                resolvedAccretionForView;

            std::optional<
                celestial_compact_objects::
                    CompactObjectPresentation>
                compactPresentation;

            if (resolvedCompactForView.has_value())
            {
                resolvedAccretionForView =
                    world_model::
                        ResolveAccretionFlow(
                            session.World().
                                Objects(),
                            *compactBodyObject,
                            resolvedCompactForView->
                                parameters);

                compactPresentation =
                    celestial_compact_objects::
                        BuildCompactObjectPresentation(
                            resolvedCompactForView->
                                parameters);

                const auto accretionProduct =
                    resolvedAccretionForView.has_value()
                        ? std::optional(
                              celestial_compact_objects::
                                  BuildAccretionFlowProduct(
                                      resolvedAccretionForView->
                                          parameters,
                                      resolvedCompactForView->
                                          parameters,
                                      64U))
                        : std::nullopt;

                const f64 opticalRadiusMeters =
                    std::max(
                        compactPresentation->
                            shadowRadiusMeters,
                        accretionProduct.has_value()
                            ? accretionProduct->
                                  outerRadiusMeters
                            : 0.0);

                const f64 cameraDistance =
                    std::max(
                        math::Length(
                            camera.
                                localPositionMeters),
                        opticalRadiusMeters);

                celestial_representation::ResolveInput
                    compactResolveInput{
                        .bodyRadiusMeters =
                            std::max(
                                opticalRadiusMeters,
                                1.0),
                        .maximumProductionDetailMeters =
                            0.0,
                        .maximumMacroDisplacementMeters =
                            0.0,
                        .cameraDistanceToCenterMeters =
                            cameraDistance,
                        .verticalFieldOfViewRadians =
                            static_cast<f64>(
                                camera.
                                    verticalFovRadians),
                        .viewportHeightPixels =
                            static_cast<f64>(
                                std::max(
                                    height,
                                    1U)),
                        .features = {
                            .productionSurfaceAvailable =
                                false,
                            .macroDisplacementAvailable =
                                false,
                            .complexFarAppearance =
                                false,
                            .radiativeEmitter =
                                false
                        },
                        .policy =
                            celestialQualityPolicy_
                    };

                const celestial_representation::
                    RepresentationSubjectId
                    compactSubject{
                        .high =
                            logicalTarget->
                                target->body.high,
                        .low =
                            logicalTarget->
                                target->body.low
                    };

                const auto compactDecision =
                    representationTracker_.
                        ResolveFor(
                            compactSubject,
                            compactResolveInput);

                const auto compactBlend =
                    celestial_representation::
                        ResolveRepresentationBlend(
                            compactResolveInput,
                            compactDecision);

                const auto pointWeightFor =
                    [](const celestial_representation::
                           Representation representation,
                       const f64 weight)
                    {
                        return representation ==
                                   celestial_representation::
                                       Representation::
                                           PointProxy ||
                               representation ==
                                   celestial_representation::
                                       Representation::
                                           StellarPointProxy
                            ? weight
                            : 0.0;
                    };

                const f64 pointProxyWeight =
                    std::clamp(
                        pointWeightFor(
                            compactBlend.richer,
                            compactBlend.richerWeight) +
                        pointWeightFor(
                            compactBlend.lower,
                            compactBlend.lowerWeight),
                        0.0,
                        1.0);

                const f64 projectedOpticalRadiusPixels =
                    compactDecision.
                        projectedRadiusPixels;

                const f64 projectedShadowRadiusPixels =
                    projectedOpticalRadiusPixels *
                    compactPresentation->
                        shadowRadiusMeters /
                    std::max(
                        opticalRadiusMeters,
                        1.0e-9);

                compactObjectDiagnostics_.
                    insert_or_assign(
                        info.id,
                        StudioCompactObjectDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .fingerprint =
                                resolvedCompactForView->
                                    fingerprint,
                            .gravitationalRadiusMeters =
                                compactPresentation->
                                    scales.
                                    gravitationalRadiusMeters,
                            .schwarzschildRadiusMeters =
                                compactPresentation->
                                    scales.
                                    schwarzschildRadiusMeters,
                            .photonSphereRadiusMeters =
                                compactPresentation->
                                    scales.
                                    photonSphereRadiusMeters,
                            .iscoRadiusMeters =
                                compactPresentation->
                                    scales.
                                    iscoRadiusMeters,
                            .shadowRadiusMeters =
                                compactPresentation->
                                    shadowRadiusMeters,
                            .projectedShadowRadiusPixels =
                                projectedShadowRadiusPixels,
                            .projectedOpticalRadiusPixels =
                                projectedOpticalRadiusPixels,
                            .representation =
                                compactDecision.
                                    representation,
                            .pointProxyWeight =
                                pointProxyWeight,
                            .accretionEnabled =
                                resolvedAccretionForView.
                                    has_value(),
                            .accretionOuterRadiusMeters =
                                accretionProduct.has_value()
                                    ? accretionProduct->
                                          outerRadiusMeters
                                    : 0.0
                        });

                const auto compactDraw =
                    celestial_compact_render::
                        CompactObjectDraw{
                            .compact =
                                *compactPresentation,
                            .accretion =
                                resolvedAccretionForView.
                                    has_value()
                                    ? std::optional(
                                          resolvedAccretionForView->
                                              parameters)
                                    : std::nullopt,
                            .camera = camera,
                            .projectedShadowRadiusPixels =
                                projectedShadowRadiusPixels,
                            .projectedOpticalRadiusPixels =
                                projectedOpticalRadiusPixels,
                            .pointProxyWeight =
                                static_cast<f32>(
                                    pointProxyWeight),
                            .opacity = 1.0F
                        };

                graph.AddPass(
                    prefix +
                        ".CompactObject",
                    {
                        {
                            .texture =
                                targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture =
                                targets.
                                    surfaceBaseRoughness,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture =
                                targets.
                                    surfaceNormalMetallic,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture =
                                targets.
                                    surfaceEmissionClass,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     color,
                     bodySurfaceBaseRoughness,
                     bodySurfaceNormalMetallic,
                     bodySurfaceEmissionClass,
                     width,
                     height,
                     compactDraw](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *color,
                            {
                                .red = 0.006F,
                                .green = 0.010F,
                                .blue = 0.018F,
                                .alpha = 1.0F
                            });
                        commands.ClearColorTarget(
                            *bodySurfaceBaseRoughness,
                            {0.0F, 0.0F, 0.0F, 1.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceNormalMetallic,
                            {0.0F, 1.0F, 0.0F, 0.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceEmissionClass,
                            {0.0F, 0.0F, 0.0F, 0.0F});

                        compactObjectRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            compactDraw);
                    });

                transitionDiagnostics_.erase(
                    info.id);

                break;
            }

            if (perspective &&
                logicalTarget->target.has_value())
            {
                bool radiativeEmitter = false;
                f32 resolvedRadiometricIntensity = 1.0F;
                f32 pointRadiometricIntensity = 1.0F;
                std::optional<
                    world_model::ResolvedRadiativeBody>
                    resolvedRadiativeForView;

                const auto bodyObject =
                    session.World().
                        Universe().
                        ObjectForBody(
                            logicalTarget->
                                target->body);

                std::optional<
                    world_model::ResolvedGiantAppearance>
                    resolvedGiantForView;
                std::optional<
                    world_model::ResolvedSmallBodyAppearance>
                    resolvedSmallBodyForView;

                if (bodyObject.has_value())
                {
                    resolvedRadiativeForView =
                        world_model::
                            ResolveRadiativeBody(
                                session.World().
                                    Objects(),
                                *bodyObject);

                    resolvedGiantForView =
                        world_model::
                            ResolveGiantAppearance(
                                session.World().
                                    Objects(),
                                *bodyObject);

                    resolvedSmallBodyForView =
                        world_model::
                            ResolveSmallBodyAppearance(
                                session.World().
                                    Objects(),
                                *bodyObject);

                    if (resolvedGiantForView.has_value() &&
                        resolvedSmallBodyForView.has_value())
                    {
                        throw std::runtime_error(
                            "A body cannot enable Giant Appearance and Small Body Appearance simultaneously.");
                    }

                    if (resolvedRadiativeForView.has_value())
                    {
                        const auto& radiative =
                            *resolvedRadiativeForView;
                        radiativeEmitter = true;


                        const f64 distanceMeters =
                            std::max(
                                math::Length(
                                    camera.
                                        localPositionMeters),
                                1.0);

                        const f64 pointIrradiance =
                            celestial_radiometry::
                                IrradianceWattsPerSquareMeter(
                                    radiative.
                                        radiative.
                                        luminosityWatts,
                                    distanceMeters);

                        const f64 resolvedPixelIrradiance =
                            celestial_radiometry::
                                ResolvedPixelIrradianceWattsPerSquareMeter(
                                    radiative.
                                        radiative.
                                        surfaceRadianceWattsPerSquareMeterSteradian,
                                    static_cast<f64>(
                                        camera.
                                            verticalFovRadians),
                                    std::max(
                                        height,
                                        1U));

                        pointRadiometricIntensity =
                            static_cast<f32>(
                                celestial_radiometry::
                                    EncodeIrradianceSceneLinear(
                                        pointIrradiance));

                        resolvedRadiometricIntensity =
                            static_cast<f32>(
                                celestial_radiometry::
                                    EncodeIrradianceSceneLinear(
                                        resolvedPixelIrradiance));
                    }
                }

                const f64 radius =
                    ReferenceRadiusForShape(
                        bodyShape);

                celestial_representation::ResolveInput
                    input{
                        .bodyRadiusMeters =
                            radius,
                        .maximumProductionDetailMeters =
                            0.0,
                        .maximumMacroDisplacementMeters =
                            0.0,
                        .cameraDistanceToCenterMeters =
                            std::max(
                                math::Length(
                                    camera.
                                        localPositionMeters),
                                radius),
                        .verticalFieldOfViewRadians =
                            static_cast<f64>(
                                camera.
                                    verticalFovRadians),
                        .viewportHeightPixels =
                            static_cast<f64>(
                                std::max(
                                    height,
                                    1U)),
                        .features = {
                            .productionSurfaceAvailable =
                                false,
                            .macroDisplacementAvailable =
                                false,
                            .complexFarAppearance =
                                false,
                            .radiativeEmitter =
                                radiativeEmitter
                        },
                        .policy =
                            celestialQualityPolicy_
                    };

                const celestial_representation::
                    RepresentationSubjectId
                    subject{
                        .high =
                            logicalTarget->
                                target->body.high,
                        .low =
                            logicalTarget->
                                target->body.low
                    };

                const auto decision =
                    representationTracker_.
                        ResolveFor(
                            subject,
                            input);

                const auto blend =
                    celestial_representation::
                        ResolveRepresentationBlend(
                            input,
                            decision);

                transitionDiagnostics_.
                    insert_or_assign(
                        info.id,
                        StudioSurfaceGlobeTransitionDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .representation =
                                decision.
                                    representation,
                            .lowerFidelityNeighbor =
                                decision.
                                    lowerFidelityNeighbor,
                            .richerWeight =
                                blend.richerWeight,
                            .lowerWeight =
                                blend.lowerWeight,
                            .productionSurfaceWeight =
                                0.0,
                            .macroGlobeWeight =
                                0.0,
                            .projectedRadiusPixels =
                                decision.
                                    projectedRadiusPixels,
                            .productionDetailErrorPixels =
                                0.0,
                            .macroDisplacementErrorPixels =
                                0.0,
                            .hysteresisHeld =
                                decision.
                                    hysteresisHeld,
                            .overlapping =
                                blend.overlapping
                        });

                const math::Float3 stellarColor =
                    resolvedRadiativeForView.has_value()
                        ? math::Float3{
                              static_cast<f32>(
                                  resolvedRadiativeForView->
                                      stellarColorLinear.x),
                              static_cast<f32>(
                                  resolvedRadiativeForView->
                                      stellarColorLinear.y),
                              static_cast<f32>(
                                  resolvedRadiativeForView->
                                      stellarColorLinear.z)}
                        : math::Float3{
                              1.0F, 1.0F, 1.0F};

                std::optional<
                    celestial_far_render::AppearanceSummary>
                    giantAppearanceSummary;
                std::optional<
                    celestial_far_render::AppearanceSummary>
                    smallBodyAppearanceSummary;
                f64 smallBodyMinimumRadiusScale = 1.0;
                f64 smallBodyMaximumRadiusScale = 1.0;

                if (resolvedGiantForView.has_value())
                {
                    auto& giantPresentation =
                        giantPresentations_[info.id];

                    const bool giantNeedsBuild =
                        giantPresentation.appearance ==
                            nullptr ||
                        giantPresentation.body !=
                            logicalTarget->
                                target->body ||
                        giantPresentation.fingerprint !=
                            resolvedGiantForView->
                                fingerprint;

                    if (giantNeedsBuild &&
                        acquireCelestialGrant(
                            info.id,
                            logicalTarget->
                                target->body,
                            celestial_scheduler::
                                WorkKind::
                                    OrbitalAppearance,
                            resolvedGiantForView->
                                fingerprint,
                            celestial_scheduler::
                                WorkBackend::Cpu,
                            3U,
                            80,
                            true))
                    {
                        auto giantAppearance =
                            celestial_giants::
                                BuildGiantAppearance(
                                    resolvedGiantForView->
                                        parameters,
                                    {
                                        .faceResolution =
                                            65U
                                    });

                        giantPresentation.summary =
                            celestial_far_render::
                                SummarizeAppearance(
                                    giantAppearance);
                        giantPresentation.gpuAppearance =
                            std::make_unique<
                                celestial_appearance::
                                    GpuPlanetaryAppearanceProduct>(
                                        *device_,
                                        giantAppearance);
                        giantPresentation.appearance =
                            std::make_unique<
                                celestial_appearance::
                                    PlanetaryAppearanceProduct>(
                                        std::move(
                                            giantAppearance));
                        giantPresentation.body =
                            logicalTarget->
                                target->body;
                        giantPresentation.fingerprint =
                            resolvedGiantForView->
                                fingerprint;

                        static_cast<void>(
                            completeCelestialGrant(
                                info.id,
                                logicalTarget->
                                    target->body,
                                celestial_scheduler::
                                    WorkKind::
                                        OrbitalAppearance,
                                resolvedGiantForView->
                                    fingerprint));
                    }

                    if (giantPresentation.appearance !=
                            nullptr &&
                        giantPresentation.body ==
                            logicalTarget->
                                target->body &&
                        giantPresentation.fingerprint ==
                            resolvedGiantForView->
                                fingerprint)
                    {
                        giantAppearanceSummary =
                            giantPresentation.summary;
                    }
                }
                else
                {
                    giantPresentations_.erase(
                        info.id);
                    giantDiagnostics_.erase(
                        info.id);
                }

                if (resolvedSmallBodyForView.has_value())
                {
                    auto& smallPresentation =
                        smallBodyPresentations_[info.id];

                    const bool smallBodyNeedsBuild =
                        smallPresentation.appearance ==
                            nullptr ||
                        smallPresentation.body !=
                            logicalTarget->
                                target->body ||
                        smallPresentation.fingerprint !=
                            resolvedSmallBodyForView->
                                fingerprint;

                    if (smallBodyNeedsBuild &&
                        acquireCelestialGrant(
                            info.id,
                            logicalTarget->
                                target->body,
                            celestial_scheduler::
                                WorkKind::
                                    OrbitalAppearance,
                            resolvedSmallBodyForView->
                                fingerprint,
                            celestial_scheduler::
                                WorkBackend::Cpu,
                            3U,
                            80,
                            true))
                    {
                        auto smallAppearance =
                            celestial_small_bodies::
                                BuildSmallBodyAppearance(
                                    resolvedSmallBodyForView->
                                        parameters,
                                    {
                                        .faceResolution =
                                            65U
                                    });

                        const auto smallShape =
                            celestial_small_bodies::
                                BuildSmallBodyShape(
                                    resolvedSmallBodyForView->
                                        parameters,
                                    {
                                        .faceResolution =
                                            65U
                                    });

                        smallPresentation.summary =
                            celestial_far_render::
                                SummarizeAppearance(
                                    smallAppearance);
                        smallPresentation.gpuAppearance =
                            std::make_unique<
                                celestial_appearance::
                                    GpuPlanetaryAppearanceProduct>(
                                        *device_,
                                        smallAppearance);
                        smallPresentation.appearance =
                            std::make_unique<
                                celestial_appearance::
                                    PlanetaryAppearanceProduct>(
                                        std::move(
                                            smallAppearance));
                        smallPresentation.body =
                            logicalTarget->
                                target->body;
                        smallPresentation.fingerprint =
                            resolvedSmallBodyForView->
                                fingerprint;
                        smallPresentation.minimumRadiusScale =
                            smallShape.minimumRadiusScale;
                        smallPresentation.maximumRadiusScale =
                            smallShape.maximumRadiusScale;

                        static_cast<void>(
                            completeCelestialGrant(
                                info.id,
                                logicalTarget->
                                    target->body,
                                celestial_scheduler::
                                    WorkKind::
                                        OrbitalAppearance,
                                resolvedSmallBodyForView->
                                    fingerprint));
                    }

                    if (smallPresentation.appearance !=
                            nullptr &&
                        smallPresentation.body ==
                            logicalTarget->
                                target->body &&
                        smallPresentation.fingerprint ==
                            resolvedSmallBodyForView->
                                fingerprint)
                    {
                        smallBodyAppearanceSummary =
                            smallPresentation.summary;
                        smallBodyMinimumRadiusScale =
                            smallPresentation.
                                minimumRadiusScale;
                        smallBodyMaximumRadiusScale =
                            smallPresentation.
                                maximumRadiusScale;
                    }
                }
                else
                {
                    smallBodyPresentations_.erase(
                        info.id);
                    smallBodyDiagnostics_.erase(
                        info.id);
                }

                celestial_far_render::
                    AppearanceSummary appearance{
                        .albedoLinear =
                            radiativeEmitter
                                ? stellarColor
                                : giantAppearanceSummary.has_value()
                                    ? giantAppearanceSummary->
                                          albedoLinear
                                    : smallBodyAppearanceSummary.has_value()
                                        ? smallBodyAppearanceSummary->
                                              albedoLinear
                                        : math::Float3{
                                              0.18F,
                                              0.21F,
                                              0.23F},
                        .roughness =
                            radiativeEmitter
                                ? 0.0F
                                : giantAppearanceSummary.has_value()
                                    ? giantAppearanceSummary->
                                          roughness
                                    : smallBodyAppearanceSummary.has_value()
                                        ? smallBodyAppearanceSummary->
                                              roughness
                                        : 0.82F,
                        .oceanFraction = 0.0F,
                        .iceFraction = 0.0F,
                        .emissionLinear = {}
                    };

                const auto richer =
                    blend.richer;
                const auto lower =
                    blend.lower;
                const f32 lowerOpacity =
                    static_cast<f32>(
                        std::clamp(
                            blend.lowerWeight,
                            0.0,
                            1.0));
                const f64 projectedRadius =
                    decision.
                        projectedRadiusPixels;

                if (resolvedRadiativeForView.has_value())
                {
                    stellarDiagnostics_.insert_or_assign(
                        info.id,
                        StudioStellarDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .appearanceFingerprint =
                                resolvedRadiativeForView->
                                    stellarAppearanceFingerprint,
                            .effectiveTemperatureKelvin =
                                resolvedRadiativeForView->
                                    radiative.
                                    effectiveTemperatureKelvin,
                            .colorLinear =
                                stellarColor,
                            .projectedRadiusPixels =
                                projectedRadius,
                            .representation =
                                decision.representation,
                            .resolvedSceneIntensity =
                                resolvedRadiometricIntensity,
                            .pointSceneIntensity =
                                pointRadiometricIntensity
                        });
                }
                else
                {
                    stellarDiagnostics_.erase(
                        info.id);
                }

                if (resolvedGiantForView.has_value())
                {
                    giantDiagnostics_.insert_or_assign(
                        info.id,
                        StudioGiantDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .appearanceFingerprint =
                                resolvedGiantForView->
                                    fingerprint,
                            .iceGiant =
                                resolvedGiantForView->
                                    parameters.giantClass ==
                                celestial_giants::
                                    GiantClass::IceGiant,
                            .bandFrequency =
                                resolvedGiantForView->
                                    parameters.bandFrequency,
                            .bandStrength =
                                resolvedGiantForView->
                                    parameters.bandStrength,
                            .stormStrength =
                                resolvedGiantForView->
                                    parameters.stormStrength,
                            .projectedRadiusPixels =
                                projectedRadius,
                            .representation =
                                decision.representation
                        });
                }

                if (resolvedSmallBodyForView.has_value())
                {
                    smallBodyDiagnostics_.insert_or_assign(
                        info.id,
                        StudioSmallBodyDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .appearanceFingerprint =
                                resolvedSmallBodyForView->
                                    fingerprint,
                            .minimumRadiusScale =
                                smallBodyMinimumRadiusScale,
                            .maximumRadiusScale =
                                smallBodyMaximumRadiusScale,
                            .irregularity =
                                resolvedSmallBodyForView->
                                    parameters.irregularity,
                            .craterDensity =
                                resolvedSmallBodyForView->
                                    parameters.craterDensity,
                            .oppositionStrength =
                                resolvedSmallBodyForView->
                                    parameters.oppositionStrength,
                            .projectedRadiusPixels =
                                projectedRadius,
                            .representation =
                                decision.representation
                        });
                }

                graph.AddPass(
                    prefix +
                        ".FarBody",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture = targets.surfaceBaseRoughness,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceNormalMetallic,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceEmissionClass,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     color,
                     bodySurfaceBaseRoughness,
                     bodySurfaceNormalMetallic,
                     bodySurfaceEmissionClass,
                     width,
                     height,
                     bodyShape,
                     camera,
                     appearance,
                     richer,
                     lower,
                     lowerOpacity,
                     projectedRadius,
                     radiativeEmitter,
                     resolvedRadiometricIntensity,
                     pointRadiometricIntensity,
                     resolvedRadiativeForView,
                     resolvedGiantForView,
                     resolvedSmallBodyForView,
                     studioDirectLight,
                     resolvedOceanForView](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *color,
                            {
                                .red = 0.006F,
                                .green = 0.010F,
                                .blue = 0.018F,
                                .alpha = 1.0F
                            });
                        commands.ClearColorTarget(
                            *bodySurfaceBaseRoughness,
                            {0.0F, 0.0F, 0.0F, 1.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceNormalMetallic,
                            {0.0F, 1.0F, 0.0F, 0.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceEmissionClass,
                            {0.0F, 0.0F, 0.0F, 0.0F});

                        const auto draw =
                            [&](const celestial_representation::
                                    Representation representation,
                                const f32 opacity)
                            {
                                celestial_far_render::
                                    FarBodyDraw far{
                                        .representation =
                                            representation,
                                        .shape =
                                            bodyShape,
                                        .camera =
                                            camera,
                                        .appearance =
                                            appearance,
                                        .projectedRadiusPixels =
                                            projectedRadius,
                                        .opacity =
                                            opacity,
                                        .radiometricIntensity =
                                            radiativeEmitter
                                                ? (representation ==
                                                           celestial_representation::
                                                               Representation::
                                                                   PointProxy ||
                                                   representation ==
                                                       celestial_representation::
                                                           Representation::
                                                               StellarPointProxy
                                                       ? pointRadiometricIntensity
                                                       : resolvedRadiometricIntensity)
                                                : 1.0F,
                                        .lightDirectionBody =
                                            studioDirectLight.
                                                directionBody,
                                        .incidentLightScale =
                                            radiativeEmitter
                                                ? 1.0F
                                                : studioDirectLight.
                                                    irradianceScale,
                                        .oceanRefractiveIndex =
                                            static_cast<f32>(
                                                resolvedOceanForView.has_value()
                                                    ? resolvedOceanForView->optical.refractiveIndex
                                                    : 1.333),
                                        .oceanRoughness =
                                            static_cast<f32>(
                                                resolvedOceanForView.has_value()
                                                    ? resolvedOceanForView->optical.orbitalRoughness
                                                    : 0.12),
                                        .oceanGlintStrength =
                                            static_cast<f32>(
                                                resolvedOceanForView.has_value()
                                                    ? resolvedOceanForView->optical.glintStrength
                                                    : 1.0),
                                        .oceanEnabled =
                                            resolvedOceanForView.has_value(),
                                        .giantEnabled =
                                            resolvedGiantForView.has_value(),
                                        .giantBaseColorLinear =
                                            resolvedGiantForView.has_value()
                                                ? math::Float3{
                                                      static_cast<f32>(resolvedGiantForView->parameters.baseColorLinear.x),
                                                      static_cast<f32>(resolvedGiantForView->parameters.baseColorLinear.y),
                                                      static_cast<f32>(resolvedGiantForView->parameters.baseColorLinear.z)}
                                                : math::Float3{0.62F,0.48F,0.31F},
                                        .giantBandColorLinear =
                                            resolvedGiantForView.has_value()
                                                ? math::Float3{
                                                      static_cast<f32>(resolvedGiantForView->parameters.bandColorLinear.x),
                                                      static_cast<f32>(resolvedGiantForView->parameters.bandColorLinear.y),
                                                      static_cast<f32>(resolvedGiantForView->parameters.bandColorLinear.z)}
                                                : math::Float3{0.90F,0.78F,0.58F},
                                        .giantPolarColorLinear =
                                            resolvedGiantForView.has_value()
                                                ? math::Float3{
                                                      static_cast<f32>(resolvedGiantForView->parameters.polarColorLinear.x),
                                                      static_cast<f32>(resolvedGiantForView->parameters.polarColorLinear.y),
                                                      static_cast<f32>(resolvedGiantForView->parameters.polarColorLinear.z)}
                                                : math::Float3{0.48F,0.42F,0.36F},
                                        .giantBandFrequency =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.bandFrequency:11.0),
                                        .giantBandStrength =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.bandStrength:0.72),
                                        .giantZonalShear =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.zonalShear:0.18),
                                        .giantStormStrength =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.stormStrength:0.35),
                                        .giantStormScale =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.stormScale:5.0),
                                        .giantPolarStrength =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.polarStrength:0.22),
                                        .giantDepthContrast =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.depthContrast:0.25),
                                        .giantTurbulenceStrength =
                                            static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.turbulenceStrength:0.18),
                                        .giantSeed =
                                            static_cast<u32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.seed&0xffffffffULL:1ULL),
                                        .smallBodyEnabled =
                                            resolvedSmallBodyForView.has_value(),
                                        .smallBodyAxisScale =
                                            resolvedSmallBodyForView.has_value()
                                                ? math::Float3{
                                                      static_cast<f32>(resolvedSmallBodyForView->parameters.axisScale.x),
                                                      static_cast<f32>(resolvedSmallBodyForView->parameters.axisScale.y),
                                                      static_cast<f32>(resolvedSmallBodyForView->parameters.axisScale.z)}
                                                : math::Float3{1.0F,0.82F,0.68F},
                                        .smallBodyIrregularity =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.irregularity:0.18),
                                        .smallBodyLargeLobeStrength =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.largeLobeStrength:0.12),
                                        .smallBodyCraterDensity =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.craterDensity:0.55),
                                        .smallBodyCraterDepth =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.craterDepth:0.12),
                                        .smallBodyCraterRimStrength =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.craterRimStrength:0.08),
                                        .smallBodyFreshMaterialColorLinear =
                                            resolvedSmallBodyForView.has_value()
                                                ? math::Float3{
                                                      static_cast<f32>(resolvedSmallBodyForView->parameters.freshMaterialColorLinear.x),
                                                      static_cast<f32>(resolvedSmallBodyForView->parameters.freshMaterialColorLinear.y),
                                                      static_cast<f32>(resolvedSmallBodyForView->parameters.freshMaterialColorLinear.z)}
                                                : math::Float3{0.24F,0.22F,0.19F},
                                        .smallBodyColorVariation =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.colorVariation:0.18),
                                        .smallBodyOppositionStrength =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.oppositionStrength:0.55),
                                        .smallBodyOppositionWidthRadians =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.oppositionWidthRadians:0.055),
                                        .smallBodySingleScatteringAlbedo =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.singleScatteringAlbedo:0.16),
                                        .smallBodyMacroscopicRoughnessRadians =
                                            static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.macroscopicRoughnessRadians:0.42),
                                        .smallBodySeed =
                                            static_cast<u32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.seed&0xffffffffULL:1ULL),
                                        .stellar =
                                            radiativeEmitter,
                                        .stellarColorLinear =
                                            resolvedRadiativeForView.has_value()
                                                ? math::Float3{
                                                      static_cast<f32>(
                                                          resolvedRadiativeForView->
                                                              stellarColorLinear.x),
                                                      static_cast<f32>(
                                                          resolvedRadiativeForView->
                                                              stellarColorLinear.y),
                                                      static_cast<f32>(
                                                          resolvedRadiativeForView->
                                                              stellarColorLinear.z)}
                                                : math::Float3{
                                                      1.0F, 1.0F, 1.0F},
                                        .stellarLimbDarkening =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          limbDarkening
                                                    : 0.58),
                                        .stellarGranulationStrength =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          granulationStrength
                                                    : 0.10),
                                        .stellarGranulationScale =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          granulationScale
                                                    : 42.0),
                                        .stellarActivityLevel =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          activityLevel
                                                    : 0.12),
                                        .stellarActivitySeed =
                                            static_cast<u32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          activitySeed &
                                                          0xffffffffULL
                                                    : 1ULL),
                                        .stellarChromosphereStrength =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          chromosphereStrength
                                                    : 0.08),
                                        .stellarChromosphereExtent =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          chromosphereExtent
                                                    : 0.035),
                                        .stellarCoronaStrength =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          coronaStrength
                                                    : 0.025),
                                        .stellarCoronaExtent =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          coronaExtent
                                                    : 1.75),
                                        .stellarGlareStrength =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          glareStrength
                                                    : 0.35),
                                        .stellarGlareRadiusPixels =
                                            static_cast<f32>(
                                                resolvedRadiativeForView.has_value()
                                                    ? resolvedRadiativeForView->
                                                          stellarAppearance.
                                                          glareRadiusPixels
                                                    : 5.0)
                                    };

                                farBodyRenderer_.Draw(
                                    commands,
                                    *color,
                                    width,
                                    height,
                                    far,
                                    nullptr);
                                if (opacity >= 0.5F)
                                {
                                    farBodyRenderer_.DrawSurfaceData(
                                        commands,
                                        *bodySurfaceBaseRoughness,
                                        *bodySurfaceNormalMetallic,
                                        *bodySurfaceEmissionClass,
                                        width,
                                        height,
                                        far);
                                }
                            };

                        draw(
                            richer,
                            1.0F);

                        if (lower != richer)
                        {
                            draw(
                                lower,
                                lowerOpacity);
                        }
                    });
            }
            else
            {
                transitionDiagnostics_.erase(
                    info.id);

                graph.AddPass(
                    prefix + ".Body",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture = targets.surfaceBaseRoughness,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceNormalMetallic,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceEmissionClass,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     color,
                     bodySurfaceBaseRoughness,
                     bodySurfaceNormalMetallic,
                     bodySurfaceEmissionClass,
                     width,
                     height,
                     camera,
                     bodyShape,
                     bodyMaterial](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *bodySurfaceBaseRoughness,
                            {0.0F, 0.0F, 0.0F, 1.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceNormalMetallic,
                            {0.0F, 1.0F, 0.0F, 0.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceEmissionClass,
                            {0.0F, 0.0F, 0.0F, 0.0F});

                        bodyRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            bodyShape,
                            camera,
                            bodyMaterial);
                        bodyRenderer_.DrawSurfaceData(
                            commands,
                            *bodySurfaceBaseRoughness,
                            *bodySurfaceNormalMetallic,
                            *bodySurfaceEmissionClass,
                            width,
                            height,
                            bodyShape,
                            camera,
                            bodyMaterial);
                    });
            }

            break;
        }

        case StudioViewportPresentation::Blank:
        {
            transitionDiagnostics_.erase(
                info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            graph.AddPass(
                prefix + ".Blank",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.018F,
                            .green = 0.021F,
                            .blue = 0.027F,
                            .alpha = 1.0F
                        });
                });
            break;
        }
        }

        const bool usesPhysicalSurfaceLighting =
            compactObjectDiagnostics_.find(
                info.id) ==
                compactObjectDiagnostics_.end() &&
            (presentation ==
                 StudioViewportPresentation::ProductionTerrain ||
             presentation ==
                 StudioViewportPresentation::MacroGlobe ||
             presentation ==
                 StudioViewportPresentation::BodyPreview);

        if (usesPhysicalSurfaceLighting)
        {
            const auto physicalBodyObject =
                logicalTarget->target.has_value()
                    ? session.World().
                          Universe().
                          ObjectForBody(
                              logicalTarget->
                                  target->body)
                    : std::nullopt;

            const auto runtimeMaterialOverride =
                ResolveRuntimeBodyMaterialIfAssigned(
                    content_,
                    session.World().Objects(),
                    physicalBodyObject);

            auto* lightingBaseRoughness =
                &view->SurfaceBaseRoughness();
            auto* lightingNormalMetallic =
                &view->SurfaceNormalMetallic();
            auto* lightingEmissionClass =
                &view->SurfaceEmissionClass();
            auto* lightingDepth =
                &view->Depth();

            const auto lightingView =
                view->Lighting();

            const lighting::DirectionalLight
                directLight{
                    .directionToLight =
                        studioDirectLight.directionBody,
                    .colorLinear = {
                        1.0F,
                        1.0F,
                        1.0F
                    },
                    .irradianceScale =
                        studioDirectLight.irradianceScale
                };

            std::optional<scene::ObjectId>
                authoredLightRoot;

            if (logicalTarget->target.has_value() &&
                snapshot.hasWorld)
            {
                authoredLightRoot =
                    session.World().
                        Universe().
                        ObjectForBody(
                            logicalTarget->
                                target->body);
            }

            const auto authoredLights =
                world_model::
                    ResolveAuthoredLocalLights(
                        session.World().Objects(),
                        authoredLightRoot);

            std::vector<lighting::LocalLight>
                localLights;
            localLights.reserve(
                authoredLights.size());

            constexpr f64 kDegreesToRadians =
                0.017453292519943295769;

            for (const auto& authored :
                 authoredLights)
            {
                localLights.push_back({
                    .type =
                        authored.kind ==
                                world_model::
                                    AuthoredLightKind::
                                        Spot
                            ? lighting::
                                LocalLightType::
                                    Spot
                            : lighting::
                                LocalLightType::
                                    Point,
                    .positionInFrameMeters =
                        authored.positionMeters,
                    .direction = {
                        static_cast<f32>(
                            authored.direction.x),
                        static_cast<f32>(
                            authored.direction.y),
                        static_cast<f32>(
                            authored.direction.z)
                    },
                    .colorLinear = {
                        static_cast<f32>(
                            authored.colorLinear.x),
                        static_cast<f32>(
                            authored.colorLinear.y),
                        static_cast<f32>(
                            authored.colorLinear.z)
                    },
                    .luminousFluxLumens =
                        static_cast<f32>(
                            authored.
                                luminousFluxLumens),
                    .rangeMeters =
                        static_cast<f32>(
                            authored.rangeMeters),
                    .innerConeRadians =
                        static_cast<f32>(
                            authored.
                                innerConeDegrees *
                            kDegreesToRadians),
                    .outerConeRadians =
                        static_cast<f32>(
                            authored.
                                outerConeDegrees *
                            kDegreesToRadians),
                    .stableId =
                        authored.object.high ^
                        authored.object.low
                });
            }

            const lighting::TiledLightGrid
                localLightGrid =
                    lighting::BuildTiledLightGrid(
                        localLights,
                        lightingView,
                        width,
                        height);

            std::vector<lighting::GpuLocalLight>
                gpuLights;
            gpuLights.reserve(
                localLightGrid.lights.size());

            for (const auto& light :
                 localLightGrid.lights)
            {
                gpuLights.push_back(
                    lighting::
                        EncodeGpuLocalLight(
                            light));
            }

            const u64 lightBufferBytes =
                std::max<u64>(
                    sizeof(
                        lighting::GpuLocalLight),
                    static_cast<u64>(
                        gpuLights.size()) *
                        sizeof(
                            lighting::
                                GpuLocalLight));

            const u64 offsetBufferBytes =
                std::max<u64>(
                    sizeof(u32),
                    static_cast<u64>(
                        localLightGrid.
                            offsets.size()) *
                        sizeof(u32));

            const u64 indexBufferBytes =
                std::max<u64>(
                    sizeof(u32),
                    static_cast<u64>(
                        localLightGrid.
                            lightIndices.size()) *
                        sizeof(u32));

            const auto localLightsHandle =
                graph.CreateBuffer(
                    prefix + ".LocalLights",
                    {
                        .sizeBytes =
                            lightBufferBytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostVisible,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    });

            const auto localOffsetsHandle =
                graph.CreateBuffer(
                    prefix + ".LocalLightOffsets",
                    {
                        .sizeBytes =
                            offsetBufferBytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostVisible,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    });

            const auto localIndicesHandle =
                graph.CreateBuffer(
                    prefix + ".LocalLightIndices",
                    {
                        .sizeBytes =
                            indexBufferBytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostVisible,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    });

            const auto uploadBuffer =
                [](rhi::Buffer& buffer,
                   const void* source,
                   const u64 sourceBytes)
                {
                    auto* destination =
                        buffer.Map();

                    std::memset(
                        destination,
                        0,
                        static_cast<std::size_t>(
                            buffer.SizeBytes()));

                    if (source != nullptr &&
                        sourceBytes > 0U)
                    {
                        std::memcpy(
                            destination,
                            source,
                            static_cast<
                                std::size_t>(
                                    sourceBytes));
                    }

                    buffer.Unmap();
                };

            uploadBuffer(
                graph.Buffer(
                    localLightsHandle),
                gpuLights.empty()
                    ? nullptr
                    : gpuLights.data(),
                static_cast<u64>(
                    gpuLights.size()) *
                    sizeof(
                        lighting::
                            GpuLocalLight));

            uploadBuffer(
                graph.Buffer(
                    localOffsetsHandle),
                localLightGrid.offsets.empty()
                    ? nullptr
                    : localLightGrid.
                        offsets.data(),
                static_cast<u64>(
                    localLightGrid.
                        offsets.size()) *
                    sizeof(u32));

            uploadBuffer(
                graph.Buffer(
                    localIndicesHandle),
                localLightGrid.
                        lightIndices.empty()
                    ? nullptr
                    : localLightGrid.
                        lightIndices.data(),
                static_cast<u64>(
                    localLightGrid.
                        lightIndices.size()) *
                    sizeof(u32));

            auto& finalGather =
                finalGatherPresentations_[info.id];

            if (finalGather.radianceResidency == nullptr)
            {
                finalGather.radianceResidency =
                    std::make_unique<
                        lighting::RadianceClipmapResidency>(
                            lighting::RadianceClipmapConfig{});
            }

            u64 radianceSourceRevision =
                session.World().Objects().Revision();
            if (terrainRuntime.has_value())
            {
                const auto& radianceTerrainSource =
                    session.TerrainRuntime().TerrainSource(
                        *terrainRuntime);

                radianceSourceRevision =
                    CombineFingerprint(
                        radianceSourceRevision,
                        radianceTerrainSource.Revision());
            }

            static_cast<void>(
                finalGather.radianceResidency->ScrollTo(
                    lightingView,
                    lightingView.cameraPositionInFrameMeters,
                    radianceSourceRevision,
                    1.0F / 60.0F));

            lighting::VisibilityRegistry
                radianceVisibility;

            if (const auto proxyFound =
                    visibilityProxyPresentations_.find(
                        info.id);
                proxyFound !=
                        visibilityProxyPresentations_.end() &&
                    proxyFound->second.provider !=
                        nullptr)
            {
                radianceVisibility.Register(
                    *proxyFound->second.provider);
            }

            std::unique_ptr<
                lighting::AnalyticBodyVisibilityProvider>
                analyticVisibility;

            std::unique_ptr<
                lighting::TerrainHeightfieldVisibilityProvider>
                terrainVisibility;

            if (bodies != nullptr &&
                frames != nullptr)
            {
                analyticVisibility =
                    std::make_unique<
                        lighting::
                            AnalyticBodyVisibilityProvider>(
                                *bodies,
                                *frames,
                                atTime);

                radianceVisibility.Register(
                    *analyticVisibility);

                if (terrainRuntime.has_value())
                {
                    const auto& radianceTerrainSource =
                        session.TerrainRuntime().TerrainSource(
                            *terrainRuntime);

                    terrainVisibility =
                        std::make_unique<
                            lighting::
                                TerrainHeightfieldVisibilityProvider>(
                                    terrainRuntime->body,
                                    terrainRuntime->planet,
                                    radianceTerrainSource,
                                    *bodies,
                                    *frames,
                                    lighting::
                                        TerrainVisibilityConfig{},
                                    atTime);

                    radianceVisibility.Register(
                        *terrainVisibility);
                }
            }

            lighting::RadianceEstimateSettings
                radianceEstimateSettings{};

            if (const auto atmosphereFound =
                    atmospherePresentations_.find(
                        info.id);
                atmosphereFound !=
                        atmospherePresentations_.end() &&
                    atmosphereFound->second.skyView !=
                        nullptr)
            {
                const auto skyIrradiance =
                    AtmosphereSkyIrradianceSummary(
                        atmosphereFound->second.
                            skyView.get());

                if (skyIrradiance.x > 0.0F ||
                    skyIrradiance.y > 0.0F ||
                    skyIrradiance.z > 0.0F)
                {
                    radianceEstimateSettings.
                        ambientIrradianceScale =
                            0.0F;
                    radianceEstimateSettings.
                        skyIrradianceLinear =
                            skyIrradiance;
                }
            }

            std::vector<
                lighting::EmissiveVolumeSource>
                emissiveVolumes;

            if (resolvedMagnetosphereForView.has_value() &&
                shape.has_value())
            {
                const f64 referenceRadius =
                    ReferenceRadiusForShape(
                        *shape);

                const f64 outerRadius =
                    referenceRadius +
                    resolvedMagnetosphereForView->
                        parameters.
                        auroralMaximumAltitudeMeters;

                const f64 shellThickness =
                    std::max(
                        resolvedMagnetosphereForView->
                            parameters.
                            auroralMaximumAltitudeMeters -
                        resolvedMagnetosphereForView->
                            parameters.
                            auroralMinimumAltitudeMeters,
                        1.0);

                const auto& p =
                    resolvedMagnetosphereForView->
                        parameters;

                emissiveVolumes.push_back({
                    .centerInFrameMeters =
                        {0.0, 0.0, 0.0},
                    .radiusMeters =
                        static_cast<f32>(
                            outerRadius),
                    .emissionLinear = {
                        static_cast<f32>(
                            p.auroralColorLinear.x),
                        static_cast<f32>(
                            p.auroralColorLinear.y),
                        static_cast<f32>(
                            p.auroralColorLinear.z)
                    },
                    .intensityScale =
                        static_cast<f32>(
                            p.auroralIntensity *
                            (0.22 +
                             0.78 * p.activity) *
                            std::clamp(
                                shellThickness /
                                    std::max(
                                        referenceRadius,
                                        1.0),
                                0.002,
                                0.08)),
                    .influenceRangeMeters =
                        static_cast<f32>(
                            std::max(
                                referenceRadius *
                                    0.45,
                                shellThickness *
                                    18.0)),
                    .stableId =
                        resolvedMagnetosphereForView->
                            capability.high ^
                        resolvedMagnetosphereForView->
                            capability.low
                });
            }

            std::vector<
                lighting::DynamicEmissiveSourceState>
                dynamicEmissiveSources;
            dynamicEmissiveSources.reserve(
                emissiveVolumes.size());

            for (const auto& volume :
                 emissiveVolumes)
            {
                u64 revision =
                    CombineFingerprint(
                        volume.stableId,
                        QuantizedLightingFingerprintValue(
                            volume.intensityScale,
                            0.0025F));

                revision =
                    CombineFingerprint(
                        revision,
                        QuantizedLightingFingerprintValue(
                            volume.emissionLinear.x,
                            0.0025F));
                revision =
                    CombineFingerprint(
                        revision,
                        QuantizedLightingFingerprintValue(
                            volume.emissionLinear.y,
                            0.0025F));
                revision =
                    CombineFingerprint(
                        revision,
                        QuantizedLightingFingerprintValue(
                            volume.emissionLinear.z,
                            0.0025F));
                revision =
                    CombineFingerprint(
                        revision,
                        QuantizedLightingFingerprintValue(
                            volume.radiusMeters,
                            0.05F));
                revision =
                    CombineFingerprint(
                        revision,
                        QuantizedLightingFingerprintValue(
                            volume.influenceRangeMeters,
                            0.05F));

                dynamicEmissiveSources.push_back({
                    .stableId =
                        volume.stableId,
                    .contentRevision =
                        revision,
                    .centerInFrameMeters =
                        volume.centerInFrameMeters,
                    .sourceRadiusMeters =
                        std::max<f64>(
                            volume.radiusMeters,
                            0.0F),
                    .influenceRangeMeters =
                        std::max<f64>(
                            volume.influenceRangeMeters,
                            0.0F)
                });
            }

            u64 emissionAuthorityFingerprint =
                0x4f52424954454d31ULL;

            for (const auto& sourceState :
                 dynamicEmissiveSources)
            {
                emissionAuthorityFingerprint =
                    CombineFingerprint(
                        emissionAuthorityFingerprint,
                        sourceState.stableId);
                emissionAuthorityFingerprint =
                    CombineFingerprint(
                        emissionAuthorityFingerprint,
                        sourceState.contentRevision);
            }

            if (runtimeMaterialOverride.has_value())
            {
                const auto runtimeEmission =
                    runtimeMaterialOverride->emissionRadiance;

                emissionAuthorityFingerprint =
                    CombineFingerprint(
                        emissionAuthorityFingerprint,
                        QuantizedLightingFingerprintValue(
                            runtimeEmission.x,
                            0.0025F));
                emissionAuthorityFingerprint =
                    CombineFingerprint(
                        emissionAuthorityFingerprint,
                        QuantizedLightingFingerprintValue(
                            runtimeEmission.y,
                            0.0025F));
                emissionAuthorityFingerprint =
                    CombineFingerprint(
                        emissionAuthorityFingerprint,
                        QuantizedLightingFingerprintValue(
                            runtimeEmission.z,
                            0.0025F));
            }

            const auto emissiveInvalidations =
                finalGather.
                    emissiveInvalidationTracker.
                    Update(
                        dynamicEmissiveSources);

            if (!emissiveInvalidations.empty())
            {
                lighting::ApplyEmissiveInvalidations(
                    *finalGather.radianceResidency,
                    emissiveInvalidations,
                    radianceSourceRevision,
                    8.0F);
            }

            u64 lightingFingerprint =
                0x4f52424954474931ULL;

            const auto addLightingValue =
                [&](const f32 value,
                    const f32 quantum)
                {
                    lightingFingerprint =
                        CombineFingerprint(
                            lightingFingerprint,
                            QuantizedLightingFingerprintValue(
                                value,
                                quantum));
                };

            addLightingValue(
                directLight.directionToLight.x,
                0.005F);
            addLightingValue(
                directLight.directionToLight.y,
                0.005F);
            addLightingValue(
                directLight.directionToLight.z,
                0.005F);
            addLightingValue(
                directLight.irradianceScale,
                0.01F);
            addLightingValue(
                directLight.colorLinear.x,
                0.01F);
            addLightingValue(
                directLight.colorLinear.y,
                0.01F);
            addLightingValue(
                directLight.colorLinear.z,
                0.01F);

            addLightingValue(
                radianceEstimateSettings.
                    skyIrradianceLinear.x,
                0.002F);
            addLightingValue(
                radianceEstimateSettings.
                    skyIrradianceLinear.y,
                0.002F);
            addLightingValue(
                radianceEstimateSettings.
                    skyIrradianceLinear.z,
                0.002F);

            for (const auto& light :
                 localLightGrid.lights)
            {
                lightingFingerprint =
                    CombineFingerprint(
                        lightingFingerprint,
                        light.stableId);

                addLightingValue(
                    light.positionCameraRelativeMeters.x,
                    0.05F);
                addLightingValue(
                    light.positionCameraRelativeMeters.y,
                    0.05F);
                addLightingValue(
                    light.positionCameraRelativeMeters.z,
                    0.05F);
                addLightingValue(
                    light.luminousFluxLumens,
                    5.0F);
            }

            const auto radianceRefreshReason =
                lighting::EvaluateRadianceRefresh(
                    finalGather.lightingFingerprint,
                    lightingFingerprint,
                    finalGather.previousView.frame,
                    lightingView.frame,
                    finalGather.previousView.body,
                    lightingView.body);

            const bool radianceCacheRefreshRequested =
                radianceRefreshReason !=
                    lighting::RadianceRefreshReason::None;

            if (radianceCacheRefreshRequested)
            {
                finalGather.radianceResidency->
                    RequestGlobalRefresh();
            }

            finalGather.lightingFingerprint =
                lightingFingerprint;

            if (auto transitionFound =
                    transitionDiagnostics_.find(info.id);
                transitionFound !=
                    transitionDiagnostics_.end())
            {
                auto& diagnostic =
                    transitionFound->second;

                const std::array<
                    lighting::RepresentationLightingAuthority,
                    2U>
                    authorities{{
                        {
                            .representation =
                                diagnostic.representation,
                            .weight =
                                diagnostic.richerWeight,
                            .directLightingFingerprint =
                                lightingFingerprint,
                            .emissionAuthorityFingerprint =
                                emissionAuthorityFingerprint,
                            .radianceFrame =
                                lightingView.frame,
                            .radianceBody =
                                lightingView.body
                        },
                        {
                            .representation =
                                diagnostic.lowerFidelityNeighbor,
                            .weight =
                                diagnostic.lowerWeight,
                            .directLightingFingerprint =
                                lightingFingerprint,
                            .emissionAuthorityFingerprint =
                                emissionAuthorityFingerprint,
                            .radianceFrame =
                                lightingView.frame,
                            .radianceBody =
                                lightingView.body
                        }
                    }};

                const auto continuity =
                    lighting::EvaluateLightingContinuity(
                        authorities);

                diagnostic.directLightingFingerprint =
                    lightingFingerprint;
                diagnostic.emissionAuthorityFingerprint =
                    emissionAuthorityFingerprint;
                diagnostic.directLightingCoherent =
                    continuity.directLightingCoherent;
                diagnostic.emissionAuthorityCoherent =
                    continuity.emissionAuthorityCoherent;
                diagnostic.broadIndirectCoherent =
                    continuity.radianceIdentityCoherent;
                diagnostic.continuityPassed =
                    continuity.Passed();
                diagnostic.radianceCacheRefreshRequested =
                    radianceCacheRefreshRequested;
            }

            const auto radianceUpdates =
                finalGather.radianceResidency->BuildUpdateList(
                    lightingView.cameraPositionInFrameMeters,
                    lightingPlan.radianceCacheUpdates);

            const auto radianceStats =
                finalGather.radianceResidency->Stats();

            emissiveGiDiagnostics_.insert_or_assign(
                info.id,
                StudioEmissiveGiDiagnostics{
                    .trackedSources =
                        static_cast<u32>(
                            finalGather.
                                emissiveInvalidationTracker.
                                SourceCount()),
                    .invalidationEventsThisFrame =
                        static_cast<u32>(
                            emissiveInvalidations.size()),
                    .dirtyRadianceCells =
                        radianceStats.dirtyCells,
                    .scheduledRadianceUpdates =
                        static_cast<u32>(
                            radianceUpdates.size())
                });

            for (const auto& update : radianceUpdates)
            {
                const auto estimate =
                    lighting::EstimateRadianceCell(
                        update.key,
                        finalGather.radianceResidency->Config(),
                        lightingView,
                        directLight,
                        localLightGrid.lights,
                        &radianceVisibility,
                        radianceEstimateSettings,
                        emissiveVolumes);

                static_cast<void>(
                    finalGather.radianceResidency->CommitUpdate(
                        update.key,
                        estimate,
                        1U,
                        radianceSourceRevision));
            }

            const auto radianceSnapshot =
                finalGather.radianceResidency->BuildGpuSnapshot(
                    lightingView);

            const u64 radianceCellBytes =
                std::max<u64>(
                    sizeof(lighting::GpuRadianceCell),
                    static_cast<u64>(
                        radianceSnapshot.cells.size()) *
                        sizeof(lighting::GpuRadianceCell));

            const u64 radianceLevelBytes =
                std::max<u64>(
                    sizeof(lighting::GpuRadianceLevelInfo),
                    static_cast<u64>(
                        radianceSnapshot.levels.size()) *
                        sizeof(lighting::GpuRadianceLevelInfo));

            const auto radianceCellsHandle =
                graph.CreateBuffer(
                    prefix + ".RadianceCacheCells",
                    {
                        .sizeBytes = radianceCellBytes,
                        .usage = rhi::BufferUsage::Structured,
                        .memory = rhi::MemoryUsage::HostVisible,
                        .initialState =
                            rhi::ResourceState::ShaderResource
                    });

            const auto radianceLevelsHandle =
                graph.CreateBuffer(
                    prefix + ".RadianceCacheLevels",
                    {
                        .sizeBytes = radianceLevelBytes,
                        .usage = rhi::BufferUsage::Structured,
                        .memory = rhi::MemoryUsage::HostVisible,
                        .initialState =
                            rhi::ResourceState::ShaderResource
                    });

            uploadBuffer(
                graph.Buffer(radianceCellsHandle),
                radianceSnapshot.cells.empty()
                    ? nullptr
                    : radianceSnapshot.cells.data(),
                static_cast<u64>(
                    radianceSnapshot.cells.size()) *
                    sizeof(lighting::GpuRadianceCell));

            uploadBuffer(
                graph.Buffer(radianceLevelsHandle),
                radianceSnapshot.levels.empty()
                    ? nullptr
                    : radianceSnapshot.levels.data(),
                static_cast<u64>(
                    radianceSnapshot.levels.size()) *
                    sizeof(lighting::GpuRadianceLevelInfo));

            const u32 radianceLevelCount =
                static_cast<u32>(
                    radianceSnapshot.levels.size());

            if (runtimeMaterialOverride.has_value() &&
                presentation !=
                    StudioViewportPresentation::BodyPreview)
            {
                const auto emission =
                    runtimeMaterialOverride->
                        emissionRadiance;

                if (emission.x > 0.0F ||
                    emission.y > 0.0F ||
                    emission.z > 0.0F)
                {
                    graph.AddPass(
                        prefix +
                            ".RuntimeMaterialEmissionOverride",
                        {
                            {
                                .texture =
                                    targets.
                                        surfaceEmissionClass,
                                .state =
                                    rhi::ResourceState::
                                        UnorderedAccess,
                                .access =
                                    render_graph::Access::
                                        Write
                            }
                        },
                        [this,
                         lightingEmissionClass,
                         width,
                         height,
                         emission](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            materialEmissionSurfaceOverride_.
                                Apply(
                                    commands,
                                    *lightingEmissionClass,
                                    width,
                                    height,
                                    emission);
                        });
                }
            }

            graph.AddPass(
                prefix + ".SharedDirectLighting",
                {
                    {
                        .texture =
                            targets.surfaceBaseRoughness,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.surfaceNormalMetallic,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.surfaceEmissionClass,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::
                                DepthRead,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                {
                    {
                        .buffer =
                            localLightsHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            localOffsetsHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            localIndicesHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    }
                },
                [this,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 color,
                 width,
                 height,
                 lightingView,
                 directLight,
                 localLightGrid,
                 localLightsHandle,
                 localOffsetsHandle,
                 localIndicesHandle,
                 lightingTimestamps,
                 frameIndex](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->BeginSection(
                            commands,
                            frameIndex,
                            lighting::LightingGpuSection::Direct);
                    }

                    directLightingRenderer_.Draw(
                        commands,
                        *lightingBaseRoughness,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        *lightingDepth,
                        resources.Buffer(
                            localLightsHandle),
                        resources.Buffer(
                            localOffsetsHandle),
                        resources.Buffer(
                            localIndicesHandle),
                        *color,
                        width,
                        height,
                        lightingView,
                        directLight,
                        localLightGrid);

                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->EndSection(
                            commands,
                            frameIndex,
                            lighting::LightingGpuSection::Direct);
                    }
                });

            if (finalGather.width != width ||
                finalGather.height != height ||
                finalGather.indirectA == nullptr ||
                finalGather.indirectB == nullptr ||
                finalGather.metaA == nullptr ||
                finalGather.metaB == nullptr ||
                finalGather.scratch == nullptr)
            {
                const auto createGatherTexture =
                    [this, width, height]()
                    {
                        return device_->CreateTexture({
                            .width = width,
                            .height = height,
                            .format =
                                rhi::TextureFormat::
                                    RGBA16_Float,
                            .initialState =
                                rhi::ResourceState::
                                    ShaderResource,
                            .allowUnorderedAccess =
                                true
                        });
                    };

                finalGather.width = width;
                finalGather.height = height;
                finalGather.writeA = true;
                finalGather.hasHistory = false;
                finalGather.previousView = {};

                finalGather.indirectA =
                    createGatherTexture();
                finalGather.indirectB =
                    createGatherTexture();
                finalGather.metaA =
                    createGatherTexture();
                finalGather.metaB =
                    createGatherTexture();
                finalGather.scratch =
                    createGatherTexture();
            }

            auto* currentIndirect =
                finalGather.writeA
                    ? finalGather.indirectA.get()
                    : finalGather.indirectB.get();

            auto* previousIndirect =
                finalGather.writeA
                    ? finalGather.indirectB.get()
                    : finalGather.indirectA.get();

            auto* currentMeta =
                finalGather.writeA
                    ? finalGather.metaA.get()
                    : finalGather.metaB.get();

            auto* previousMeta =
                finalGather.writeA
                    ? finalGather.metaB.get()
                    : finalGather.metaA.get();

            auto* gatherScratch =
                finalGather.scratch.get();

            const bool historyCompatible =
                finalGather.hasHistory &&
                lighting::
                    CanReuseFinalGatherHistory(
                        finalGather.previousView,
                        lightingView);

            const auto currentIndirectHandle =
                graph.ImportTexture(
                    prefix +
                        ".FinalGather.CurrentIndirect",
                    *currentIndirect,
                    rhi::ResourceState::
                        ShaderResource);

            const auto previousIndirectHandle =
                graph.ImportTexture(
                    prefix +
                        ".FinalGather.PreviousIndirect",
                    *previousIndirect,
                    rhi::ResourceState::
                        ShaderResource);

            const auto currentMetaHandle =
                graph.ImportTexture(
                    prefix +
                        ".FinalGather.CurrentMeta",
                    *currentMeta,
                    rhi::ResourceState::
                        ShaderResource);

            const auto previousMetaHandle =
                graph.ImportTexture(
                    prefix +
                        ".FinalGather.PreviousMeta",
                    *previousMeta,
                    rhi::ResourceState::
                        ShaderResource);

            const auto gatherScratchHandle =
                graph.ImportTexture(
                    prefix +
                        ".FinalGather.Scratch",
                    *gatherScratch,
                    rhi::ResourceState::
                        ShaderResource);

            lighting::
                ScreenSpaceFinalGatherSettings
                    gatherSettings;

            gatherSettings.stepsPerRay =
                std::clamp(
                    static_cast<u32>(
                        std::lround(
                            2.0F +
                            8.0F *
                                std::clamp(
                                    lightingPlan.giScale,
                                    0.0F,
                                    1.0F))),
                    2U,
                    10U);

            graph.AddPass(
                prefix + ".ScreenSpaceFinalGather",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceBaseRoughness,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceNormalMetallic,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.
                                surfaceEmissionClass,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::
                                DepthRead,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            previousIndirectHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            previousMetaHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            currentIndirectHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .texture =
                            currentMetaHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 lightingBaseRoughness,
                 lightingNormalMetallic,
                 lightingEmissionClass,
                 lightingDepth,
                 previousIndirect,
                 previousMeta,
                 currentIndirect,
                 currentMeta,
                 width,
                 height,
                 lightingView,
                 historyCompatible,
                 gatherSettings,
                 lightingTimestamps,
                 frameIndex](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->
                            BeginSection(
                                commands,
                                frameIndex,
                                lighting::
                                    LightingGpuSection::
                                        Gi);
                    }

                    finalGatherRenderer_.Gather(
                        commands,
                        *color,
                        *lightingBaseRoughness,
                        *lightingNormalMetallic,
                        *lightingEmissionClass,
                        *lightingDepth,
                        *previousIndirect,
                        *previousMeta,
                        *currentIndirect,
                        *currentMeta,
                        width,
                        height,
                        lightingView,
                        historyCompatible,
                        gatherSettings);
                });

            if (radianceLevelCount > 0U)
            {
                graph.AddPass(
                    prefix + ".RadianceCacheFallback",
                    {
                        {
                            .texture =
                                currentIndirectHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        },
                        {
                            .texture =
                                targets.
                                    surfaceBaseRoughness,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.
                                    surfaceNormalMetallic,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.depth,
                            .state =
                                rhi::ResourceState::
                                    DepthRead,
                            .access =
                                render_graph::Access::
                                    Read
                        }
                    },
                    {
                        {
                            .buffer =
                                radianceCellsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .buffer =
                                radianceLevelsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        }
                    },
                    [this,
                     currentIndirect,
                     lightingBaseRoughness,
                     lightingNormalMetallic,
                     lightingDepth,
                     radianceCellsHandle,
                     radianceLevelsHandle,
                     radianceLevelCount,
                     width,
                     height,
                     lightingView](
                        rhi::CommandList& commands,
                        const render_graph::Resources&
                            resources)
                    {
                        radianceCacheSampler_.
                            ResolveFallback(
                                commands,
                                *currentIndirect,
                                *lightingBaseRoughness,
                                *lightingNormalMetallic,
                                *lightingDepth,
                                resources.Buffer(
                                    radianceCellsHandle),
                                resources.Buffer(
                                    radianceLevelsHandle),
                                radianceLevelCount,
                                width,
                                height,
                                lightingView);
                    });
            }

            graph.AddPass(
                prefix + ".FinalGatherCombine",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            currentIndirectHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            gatherScratchHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 currentIndirect,
                 gatherScratch,
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    finalGatherRenderer_.Combine(
                        commands,
                        *color,
                        *currentIndirect,
                        *gatherScratch,
                        width,
                        height);
                });

            graph.AddPass(
                prefix + ".FinalGatherCopyBack",
                {
                    {
                        .texture =
                            gatherScratchHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 gatherScratch,
                 color,
                 width,
                 height,
                 lightingTimestamps,
                 frameIndex](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    debugComposite_.Draw(
                        commands,
                        *gatherScratch,
                        *color,
                        width,
                        height);

                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->
                            EndSection(
                                commands,
                                frameIndex,
                                lighting::
                                    LightingGpuSection::
                                        Gi);
                    }
                });

            if (radianceLevelCount > 0U)
            {
                lighting::HardwareRayQueryVisibilityBatch*
                    exactReflectionHardware = nullptr;

                if (const auto proxyFound =
                        visibilityProxyPresentations_.find(
                            info.id);
                    proxyFound !=
                            visibilityProxyPresentations_.end() &&
                        proxyFound->second.hardware != nullptr &&
                        proxyFound->second.hardware->Ready())
                {
                    exactReflectionHardware =
                        proxyFound->second.hardware.get();
                }

                const u32 maximumExactReflectionQueries =
                    std::min(
                        lightingPlan.exactVisibilityQueries,
                        lightingPlan.reflectionQueries);
                const auto exactSceneOrigin =
                    exactReflectionHardware != nullptr
                        ? exactReflectionHardware->
                              GpuOriginInFrameMeters()
                        : lightingView.
                              gpuOriginInFrameMeters;

                const math::Float3
                    currentToExactSceneOrigin{
                        static_cast<f32>(
                            lightingView.
                                gpuOriginInFrameMeters.x -
                            exactSceneOrigin.x),
                        static_cast<f32>(
                            lightingView.
                                gpuOriginInFrameMeters.y -
                            exactSceneOrigin.y),
                        static_cast<f32>(
                            lightingView.
                                gpuOriginInFrameMeters.z -
                            exactSceneOrigin.z)
                    };

                const math::Float3
                    exactSceneToCurrentOrigin{
                        -currentToExactSceneOrigin.x,
                        -currentToExactSceneOrigin.y,
                        -currentToExactSceneOrigin.z
                    };


                render_graph::BufferHandle
                    exactReflectionQueriesHandle{};
                render_graph::BufferHandle
                    exactReflectionResultsHandle{};
                render_graph::BufferHandle
                    exactReflectionPixelMapHandle{};
                render_graph::BufferHandle
                    exactReflectionCounterHandle{};

                if (exactReflectionHardware != nullptr &&
                    maximumExactReflectionQueries > 0U)
                {
                    exactReflectionQueriesHandle =
                        graph.CreateBuffer(
                            prefix +
                                ".ExactReflectionQueries",
                            {
                                .sizeBytes =
                                    static_cast<u64>(
                                        maximumExactReflectionQueries) *
                                    sizeof(
                                        lighting::
                                            GpuVisibilityQuery),
                                .usage =
                                    rhi::BufferUsage::
                                        Structured,
                                .memory =
                                    rhi::MemoryUsage::
                                        HostVisible,
                                .initialState =
                                    rhi::ResourceState::
                                        ShaderResource
                            });

                    exactReflectionResultsHandle =
                        graph.CreateBuffer(
                            prefix +
                                ".ExactReflectionResults",
                            {
                                .sizeBytes =
                                    static_cast<u64>(
                                        maximumExactReflectionQueries) *
                                    sizeof(
                                        lighting::
                                            GpuVisibilityResult),
                                .usage =
                                    rhi::BufferUsage::
                                        Structured,
                                .memory =
                                    rhi::MemoryUsage::
                                        HostVisible,
                                .initialState =
                                    rhi::ResourceState::
                                        ShaderResource
                            });

                    exactReflectionPixelMapHandle =
                        graph.CreateBuffer(
                            prefix +
                                ".ExactReflectionPixelMap",
                            {
                                .sizeBytes =
                                    static_cast<u64>(
                                        maximumExactReflectionQueries) *
                                    sizeof(u32),
                                .usage =
                                    rhi::BufferUsage::
                                        Structured,
                                .memory =
                                    rhi::MemoryUsage::
                                        HostVisible,
                                .initialState =
                                    rhi::ResourceState::
                                        ShaderResource
                            });

                    exactReflectionCounterHandle =
                        graph.CreateBuffer(
                            prefix +
                                ".ExactReflectionCounter",
                            {
                                .sizeBytes = sizeof(u32),
                                .usage =
                                    rhi::BufferUsage::
                                        Structured,
                                .memory =
                                    rhi::MemoryUsage::
                                        HostVisible,
                                .initialState =
                                    rhi::ResourceState::
                                        ShaderResource
                            });

                    std::vector<
                        lighting::GpuVisibilityQuery>
                        safeQueries(
                            maximumExactReflectionQueries);

                    for (auto& safeQuery : safeQueries)
                    {
                        safeQuery.originMinimumDistance.w =
                            0.03F;
                        safeQuery.directionMaximumDistance =
                            {0.0F, 0.0F, 1.0F, 0.03F};
                        safeQuery.requirements = {
                            std::numeric_limits<f32>::
                                infinity(),
                            0.0F,
                            0.0F,
                            std::bit_cast<f32>(3U)
                        };
                    }

                    uploadBuffer(
                        graph.Buffer(
                            exactReflectionQueriesHandle),
                        safeQueries.data(),
                        static_cast<u64>(
                            safeQueries.size()) *
                            sizeof(
                                lighting::
                                    GpuVisibilityQuery));

                    std::vector<u32>
                        emptyPixelMap(
                            maximumExactReflectionQueries,
                            0xFFFF'FFFFU);

                    uploadBuffer(
                        graph.Buffer(
                            exactReflectionPixelMapHandle),
                        emptyPixelMap.data(),
                        static_cast<u64>(
                            emptyPixelMap.size()) *
                            sizeof(u32));

                    const u32 zero = 0U;
                    uploadBuffer(
                        graph.Buffer(
                            exactReflectionCounterHandle),
                        &zero,
                        sizeof(zero));

                    std::vector<
                        lighting::GpuVisibilityResult>
                        emptyResults(
                            maximumExactReflectionQueries);

                    uploadBuffer(
                        graph.Buffer(
                            exactReflectionResultsHandle),
                        emptyResults.data(),
                        static_cast<u64>(
                            emptyResults.size()) *
                            sizeof(
                                lighting::
                                    GpuVisibilityResult));
                }

                graph.AddPass(
                    prefix + ".HybridReflections",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.
                                    surfaceBaseRoughness,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                targets.
                                    surfaceNormalMetallic,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture = targets.depth,
                            .state =
                                rhi::ResourceState::
                                    DepthRead,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture =
                                gatherScratchHandle,
                            .state =
                                rhi::ResourceState::
                                    UnorderedAccess,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    {
                        {
                            .buffer =
                                radianceCellsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .buffer =
                                radianceLevelsHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        }
                    },
                    [this,
                     color,
                     lightingBaseRoughness,
                     lightingNormalMetallic,
                     lightingDepth,
                     gatherScratch,
                     radianceCellsHandle,
                     radianceLevelsHandle,
                     radianceLevelCount,
                     width,
                     height,
                     lightingView,
                     lightingPlan,
                     lightingTimestamps,
                     frameIndex](
                        rhi::CommandList& commands,
                        const render_graph::Resources&
                            resources)
                    {
                        if (lightingTimestamps != nullptr)
                        {
                            lightingTimestamps->
                                BeginSection(
                                    commands,
                                    frameIndex,
                                    lighting::
                                        LightingGpuSection::
                                            Reflections);
                        }

                        hybridReflectionRenderer_.
                            Resolve(
                                commands,
                                *color,
                                *lightingBaseRoughness,
                                *lightingNormalMetallic,
                                *lightingDepth,
                                resources.Buffer(
                                    radianceCellsHandle),
                                resources.Buffer(
                                    radianceLevelsHandle),
                                radianceLevelCount,
                                *gatherScratch,
                                width,
                                height,
                                lightingView,
                                lightingPlan.
                                    reflectionScale);
                    });

                if (exactReflectionHardware != nullptr &&
                    maximumExactReflectionQueries > 0U)
                {
                    graph.AddPass(
                        prefix +
                            ".ExactReflectionCompact",
                        {
                            {
                                .texture =
                                    targets.
                                        surfaceBaseRoughness,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .texture =
                                    targets.
                                        surfaceNormalMetallic,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .texture =
                                    targets.depth,
                                .state =
                                    rhi::ResourceState::
                                        DepthRead,
                                .access =
                                    render_graph::Access::
                                        Read
                            }
                        },
                        {
                            {
                                .buffer =
                                    exactReflectionQueriesHandle,
                                .state =
                                    rhi::ResourceState::
                                        UnorderedAccess,
                                .access =
                                    render_graph::Access::
                                        Write
                            },
                            {
                                .buffer =
                                    exactReflectionPixelMapHandle,
                                .state =
                                    rhi::ResourceState::
                                        UnorderedAccess,
                                .access =
                                    render_graph::Access::
                                        Write
                            },
                            {
                                .buffer =
                                    exactReflectionCounterHandle,
                                .state =
                                    rhi::ResourceState::
                                        UnorderedAccess,
                                .access =
                                    render_graph::Access::
                                        Write
                            }
                        },
                        [this,
                         lightingBaseRoughness,
                         lightingNormalMetallic,
                         lightingDepth,
                         exactReflectionQueriesHandle,
                         exactReflectionPixelMapHandle,
                         exactReflectionCounterHandle,
                         maximumExactReflectionQueries,
                         width,
                         height,
                         lightingView,
                         currentToExactSceneOrigin,
                         lightingPlan](
                            rhi::CommandList& commands,
                            const render_graph::Resources&
                                resources)
                        {
                            const u32 screenSteps =
                                std::clamp(
                                    static_cast<u32>(
                                        std::lround(
                                            4.0F +
                                            12.0F *
                                                lightingPlan.
                                                    reflectionScale)),
                                    4U,
                                    16U);

                            exactReflectionQueryRenderer_.
                                BuildQueries(
                                    commands,
                                    *lightingBaseRoughness,
                                    *lightingNormalMetallic,
                                    *lightingDepth,
                                    resources.Buffer(
                                        exactReflectionQueriesHandle),
                                    resources.Buffer(
                                        exactReflectionPixelMapHandle),
                                    resources.Buffer(
                                        exactReflectionCounterHandle),
                                    maximumExactReflectionQueries,
                                    width,
                                    height,
                                    lightingView,
                                    currentToExactSceneOrigin,
                                    0.08F,
                                    40.0F,
                                    0.12F,
                                    screenSteps);
                        });

                    graph.AddPass(
                        prefix +
                            ".ExactReflectionTrace",
                        {},
                        {
                            {
                                .buffer =
                                    exactReflectionQueriesHandle,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .buffer =
                                    exactReflectionResultsHandle,
                                .state =
                                    rhi::ResourceState::
                                        UnorderedAccess,
                                .access =
                                    render_graph::Access::
                                        Write
                            }
                        },
                        [exactReflectionHardware,
                         exactReflectionQueriesHandle,
                         exactReflectionResultsHandle,
                         maximumExactReflectionQueries](
                            rhi::CommandList& commands,
                            const render_graph::Resources&
                                resources)
                        {
                            exactReflectionHardware->
                                Dispatch(
                                    commands,
                                    resources.Buffer(
                                        exactReflectionQueriesHandle),
                                    resources.Buffer(
                                        exactReflectionResultsHandle),
                                    maximumExactReflectionQueries);
                        });

                    graph.AddPass(
                        prefix +
                            ".ExactReflectionResolve",
                        {
                            {
                                .texture =
                                    gatherScratchHandle,
                                .state =
                                    rhi::ResourceState::
                                        UnorderedAccess,
                                .access =
                                    render_graph::Access::
                                        Write
                            },
                            {
                                .texture =
                                    targets.
                                        surfaceBaseRoughness,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .texture =
                                    targets.
                                        surfaceNormalMetallic,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .texture =
                                    targets.depth,
                                .state =
                                    rhi::ResourceState::
                                        DepthRead,
                                .access =
                                    render_graph::Access::
                                        Read
                            }
                        },
                        {
                            {
                                .buffer =
                                    exactReflectionResultsHandle,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .buffer =
                                    exactReflectionPixelMapHandle,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .buffer =
                                    radianceCellsHandle,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            },
                            {
                                .buffer =
                                    radianceLevelsHandle,
                                .state =
                                    rhi::ResourceState::
                                        ShaderResource,
                                .access =
                                    render_graph::Access::
                                        Read
                            }
                        },
                        [this,
                         gatherScratch,
                         lightingBaseRoughness,
                         lightingNormalMetallic,
                         lightingDepth,
                         exactReflectionResultsHandle,
                         exactReflectionPixelMapHandle,
                         radianceCellsHandle,
                         radianceLevelsHandle,
                         radianceLevelCount,
                         maximumExactReflectionQueries,
                         width,
                         height,
                         lightingView,
                         exactSceneToCurrentOrigin](
                            rhi::CommandList& commands,
                            const render_graph::Resources&
                                resources)
                        {
                            exactReflectionQueryRenderer_.
                                ResolveResults(
                                    commands,
                                    *gatherScratch,
                                    *lightingBaseRoughness,
                                    *lightingNormalMetallic,
                                    *lightingDepth,
                                    resources.Buffer(
                                        exactReflectionResultsHandle),
                                    resources.Buffer(
                                        exactReflectionPixelMapHandle),
                                    resources.Buffer(
                                        radianceCellsHandle),
                                    resources.Buffer(
                                        radianceLevelsHandle),
                                    radianceLevelCount,
                                    maximumExactReflectionQueries,
                                    width,
                                    height,
                                    lightingView,
                                    exactSceneToCurrentOrigin);
                        });
                }

                graph.AddPass(
                    prefix + ".HybridReflectionsCopyBack",
                    {
                        {
                            .texture =
                                gatherScratchHandle,
                            .state =
                                rhi::ResourceState::
                                    ShaderResource,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     gatherScratch,
                     color,
                     width,
                     height,
                     lightingTimestamps,
                     frameIndex](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        debugComposite_.Draw(
                            commands,
                            *gatherScratch,
                            *color,
                            width,
                            height);

                        if (lightingTimestamps != nullptr)
                        {
                            lightingTimestamps->
                                EndSection(
                                    commands,
                                    frameIndex,
                                    lighting::
                                        LightingGpuSection::
                                            Reflections);
                        }
                    });
            }

            graph.AddPass(
                prefix + ".FinalGatherRestoreHistory",
                {
                    {
                        .texture =
                            currentIndirectHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            currentMetaHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    }
                },
                [](
                    rhi::CommandList&,
                    const render_graph::Resources&)
                {
                });

            finalGather.previousView =
                lightingView;
            finalGather.hasHistory = true;
            finalGather.writeA =
                !finalGather.writeA;

            // RenderView imports depth as DepthWrite on the next frame.
            // Shared direct lighting samples it read-only, so close this frame
            // by returning the actual Vulkan image to that persistent state.
            graph.AddPass(
                prefix + ".RestoreDepthWrite",
                {
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::
                                DepthWrite,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [](
                    rhi::CommandList&,
                    const render_graph::Resources&)
                {
                });
        }

        if (activeRingMesh != nullptr &&
            resolvedRingSystemForView.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto* ringMesh =
                activeRingMesh;
            const auto ringCamera =
                view->Camera();

            const auto normal =
                math::Normalize(
                    resolvedRingSystemForView->
                        parameters.
                        planeNormalBody);

            const math::Float3 ringNormal{
                static_cast<f32>(normal.x),
                static_cast<f32>(normal.y),
                static_cast<f32>(normal.z)
            };

            const bool receiveBodyShadow =
                resolvedRingSystemForView->
                    parameters.
                    receiveBodyShadow;

            graph.AddPass(
                prefix + ".CelestialRings",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 ringMesh,
                 ringCamera,
                 ringNormal,
                 receiveBodyShadow,
                 studioDirectLight](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    ringRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        *ringMesh,
                        ringCamera,
                        ringNormal,
                        receiveBodyShadow,
                        celestial_rings::
                            RingRenderLighting{
                                .directionBody =
                                    studioDirectLight.
                                        directionBody,
                                .irradianceScale =
                                    studioDirectLight.
                                        irradianceScale
                            });
                });
        }

        if (activeAuroraMesh != nullptr &&
            resolvedMagnetosphereForView.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto* auroraMesh =
                activeAuroraMesh;
            const auto auroraCamera =
                view->Camera();
            // Mesh emission is already authored in scene-linear HDR and
            // includes auroralIntensity. Keep draw scaling neutral so intensity
            // is not applied twice.
            const f32 intensityScale =
                1.0F;

            graph.AddPass(
                prefix +
                    ".CelestialAurora",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 auroraMesh,
                 auroraCamera,
                 intensityScale](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    auroraRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        *auroraMesh,
                        auroraCamera,
                        intensityScale);
                });
        }

        if (presentation ==
                StudioViewportPresentation::BodyPreview &&
            frames != nullptr &&
            !products.empty())
        {
            const auto* frameGraph = frames;
            const auto pathProducts = products;
            const auto camera =
                view->Camera();

            graph.AddPass(
                prefix + ".Paths",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 camera,
                 frameGraph,
                 pathProducts,
                 atTime](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    pathRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        camera,
                        *frameGraph,
                        atTime,
                        pathProducts);
                });
        }

        if (terrainRuntime.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            const auto& source =
                session.TerrainRuntime().TerrainSource(
                    *terrainRuntime);

            std::vector<
                editor_ui::PreviewLine>
                overlayLines;

            auto overlay =
                views.TerrainAuthoringOverlay(info.id);

            if (!overlay.has_value())
            {
                overlay =
                    SelectedTerrainAuthoringOverlay(
                        session,
                        *terrainRuntime);
            }

            if (overlay.has_value() &&
                overlay->body ==
                    terrainRuntime->body)
            {
                auto lines =
                    BuildTerrainAuthoringOverlayLines(
                        *overlay,
                        *terrainRuntime,
                        source,
                        view->Camera());

                overlayLines.insert(
                    overlayLines.end(),
                    lines.begin(),
                    lines.end());
            }

            const auto diagnostics =
                views.TerrainDiagnosticOverlays(
                    info.id);

            if (diagnostics.authoredConstraints)
            {
                const auto constraints =
                    TerrainConstraintDiagnosticOverlays(
                        session,
                        *terrainRuntime);

                for (const auto& constraint :
                     constraints)
                {
                    auto lines =
                        BuildTerrainAuthoringOverlayLines(
                            constraint,
                            *terrainRuntime,
                            source,
                            view->Camera());

                    overlayLines.insert(
                        overlayLines.end(),
                        lines.begin(),
                        lines.end());
                }
            }

            if (diagnostics.Any())
            {
                const auto statuses =
                    session.
                        TerrainPhysicalPages().
                        Catalog(
                            terrainRuntime->
                                planet.id);

                std::vector<
                    StudioTerrainDiagnosticPage>
                    pages;

                pages.reserve(
                    statuses.size());

                for (const auto& status :
                     statuses)
                {
                    pages.push_back({
                        .status = status,
                        .snapshot =
                            session.
                                TerrainPhysicalPages().
                                Find(
                                    status.address)
                    });
                }

                auto lines =
                    BuildTerrainDiagnosticOverlayLines(
                        diagnostics,
                        *terrainRuntime,
                        source,
                        pages,
                        view->Camera());

                overlayLines.insert(
                    overlayLines.end(),
                    lines.begin(),
                    lines.end());
            }

            if (!overlayLines.empty())
            {
                const auto camera =
                    view->Camera();

                auto* overlayColor =
                    color;

                graph.AddPass(
                    prefix + ".TerrainOverlays",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     overlayColor,
                     width,
                     height,
                     camera,
                     overlayLines = std::move(overlayLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.
                            DrawCameraRelativeLines(
                                commands,
                                *overlayColor,
                                width,
                                height,
                                camera,
                                overlayLines);
                    });
            }
        }

        if (resolvedMagnetosphereForView.has_value() &&
            shape.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto magnetosphereLines =
                SelectedMagnetosphereDiagnosticLines(
                    session,
                    *resolvedMagnetosphereForView,
                    ReferenceRadiusForShape(*shape),
                    view->Camera());

            if (!magnetosphereLines.empty())
            {
                const auto camera = view->Camera();

                graph.AddPass(
                    prefix + ".MagnetosphereDiagnostics",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     magnetosphereLines =
                         std::move(magnetosphereLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.DrawCameraRelativeLines(
                            commands,
                            *color,
                            width,
                            height,
                            camera,
                            magnetosphereLines);
                    });
            }
        }

        {
            auto lightGizmoLines =
                SelectedLocalLightGizmoLines(
                    session,
                    view->Camera());

            if (!lightGizmoLines.empty())
            {
                const auto camera =
                    view->Camera();

                graph.AddPass(
                    prefix + ".LocalLightGizmo",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     lightGizmoLines =
                        std::move(
                            lightGizmoLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.
                            DrawCameraRelativeLines(
                                commands,
                                *color,
                                width,
                                height,
                                camera,
                                lightGizmoLines);
                    });
            }
        }

        if (volumeFields_ != nullptr &&
            session.World().HasWorld() &&
            session.World().Selection().Ordered().size() == 1U)
        {
            scene::ObjectId volumeObject =
                session.World().Selection().Ordered().front();

            if (const auto selectedRecord =
                    session.World().Objects().Find(
                        volumeObject);
                selectedRecord.has_value() &&
                (selectedRecord->type ==
                     world_model::kVolumeSourceType ||
                 selectedRecord->type ==
                     world_model::kVolumeEffectorType) &&
                selectedRecord->parent.has_value())
            {
                volumeObject =
                    *selectedRecord->parent;
            }

            const auto authoredVolume =
                world_model::ResolveVolumeDomain(
                    session.World().Objects(),
                    volumeObject);

            if (authoredVolume.has_value())
            {
                auto runtimeVolume =
                    *authoredVolume;

                if (surfaceVolumeSolver_ != nullptr)
                {
                    const auto& runtimeSettings =
                        surfaceVolumeSolver_->
                            Settings(
                                runtimeVolume.object);

                    if (runtimeSettings.followCamera &&
                        runtimeVolume.solverPolicy ==
                            world_model::
                                VolumeSolverPolicy::
                                    Surface2D5D)
                    {
                        runtimeVolume.centerMeters =
                            view->Camera().
                                localPositionMeters;
                    }
                }

                volumeFields_->RemoveMissing(
                    session.World().Objects());

                auto& fieldStorage =
                    volumeFields_->Ensure(
                        runtimeVolume);

                static_cast<void>(
                    volumeFields_->
                        SyncAuthoredInputs(
                            session.World().Objects(),
                            runtimeVolume.object));

                auto importedFields =
                    fieldStorage.Import(
                        graph,
                        prefix + ".VolumeFields");

                if (surfaceVolumeSolver_ != nullptr)
                {
                    surfaceVolumeSolver_->
                        RemoveMissing(
                            session.World().Objects());

                    surfaceVolumeSolver_->
                        AddPasses(
                            graph,
                            prefix + ".SurfaceVolumeSolver",
                            session.World().Objects(),
                            runtimeVolume,
                            fieldStorage,
                            importedFields,
                            frameIndex %
                                framesInFlight_);

                    const auto solverSettings =
                        surfaceVolumeSolver_->
                            Settings(
                                runtimeVolume.object);

                    if (solverSettings.debugView !=
                        volume_solver::
                            SurfaceVolumeDebugView::Off)
                    {
                        render_graph::BufferHandle
                            debugField{};

                        const auto desiredField =
                            solverSettings.debugView ==
                                    volume_solver::
                                        SurfaceVolumeDebugView::
                                            FieldSlice
                                ? solverSettings.debugField
                                : solverSettings.debugView ==
                                          volume_solver::
                                              SurfaceVolumeDebugView::
                                                  Velocity
                                    ? world_model::
                                          VolumeField::Velocity
                                    : world_model::
                                          VolumeField::Density;

                        for (const auto& channel :
                             importedFields.channels)
                        {
                            if (channel.field ==
                                desiredField)
                            {
                                debugField =
                                    channel.buffer;
                                break;
                            }
                        }

                        if (debugField.IsValid())
                        {
                            const auto fieldDiagnostics =
                                fieldStorage.Diagnostics();
                            const auto camera =
                                view->Camera();
                            const auto debugView =
                                solverSettings.debugView;
                            const auto debugLayer =
                                solverSettings.debugLayer;
                            const auto sliceAxis =
                                solverSettings.sliceAxis;
                            const auto fieldChannel =
                                desiredField;

                            graph.AddPass(
                                prefix +
                                    ".SurfaceVolumeDebug",
                                {
                                    {
                                        .texture =
                                            targets.color,
                                        .state =
                                            rhi::ResourceState::
                                                RenderTarget,
                                        .access =
                                            render_graph::Access::
                                                Write
                                    }
                                },
                                {
                                    {
                                        .buffer =
                                            debugField,
                                        .state =
                                            rhi::ResourceState::
                                                ShaderResource,
                                        .access =
                                            render_graph::Access::
                                                Read
                                    },
                                    {
                                        .buffer =
                                            importedFields.
                                                residency,
                                        .state =
                                            rhi::ResourceState::
                                                ShaderResource,
                                        .access =
                                            render_graph::Access::
                                                Read
                                    }
                                },
                                [this,
                                 color,
                                 width,
                                 height,
                                 camera,
                                 domain =
                                     runtimeVolume,
                                 fieldDiagnostics,
                                 debugView,
                                 sliceAxis,
                                 fieldChannel,
                                 debugLayer,
                                 debugField,
                                 residency =
                                     importedFields.
                                         residency](
                                    rhi::CommandList& commands,
                                    const render_graph::
                                        Resources& resources)
                                {
                                    surfaceVolumeDebugRenderer_.
                                        Draw(
                                            commands,
                                            *color,
                                            width,
                                            height,
                                            camera,
                                            domain,
                                            fieldDiagnostics,
                                            debugView,
                                            sliceAxis,
                                            fieldChannel,
                                            debugLayer,
                                            resources.
                                                Buffer(
                                                    debugField),
                                            resources.
                                                Buffer(
                                                    residency));
                                });
                        }

                        if (runtimeVolume.solverPolicy ==
                                world_model::
                                    VolumeSolverPolicy::
                                        Local3D &&
                            solverSettings.debugView ==
                                volume_solver::
                                    SurfaceVolumeDebugView::
                                        FieldSlice)
                        {
                            auto sliceLines =
                                VolumeSlicePlaneLines(
                                    runtimeVolume,
                                    view->Camera(),
                                    solverSettings.sliceAxis,
                                    solverSettings.debugLayer);

                            if (!sliceLines.empty())
                            {
                                const auto camera =
                                    view->Camera();

                                graph.AddPass(
                                    prefix +
                                        ".Local3DSlicePlane",
                                    {
                                        {
                                            .texture =
                                                targets.color,
                                            .state =
                                                rhi::ResourceState::
                                                    RenderTarget,
                                            .access =
                                                render_graph::Access::
                                                    Write
                                        }
                                    },
                                    [this,
                                     color,
                                     width,
                                     height,
                                     camera,
                                     sliceLines =
                                         std::move(
                                             sliceLines)](
                                        rhi::CommandList& commands,
                                        const render_graph::
                                            Resources&)
                                    {
                                        pathRenderer_.
                                            DrawCameraRelativeLines(
                                                commands,
                                                *color,
                                                width,
                                                height,
                                                camera,
                                                sliceLines);
                                    });
                            }
                        }
                    }
                }

                std::vector<render_graph::BufferUse>
                    fieldReads;
                fieldReads.reserve(
                    importedFields.channels.size() + 1U);

                for (const auto& channel :
                     importedFields.channels)
                {
                    fieldReads.push_back({
                        .buffer = channel.buffer,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    });
                }

                fieldReads.push_back({
                    .buffer =
                        importedFields.residency,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                });

                graph.AddPass(
                    prefix +
                        ".VolumeFieldsReady",
                    {},
                    std::move(fieldReads),
                    [](
                        rhi::CommandList&,
                        const render_graph::Resources&)
                    {
                    });
            }
        }

        {
            auto volumeInputLines =
                VolumeInputGizmoLines(
                    session,
                    view->Camera(),
                    volumeSourceDebugVisualization_);

            if (!volumeInputLines.empty())
            {
                const auto camera =
                    view->Camera();

                graph.AddPass(
                    prefix + ".VolumeInputGizmos",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     volumeInputLines =
                         std::move(
                             volumeInputLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.
                            DrawCameraRelativeLines(
                                commands,
                                *color,
                                width,
                                height,
                                camera,
                                volumeInputLines);
                    });
            }
        }

        {
            auto volumeDomainLines =
                SelectedVolumeDomainLines(
                    session,
                    view->Camera());

            if (!volumeDomainLines.empty())
            {
                const auto camera =
                    view->Camera();

                graph.AddPass(
                    prefix + ".VolumeDomainBounds",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     volumeDomainLines =
                         std::move(
                             volumeDomainLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.
                            DrawCameraRelativeLines(
                                commands,
                                *color,
                                width,
                                height,
                                camera,
                                volumeDomainLines);
                    });
            }
        }

        {
            auto& histogram =
                luminanceHistogramPresentations_[
                    info.id];

            const bool recreateHistogram =
                histogram.width != width ||
                histogram.height != height ||
                histogram.meteringMask == nullptr ||
                histogram.histogramReadback.size() !=
                    framesInFlight_ ||
                histogram.statisticsReadback.size() !=
                    framesInFlight_;

            if (recreateHistogram)
            {
                const auto retainedConfig =
                    histogram.diagnostics.config;
                const auto retainedEyeConfig =
                    histogram.diagnostics.eyeConfig;
                const auto retainedEyeState =
                    histogram.diagnostics.eyeState;
                const auto retainedHighlightConfig =
                    histogram.diagnostics.highlightConfig;
                const auto retainedToneMapping =
                    histogram.diagnostics.toneMapping;
                const auto retainedEyeUpdate =
                    histogram.lastEyeUpdate;
                const bool retainedHasEyeUpdateTime =
                    histogram.hasEyeUpdateTime;
                const bool retainedOverlay =
                    histogram.showMeteringOverlay;

                histogram = {};
                histogram.diagnostics.config =
                    retainedConfig;
                histogram.diagnostics.eyeConfig =
                    retainedEyeConfig;
                histogram.diagnostics.eyeState =
                    retainedEyeState;
                histogram.diagnostics.highlightConfig =
                    retainedHighlightConfig;
                histogram.diagnostics.toneMapping =
                    retainedToneMapping;
                histogram.lastEyeUpdate =
                    retainedEyeUpdate;
                histogram.hasEyeUpdateTime =
                    retainedHasEyeUpdateTime;
                histogram.showMeteringOverlay =
                    retainedOverlay;
                histogram.width = width;
                histogram.height = height;

                histogram.meteringMask =
                    device_->CreateTexture({
                        .width = width,
                        .height = height,
                        .format =
                            rhi::TextureFormat::
                                RGBA16_Float,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource,
                        .allowUnorderedAccess =
                            true
                    });

                histogram.histogramReadback.reserve(
                    framesInFlight_);
                histogram.statisticsReadback.reserve(
                    framesInFlight_);
                histogram.submitted.assign(
                    framesInFlight_,
                    false);

                for (u32 slot = 0U;
                     slot < framesInFlight_;
                     ++slot)
                {
                    histogram.histogramReadback.push_back(
                        device_->CreateBuffer({
                            .sizeBytes =
                                static_cast<u64>(
                                    post_process::
                                        kLuminanceHistogramBins) *
                                sizeof(u32),
                            .usage =
                                rhi::BufferUsage::
                                    Structured,
                            .memory =
                                rhi::MemoryUsage::
                                    HostReadback,
                            .initialState =
                                rhi::ResourceState::
                                    CopyDestination
                        }));

                    histogram.statisticsReadback.push_back(
                        device_->CreateBuffer({
                            .sizeBytes =
                                sizeof(
                                    post_process::
                                        GpuLuminanceHistogramStatistics),
                            .usage =
                                rhi::BufferUsage::
                                    Structured,
                            .memory =
                                rhi::MemoryUsage::
                                    HostReadback,
                            .initialState =
                                rhi::ResourceState::
                                    CopyDestination
                        }));
                }
            }

            const u32 histogramFrameSlot =
                frameIndex %
                framesInFlight_;

            if (histogram.submitted[
                    histogramFrameSlot])
            {
                auto* statisticsBytes =
                    histogram.statisticsReadback[
                        histogramFrameSlot]->Map();

                post_process::
                    GpuLuminanceHistogramStatistics
                        gpuStatistics{};

                std::memcpy(
                    &gpuStatistics,
                    statisticsBytes,
                    sizeof(gpuStatistics));

                histogram.statisticsReadback[
                    histogramFrameSlot]->Unmap();

                auto* histogramBytes =
                    histogram.histogramReadback[
                        histogramFrameSlot]->Map();

                std::memcpy(
                    histogram.diagnostics.bins.data(),
                    histogramBytes,
                    static_cast<std::size_t>(
                        post_process::
                            kLuminanceHistogramBins) *
                        sizeof(u32));

                histogram.histogramReadback[
                    histogramFrameSlot]->Unmap();

                histogram.diagnostics.statistics =
                    post_process::
                        DecodeLuminanceHistogramStatistics(
                            gpuStatistics);

                const auto eyeNow =
                    std::chrono::steady_clock::now();

                f32 eyeDeltaSeconds =
                    1.0F / 60.0F;

                if (histogram.hasEyeUpdateTime)
                {
                    eyeDeltaSeconds =
                        std::clamp(
                            std::chrono::duration<f32>(
                                eyeNow -
                                histogram.lastEyeUpdate).
                                count(),
                            1.0F / 240.0F,
                            0.25F);
                }

                histogram.diagnostics.eyeState =
                    post_process::
                        UpdateHumanEyeAdaptation(
                            histogram.diagnostics.eyeState,
                            histogram.diagnostics.statistics,
                            eyeDeltaSeconds,
                            histogram.diagnostics.eyeConfig);

                histogram.lastEyeUpdate =
                    eyeNow;
                histogram.hasEyeUpdateTime =
                    true;

                histogram.diagnostics.
                    meteringMaskAvailable =
                        true;
            }

            const auto histogramHandle =
                graph.CreateBuffer(
                    prefix +
                        ".LuminanceHistogram",
                    {
                        .sizeBytes =
                            static_cast<u64>(
                                post_process::
                                    kLuminanceHistogramBins) *
                            sizeof(u32),
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                GpuOnly,
                        .initialState =
                            rhi::ResourceState::
                                UnorderedAccess
                    });

            const auto histogramStatisticsHandle =
                graph.CreateBuffer(
                    prefix +
                        ".LuminanceStatistics",
                    {
                        .sizeBytes =
                            sizeof(
                                post_process::
                                    GpuLuminanceHistogramStatistics),
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                GpuOnly,
                        .initialState =
                            rhi::ResourceState::
                                UnorderedAccess
                    });

            const auto histogramMaskHandle =
                graph.ImportTexture(
                    prefix +
                        ".LuminanceMeteringMask",
                    *histogram.meteringMask,
                    rhi::ResourceState::
                        ShaderResource);

            const auto histogramReadbackHandle =
                graph.ImportBuffer(
                    prefix +
                        ".LuminanceHistogramReadback",
                    *histogram.histogramReadback[
                        histogramFrameSlot],
                    rhi::ResourceState::
                        CopyDestination);

            const auto statisticsReadbackHandle =
                graph.ImportBuffer(
                    prefix +
                        ".LuminanceStatisticsReadback",
                    *histogram.statisticsReadback[
                        histogramFrameSlot],
                    rhi::ResourceState::
                        CopyDestination);

            const auto histogramConfig =
                histogram.diagnostics.config;

            graph.AddPass(
                prefix +
                    ".LuminanceHistogramReset",
                {},
                {
                    {
                        .buffer =
                            histogramHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .buffer =
                            histogramStatisticsHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 histogramHandle,
                 histogramStatisticsHandle](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    luminanceHistogramRenderer_.
                        Reset(
                            commands,
                            resources.Buffer(
                                histogramHandle),
                            resources.Buffer(
                                histogramStatisticsHandle));
                });

            graph.AddPass(
                prefix +
                    ".LuminanceHistogramBuild",
                {
                    {
                        .texture =
                            targets.color,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            histogramMaskHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                {
                    {
                        .buffer =
                            histogramHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .buffer =
                            histogramStatisticsHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 histogramMask =
                    histogram.meteringMask.get(),
                 histogramHandle,
                 histogramStatisticsHandle,
                 width,
                 height,
                 histogramConfig](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    luminanceHistogramRenderer_.
                        Build(
                            commands,
                            *color,
                            *histogramMask,
                            resources.Buffer(
                                histogramHandle),
                            resources.Buffer(
                                histogramStatisticsHandle),
                            width,
                            height,
                            histogramConfig);
                });

            graph.AddPass(
                prefix +
                    ".LuminanceHistogramReduce",
                {},
                {
                    {
                        .buffer =
                            histogramHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            histogramStatisticsHandle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 histogramHandle,
                 histogramStatisticsHandle,
                 histogramConfig](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    luminanceHistogramRenderer_.
                        Reduce(
                            commands,
                            resources.Buffer(
                                histogramHandle),
                            resources.Buffer(
                                histogramStatisticsHandle),
                            histogramConfig);
                });

            graph.AddPass(
                prefix +
                    ".LuminanceHistogramReadback",
                {},
                {
                    {
                        .buffer =
                            histogramHandle,
                        .state =
                            rhi::ResourceState::
                                CopySource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            histogramStatisticsHandle,
                        .state =
                            rhi::ResourceState::
                                CopySource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .buffer =
                            histogramReadbackHandle,
                        .state =
                            rhi::ResourceState::
                                CopyDestination,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .buffer =
                            statisticsReadbackHandle,
                        .state =
                            rhi::ResourceState::
                                CopyDestination,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [histogramHandle,
                 histogramStatisticsHandle,
                 histogramReadbackHandle,
                 statisticsReadbackHandle](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
                    commands.CopyBuffer(
                        resources.Buffer(
                            histogramHandle),
                        0U,
                        resources.Buffer(
                            histogramReadbackHandle),
                        0U,
                        static_cast<u64>(
                            post_process::
                                kLuminanceHistogramBins) *
                            sizeof(u32));

                    commands.CopyBuffer(
                        resources.Buffer(
                            histogramStatisticsHandle),
                        0U,
                        resources.Buffer(
                            statisticsReadbackHandle),
                        0U,
                        sizeof(
                            post_process::
                                GpuLuminanceHistogramStatistics));
                });

            graph.AddPass(
                prefix +
                    ".LuminanceMeteringMaskRestore",
                {
                    {
                        .texture =
                            histogramMaskHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    }
                },
                [](
                    rhi::CommandList&,
                    const render_graph::Resources&)
                {
                });

            histogram.submitted[
                histogramFrameSlot] =
                    true;
        }

        auto& histogram =
            luminanceHistogramPresentations_[
                info.id];

        if (colorLut_ == nullptr)
        {
            throw std::logic_error(
                "Studio viewport LUT correction has no GPU LUT.");
        }

        auto* displayLinear =
            &view->DisplayLinear();
        auto* displayGraded =
            &view->DisplayGraded();
        auto* displayColor =
            &view->DisplayColor();
        auto* colorLut =
            colorLut_.get();
        auto displayResolveSettings =
            displayResolveSettings_;

        // M25: eye adaptation controls presentation exposure only. The HDR
        // scene target remains physically untouched for GI, histogram,
        // bloom/glare extraction and future HDR output.
        if (histogram.diagnostics.eyeState.initialized)
        {
            displayResolveSettings.exposureScale *=
                histogram.diagnostics.eyeState.exposureScale;
        }

        auto colorLutSettings =
            colorLutSettings_;

        const auto surfaceDebugMode =
            view->SurfaceDebugMode();

        if (surfaceDebugMode !=
            lighting::SurfaceDebugMode::Lit)
        {
            // Diagnostic colors are data visualization, not presentation.
            // Do not let a user grade/LUT disguise the underlying buffers.
            colorLutSettings.enabled = false;
        }

        if (surfaceDebugMode ==
            lighting::SurfaceDebugMode::Lit)
        {
            graph.AddPass(
                prefix + ".DisplayResolve",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.displayLinear,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 displayLinear,
                 width,
                 height,
                 displayResolveSettings,
                 infoId = info.id](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    const auto found =
                        luminanceHistogramPresentations_.find(
                            infoId);

                    if (found ==
                        luminanceHistogramPresentations_.end())
                    {
                        displayResolveRenderer_.Draw(
                            commands,
                            *color,
                            *displayLinear,
                            width,
                            height,
                            displayResolveSettings);
                        return;
                    }

                    auto highlightConfig =
                        found->second.diagnostics.highlightConfig;

                    // M25 supplies scene-level evidence that strong highlight
                    // effects are warranted. Bloom remains a local soft-knee
                    // optical response; glare requires upper-percentile
                    // excess and flare requires an extreme peak.
                    if (found->second.diagnostics.
                            eyeState.p99ExcessStops <= 0.0F)
                    {
                        highlightConfig.glareEnabled = false;
                    }

                    if (found->second.diagnostics.
                            eyeState.peakExcessStops <= 0.0F)
                    {
                        highlightConfig.flareEnabled = false;
                    }

                    highlightEffectsRenderer_.Draw(
                        commands,
                        *color,
                        *displayLinear,
                        width,
                        height,
                        displayResolveSettings.exposureScale,
                        found->second.diagnostics.toneMapping,
                        highlightConfig);
                });
        }
        else
        {
            rhi::Texture* surfaceDebugSource = nullptr;
            render_graph::TextureHandle surfaceDebugHandle{};

            switch (surfaceDebugMode)
            {
            case lighting::SurfaceDebugMode::BaseColorRoughness:
                surfaceDebugSource =
                    &view->SurfaceBaseRoughness();
                surfaceDebugHandle =
                    targets.surfaceBaseRoughness;
                break;

            case lighting::SurfaceDebugMode::NormalMetallic:
                surfaceDebugSource =
                    &view->SurfaceNormalMetallic();
                surfaceDebugHandle =
                    targets.surfaceNormalMetallic;
                break;

            case lighting::SurfaceDebugMode::EmissionMetadata:
                surfaceDebugSource =
                    &view->SurfaceEmissionClass();
                surfaceDebugHandle =
                    targets.surfaceEmissionClass;
                break;

            case lighting::SurfaceDebugMode::Lit:
                break;
            }

            if (surfaceDebugSource == nullptr)
            {
                throw std::logic_error(
                    "Studio surface debug mode has no source attachment.");
            }

            graph.AddPass(
                prefix + ".SurfaceDebugResolve",
                {
                    {
                        .texture = surfaceDebugHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture = targets.displayLinear,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 surfaceDebugSource,
                 displayLinear,
                 width,
                 height,
                 surfaceDebugMode](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    surfaceDebugRenderer_.Draw(
                        commands,
                        *surfaceDebugSource,
                        *displayLinear,
                        width,
                        height,
                        surfaceDebugMode);
                });
        }

        graph.AddPass(
            prefix + ".ColorLutCorrection",
            {
                {
                    .texture = targets.displayLinear,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = targets.displayGraded,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             displayLinear,
             displayGraded,
             width,
             height,
             colorLut,
             colorLutSettings](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                colorLutRenderer_.Draw(
                    commands,
                    *displayLinear,
                    *displayGraded,
                    width,
                    height,
                    *colorLut,
                    colorLutSettings);
            });

        const auto outputDiagnostics =
            post_process::
                ResolveOutputTransform(
                    outputTransformSettings_,
                    outputDisplayCapabilities_);

        graph.AddPass(
            prefix + ".OutputTransform",
            {
                {
                    .texture = targets.displayGraded,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = targets.display,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             displayGraded,
             displayColor,
             width,
             height,
             outputDiagnostics,
             outputPattern =
                 outputTransformSettings_.
                     testPattern](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                outputTransformRenderer_.Draw(
                    commands,
                    *displayGraded,
                    *displayColor,
                    width,
                    height,
                    outputDiagnostics,
                    outputPattern);
            });

        if (histogram.showMeteringOverlay &&
            histogram.meteringMask != nullptr)
        {
            const auto histogramMaskHandle =
                graph.ImportTexture(
                    prefix +
                        ".LuminanceMeteringOverlayMask",
                    *histogram.meteringMask,
                    rhi::ResourceState::
                        ShaderResource);

            graph.AddPass(
                prefix +
                    ".LuminanceMeteringOverlay",
                {
                    {
                        .texture =
                            histogramMaskHandle,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    },
                    {
                        .texture =
                            targets.display,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 mask =
                    histogram.meteringMask.get(),
                 displayColor,
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    luminanceHistogramRenderer_.
                        DrawMeteringOverlay(
                            commands,
                            *mask,
                            *displayColor,
                            width,
                            height);
                });

            graph.AddPass(
                prefix +
                    ".LuminanceMeteringOverlayRestore",
                {
                    {
                        .texture =
                            targets.display,
                        .state =
                            rhi::ResourceState::
                                ShaderResource,
                        .access =
                            render_graph::Access::
                                Read
                    }
                },
                [](
                    rhi::CommandList&,
                    const render_graph::Resources&)
                {
                });
        }

        rendered.push_back({
            .id = info.id,
            .targets = targets,
            .targeted = shape.has_value()
        });
    }

    for (auto iterator =
             terrainPresentations_.begin();
         iterator !=
             terrainPresentations_.end();)
    {
        const bool stillExists =
            std::find_if(
                catalog.begin(),
                catalog.end(),
                [&iterator](
                    const StudioRenderViewInfo& item)
                {
                    return item.id ==
                        iterator->first;
                }) !=
            catalog.end();

        if (!stillExists)
        {
            iterator =
                terrainPresentations_.
                    erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    for (std::size_t index = 0U;
         index < celestialFrameGrants.size();
         ++index)
    {
        if (celestialGrantConsumed[index])
        {
            continue;
        }

        const auto& grant =
            celestialFrameGrants[index];

        celestialScheduler_.Abandon(
            grant.key,
            grant.authorityRevision);
    }

    return rendered;
}
} // namespace orbit::studio_ui
