#include <orbit/profiler/Profiler.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/celestial_atmosphere/SkyIrradiance.hpp>
#include <orbit/celestial_radiometry/Radiometry.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>
#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>
#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>
#include <orbit/studio_ui/VolumeParticleRenderBridge.hpp>
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
#include <orbit/world_model/StaticMeshBinding.hpp>
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
#include <filesystem>
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
// Resolves a view's cloud lab options into the renderer's lab: places the cloud
// ahead of the camera along the ground when the place serial changes, and builds
// the lab sun (elevation/azimuth at the cloud) when the sun is overridden.
template <typename Camera, typename Anchor>
[[nodiscard]] celestial_clouds::CloudLab ResolveCloudLab(
    Anchor& anchor,
    const StudioCloudLab& options,
    const Camera& camera,
    const f64 planetRadiusMeters,
    math::Float3& sunDirection,
    bool& sunOverridden)
{
    sunOverridden = false;
    if (!options.enabled)
    {
        anchor.valid = false;
        return {};
    }

    const math::Double3 position = camera.localPositionMeters;
    if (!anchor.valid || anchor.serial != options.placeSerial)
    {
        const math::Double3 up = math::Normalize(position);
        const math::Double3 forward{
            static_cast<f64>(camera.forward.x),
            static_cast<f64>(camera.forward.y),
            static_cast<f64>(camera.forward.z)};
        math::Double3 horizontal = forward - up * math::Dot(forward, up);
        if (math::Length(horizontal) < 1.0e-6)
        {
            horizontal = math::Cross(math::Double3{0.0, 1.0, 0.0}, up);
        }
        horizontal = math::Normalize(horizontal);
        anchor.center = math::Normalize(
            up * planetRadiusMeters +
            horizontal * static_cast<f64>(options.distanceMeters));
        anchor.serial = options.placeSerial;
        anchor.valid = true;
    }

    if (options.overrideSun)
    {
        const math::Double3 up = anchor.center;
        math::Double3 east = math::Cross(math::Double3{0.0, 1.0, 0.0}, up);
        if (math::Length(east) < 1.0e-6)
        {
            east = math::Double3{1.0, 0.0, 0.0};
        }
        east = math::Normalize(east);
        const math::Double3 north = math::Cross(up, east);
        const f64 elevation =
            static_cast<f64>(options.sunElevationDegrees) * std::numbers::pi / 180.0;
        const f64 azimuth =
            static_cast<f64>(options.sunAzimuthDegrees) * std::numbers::pi / 180.0;
        const math::Double3 sun =
            up * std::sin(elevation) +
            (north * std::cos(azimuth) + east * std::sin(azimuth)) * std::cos(elevation);
        sunDirection = {
            static_cast<f32>(sun.x), static_cast<f32>(sun.y), static_cast<f32>(sun.z)};
        sunOverridden = true;
    }

    return celestial_clouds::CloudLab{
        .enabled = true,
        .centerDirection = anchor.center,
        .radiusMeters = options.radiusMeters,
        .type = options.type,
        .coverage = options.coverage,
        .cirrus = options.cirrus,
        .precipitation = options.precipitation,
        .heightScale = options.heightScale,
        .maturity = options.maturity,
        .organisation = options.organisation,
        .density = options.density,
        .cirrusSheet = options.cirrusSheet,
        .seed = options.seed};
}

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

    // Cosine-weighted sky irradiance on an up-facing surface, from the sky
    // radiance's spherical-harmonic projection, normalized by Orbit's solar
    // reference irradiance. The sky table is in the sky frame (+Z = zenith).
    constexpr f64 kReferenceIrradiance =
        1361.0;

    const auto irradiance =
        celestial_atmosphere::EvaluateSkyIrradiance(
            celestial_atmosphere::
                ProjectSkyViewToSphericalHarmonics(
                    sky->skyView),
            {0.0, 0.0, 1.0});

    return {
        static_cast<f32>(irradiance.x / kReferenceIrradiance),
        static_cast<f32>(irradiance.y / kReferenceIrradiance),
        static_cast<f32>(irradiance.z / kReferenceIrradiance)
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
        a.overlapCells == b.overlapCells &&
        a.coarseGridResolution == b.coarseGridResolution &&
        a.coarseMinSpacingMeters == b.coarseMinSpacingMeters &&
        a.bandCount == b.bandCount &&
        a.bandEdgesMeters == b.bandEdgesMeters &&
        a.bandExtentMargin == b.bandExtentMargin &&
        a.bandZoneFraction == b.bandZoneFraction &&
        a.bandPartialUpdates == b.bandPartialUpdates;
}

// The experimental distance-banded clipmap for the layer options, or `base` (the
// ladder) when the experiment is off or its band edges are unusable.
[[nodiscard]] terrain_view::ClipmapConfig EffectiveClipmapConfig(
    const terrain_view::ClipmapConfig& base,
    const StudioTerrainLayerOptions& layers)
{
    if (!layers.fullClipmap || !layers.experimentalDistanceBands)
    {
        return base;
    }
    terrain_view::ClipmapConfig banded = base;
    banded.bandCount = 0U;
    banded.bandEdgesMeters = {};
    // Quantised so dragging the scale does not rebuild the renderer every frame.
    const f64 scale = std::exp2(
        std::round(std::log2(static_cast<f64>(layers.clipmapBandScale)) * 8.0) / 8.0);
    f64 previous = 0.0;
    for (const f32 rawEdge : layers.clipmapBandEdgesMeters)
    {
        const f64 edge = static_cast<f64>(rawEdge) * scale;
        if (rawEdge <= 0.0F || edge <= previous)
        {
            break;
        }
        banded.bandEdgesMeters[banded.bandCount++] = edge;
        previous = edge;
    }
    if (banded.bandCount < 2U)
    {
        return base;
    }
    banded.bandPartialUpdates = layers.clipmapPartialUpdates;
    banded.gridResolution = 513U;
    banded.coarseGridResolution = 0U;
    banded.levelCount = banded.bandCount;
    return banded;
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
    const world::SurfaceFrame& frame)
{
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
                localUp),
        .verticalFovRadians = camera.verticalFovRadians,
        .nearPlaneMeters = camera.nearPlaneMeters,
        .farPlaneMeters = camera.farPlaneMeters
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

// Scene-linear reference: stellar irradiance is expressed as a fraction of the
// solar constant, so a white Lambertian surface at 1 AU has radiance 1/pi.
constexpr f64 kStudioReferenceIrradianceWattsPerSquareMeter =
    1361.0;

[[nodiscard]] ResolvedStudioDirectLight
ResolveStudioDirectLight(
    studio_session::StudioSession& session,
    const universe::BodyId receiver,
    const time::SimulationTime atTime)
{
    ResolvedStudioDirectLight result;
    world_model::CelestialLightingService
        lighting(
            session.World().Objects(),
            session.World().Universe());
    result.direct = lighting.DominantDirectLightingAtBody(receiver, atTime);
    if (result.direct.has_value())
    {
        const auto direction = math::Normalize(
            result.direct->receiverBodyFixedToEmitterMeters);
        result.directionBody = {
            static_cast<f32>(direction.x),
            static_cast<f32>(direction.y),
            static_cast<f32>(direction.z)
        };
        result.irradianceScale = static_cast<f32>(
            result.direct->irradianceWattsPerSquareMeter /
            kStudioReferenceIrradianceWattsPerSquareMeter);
    }

    return result;
}

[[nodiscard]] std::optional<editor_ui::PreviewMaterial>
ResolveRuntimeBodyMaterialIfAssigned(
    content::ContentService* content,
    scene::ObjectStore& objects,
    std::optional<scene::ObjectId> bodyObject);

[[nodiscard]] std::vector<celestial_far_render::FarBodyDraw>
ResolveSystemBodyDraws(
    studio_session::StudioSession& session,
    content::ContentService* content,
    const universe::BodyId activeBody,
    const render_view::CameraState& camera,
    const time::SimulationTime atTime,
    const u32 width,
    const u32 height)
{
    struct DistanceDraw
    {
        f64 distanceMeters{0.0};
        celestial_far_render::FarBodyDraw draw;
    };
    std::vector<DistanceDraw> sorted;
    const auto& universe = session.World().Universe();
    const auto* active = universe.Bodies().FindBody(activeBody);
    if (active == nullptr || !camera.frame)
    {
        return {};
    }
    const auto& frames = universe.Frames();
    const auto& objects = session.World().Objects();
    const world_model::CelestialLightingService lighting(objects, universe);
    for (const auto bodyId : universe.Bodies().Bodies(active->system))
    {
        if (bodyId == activeBody)
        {
            continue;
        }
        const auto* body = universe.Bodies().FindBody(bodyId);
        if (body == nullptr)
        {
            continue;
        }
        const auto center = frames.TransformPoint(
            {.frame = body->frame, .localMeters = {}},
            camera.frame, atTime);
        const auto bodyFromCamera = frames.ResolveTransform(
            camera.frame, body->frame, atTime);
        if (!center.has_value() || !bodyFromCamera.has_value())
        {
            continue;
        }
        const auto projected = render_view::ProjectToViewport(
            camera, width, height, center->localMeters);
        if (!projected.has_value())
        {
            continue;
        }
        const f64 distance = math::Length(
            center->localMeters - camera.localPositionMeters);
        const auto bodyObject = universe.ObjectForBody(bodyId);
        const auto radiative = bodyObject.has_value()
            ? world_model::ResolveRadiativeBody(objects, *bodyObject)
            : std::nullopt;
        const f64 radius = radiative.has_value()
            ? radiative->photosphereRadiusMeters
            : ReferenceRadiusForShape(body->shape);
        if (!std::isfinite(distance) || !std::isfinite(radius) ||
            radius <= 0.0 || distance <= radius)
        {
            continue;
        }
        const f64 tanHalfFov = std::tan(
            static_cast<f64>(camera.verticalFovRadians) * 0.5);
        const f64 apparentRadius =
            radius / std::sqrt(distance * distance - radius * radius);
        const f64 radiusPixels = apparentRadius /
            std::max(tanHalfFov, 1.0e-6) * static_cast<f64>(height) * 0.5;
        const f64 radiusNdc = 2.0 * radiusPixels / static_cast<f64>(height);
        const f64 aspect = static_cast<f64>(width) / static_cast<f64>(height);
        // ProjectToViewport uses up x forward for screen-right, whereas the
        // Studio sky and far-body shaders use forward x up. Match the rendered
        // view so a distant body stays aligned while the camera yaws.
        const f64 centerX = 1.0 - 2.0 * projected->u;
        const f64 centerY = 1.0 - 2.0 * projected->v;
        if (std::abs(centerX) > 1.0 + radiusNdc / aspect + 0.1 ||
            std::abs(centerY) > 1.0 + radiusNdc + 0.1)
        {
            continue;
        }

        celestial_far_render::FarBodyDraw draw;
        draw.shape = body->shape;
        draw.camera = camera;
        draw.camera.frame = body->frame;
        draw.camera.localPositionMeters = math::TransformPoint(
            *bodyFromCamera, camera.localPositionMeters);
        const auto forward = math::TransformVector(
            bodyFromCamera->rotation,
            {camera.forward.x, camera.forward.y, camera.forward.z});
        const auto up = math::TransformVector(
            bodyFromCamera->rotation,
            {camera.up.x, camera.up.y, camera.up.z});
        draw.camera.forward = {
            static_cast<f32>(forward.x),
            static_cast<f32>(forward.y),
            static_cast<f32>(forward.z)};
        draw.camera.up = {
            static_cast<f32>(up.x),
            static_cast<f32>(up.y),
            static_cast<f32>(up.z)};
        draw.projectedRadiusPixels = radiusPixels;
        draw.screenCenterNdc = {
            static_cast<f32>(centerX), static_cast<f32>(centerY)};

        draw.stellar = radiative.has_value();
        draw.representation = radiusPixels >= 10.0 && !draw.stellar
            ? celestial_representation::Representation::SmoothGlobe
            : radiusPixels >= 0.55
                ? celestial_representation::Representation::AnalyticDiscImpostor
                : draw.stellar
                    ? celestial_representation::Representation::StellarPointProxy
                    : celestial_representation::Representation::PointProxy;
        if (draw.representation !=
            celestial_representation::Representation::SmoothGlobe)
        {
            // Disc normals face the body's actual camera direction even when
            // its centre is far from the middle of the viewport.
            const auto toCenter = math::Normalize(
                draw.camera.localPositionMeters * -1.0);
            draw.camera.forward = {
                static_cast<f32>(toCenter.x),
                static_cast<f32>(toCenter.y),
                static_cast<f32>(toCenter.z)};
        }

        if (radiative.has_value())
        {
            draw.shape = universe::SphereShape{
                radiative->photosphereRadiusMeters};
            draw.appearance.albedoLinear = {
                static_cast<f32>(radiative->stellarColorLinear.x),
                static_cast<f32>(radiative->stellarColorLinear.y),
                static_cast<f32>(radiative->stellarColorLinear.z)};
            draw.stellarColorLinear = draw.appearance.albedoLinear;
            draw.radiometricIntensity = static_cast<f32>(
                draw.representation ==
                    celestial_representation::Representation::StellarPointProxy
                    ? celestial_radiometry::EncodeIrradianceSceneLinear(
                          celestial_radiometry::IrradianceWattsPerSquareMeter(
                              radiative->radiative.luminosityWatts, distance))
                    : radiative->radiative.surfaceRadianceWattsPerSquareMeterSteradian /
                          kStudioReferenceIrradianceWattsPerSquareMeter);
            const auto& appearance = radiative->stellarAppearance;
            draw.stellarLimbDarkening = static_cast<f32>(appearance.limbDarkening);
            draw.stellarGranulationStrength = static_cast<f32>(appearance.granulationStrength);
            draw.stellarGranulationScale = static_cast<f32>(appearance.granulationScale);
            draw.stellarActivityLevel = static_cast<f32>(appearance.activityLevel);
            draw.stellarActivitySeed = static_cast<u32>(appearance.activitySeed);
            draw.stellarChromosphereStrength = static_cast<f32>(appearance.chromosphereStrength);
            draw.stellarChromosphereExtent = static_cast<f32>(appearance.chromosphereExtent);
            draw.stellarCoronaStrength = static_cast<f32>(appearance.coronaStrength);
            draw.stellarCoronaExtent = static_cast<f32>(appearance.coronaExtent);
            draw.stellarGlareStrength = static_cast<f32>(appearance.glareStrength);
            draw.stellarGlareRadiusPixels = static_cast<f32>(appearance.glareRadiusPixels);
        }
        else
        {
            draw.appearance.albedoLinear = {0.18F, 0.21F, 0.23F};
            if (const auto material = ResolveRuntimeBodyMaterialIfAssigned(
                    content, session.World().Objects(), bodyObject))
            {
                draw.appearance.albedoLinear = material->baseColor;
                draw.appearance.roughness = material->roughness;
                draw.appearance.emissionLinear = material->emissionRadiance;
            }
            if (const auto direct = lighting.DominantDirectLightingAtBody(bodyId, atTime))
            {
                const auto direction = math::Normalize(
                    direct->receiverBodyFixedToEmitterMeters);
                draw.lightDirectionBody = {
                    static_cast<f32>(direction.x),
                    static_cast<f32>(direction.y),
                    static_cast<f32>(direction.z)};
                draw.incidentLightScale = static_cast<f32>(
                    direct->irradianceWattsPerSquareMeter /
                    kStudioReferenceIrradianceWattsPerSquareMeter);
            }
            else
            {
                draw.incidentLightScale = 0.0F;
            }
            if (bodyObject.has_value())
            {
                if (const auto giant = world_model::ResolveGiantAppearance(
                        objects, *bodyObject))
                {
                    const auto& p = giant->parameters;
                    draw.giantEnabled = true;
                    draw.giantBaseColorLinear = {
                        static_cast<f32>(p.baseColorLinear.x),
                        static_cast<f32>(p.baseColorLinear.y),
                        static_cast<f32>(p.baseColorLinear.z)};
                    draw.giantBandColorLinear = {
                        static_cast<f32>(p.bandColorLinear.x),
                        static_cast<f32>(p.bandColorLinear.y),
                        static_cast<f32>(p.bandColorLinear.z)};
                    draw.giantPolarColorLinear = {
                        static_cast<f32>(p.polarColorLinear.x),
                        static_cast<f32>(p.polarColorLinear.y),
                        static_cast<f32>(p.polarColorLinear.z)};
                    draw.giantBandFrequency = static_cast<f32>(p.bandFrequency);
                    draw.giantBandStrength = static_cast<f32>(p.bandStrength);
                    draw.giantZonalShear = static_cast<f32>(p.zonalShear);
                    draw.giantStormStrength = static_cast<f32>(p.stormStrength);
                    draw.giantStormScale = static_cast<f32>(p.stormScale);
                    draw.giantPolarStrength = static_cast<f32>(p.polarStrength);
                    draw.giantDepthContrast = static_cast<f32>(p.depthContrast);
                    draw.giantTurbulenceStrength = static_cast<f32>(p.turbulenceStrength);
                    draw.giantSeed = static_cast<u32>(p.seed);
                }
                if (const auto small = world_model::ResolveSmallBodyAppearance(
                        objects, *bodyObject))
                {
                    const auto& p = small->parameters;
                    draw.smallBodyEnabled = true;
                    draw.appearance.albedoLinear = {
                        static_cast<f32>(p.regolithColorLinear.x),
                        static_cast<f32>(p.regolithColorLinear.y),
                        static_cast<f32>(p.regolithColorLinear.z)};
                    draw.smallBodyAxisScale = {
                        static_cast<f32>(p.axisScale.x),
                        static_cast<f32>(p.axisScale.y),
                        static_cast<f32>(p.axisScale.z)};
                    draw.smallBodyIrregularity = static_cast<f32>(p.irregularity);
                    draw.smallBodyLargeLobeStrength = static_cast<f32>(p.largeLobeStrength);
                    draw.smallBodyCraterDensity = static_cast<f32>(p.craterDensity);
                    draw.smallBodyCraterDepth = static_cast<f32>(p.craterDepth);
                    draw.smallBodyCraterRimStrength = static_cast<f32>(p.craterRimStrength);
                    draw.smallBodyFreshMaterialColorLinear = {
                        static_cast<f32>(p.freshMaterialColorLinear.x),
                        static_cast<f32>(p.freshMaterialColorLinear.y),
                        static_cast<f32>(p.freshMaterialColorLinear.z)};
                    draw.smallBodyColorVariation = static_cast<f32>(p.colorVariation);
                    draw.smallBodyOppositionStrength = static_cast<f32>(p.oppositionStrength);
                    draw.smallBodyOppositionWidthRadians = static_cast<f32>(p.oppositionWidthRadians);
                    draw.smallBodySingleScatteringAlbedo = static_cast<f32>(p.singleScatteringAlbedo);
                    draw.smallBodyMacroscopicRoughnessRadians = static_cast<f32>(p.macroscopicRoughnessRadians);
                    draw.smallBodySeed = static_cast<u32>(p.seed);
                }
            }
        }
        sorted.push_back({distance, draw});
    }
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const DistanceDraw& a, const DistanceDraw& b) {
            return a.distanceMeters > b.distanceMeters;
        });
    std::vector<celestial_far_render::FarBodyDraw> draws;
    draws.reserve(sorted.size());
    for (auto& item : sorted)
    {
        draws.push_back(std::move(item.draw));
    }
    return draws;
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

        const auto source =
            volume_render::
                BuildEmissiveVolumeSource(
                    *domain,
                    static_cast<f32>(
                        authoredEmission));

        if (source.has_value())
        {
            result.push_back(
                *source);
        }
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
      atmosphereRenderer_(device, compiler),
      cloudRenderer_(device, compiler),
      pathRenderer_(device, compiler),
      surfaceVolumeDebugRenderer_(device, compiler),
      universalVolumeRenderer_(device, compiler),
      volumeParticleRenderer_(device, compiler, framesInFlight),
      debugComposite_(device, compiler),
      directLightingRenderer_(device, compiler),
      materialEmissionSurfaceOverride_(device, compiler),
      finalGatherRenderer_(device, compiler),
      radianceCacheSampler_(device, compiler),
      proxySunShadowRenderer_(device, compiler),
      proxySurfaceRenderer_(device, compiler),
      meshLibrary_(std::make_unique<mesh_render::MeshLibrary>(device)),
      meshSurfaceRenderer_(device, compiler),
      meshSdfScene_(device, compiler),
      meshSdfDebugRenderer_(device, compiler),
      meshShadowMapRenderer_(device, compiler),
      meshSunShadowRenderer_(device, compiler),
      antiAliasingRenderer_(device, compiler),
      hybridReflectionRenderer_(device, compiler),
      exactReflectionQueryRenderer_(device, compiler),
      surfaceDebugRenderer_(device, compiler),
      flatMapRenderer_(device, compiler, framesInFlight),
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
    meshSurfaceRenderer_.SetSdfScene(&meshSdfScene_);

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
    const f64 projectedRadiusPixels,
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

    // A fixed 33-sample cube face spreads one vertex (and one appearance
    // sample: coastline, ice and cloud masks) over ~50 px once a planet
    // fills the view, producing staircase coastlines. Pick the tier whose
    // vertex spacing stays within a few pixels, with hysteresis so the
    // (parallel) rebuild does not thrash around a threshold.
    {
        constexpr std::array<u32, 5> kTiers{33U, 65U, 129U, 257U, 513U};
        constexpr f64 kUpgradeSpacingPixels = 4.0;
        constexpr f64 kDowngradeSpacingPixels = 2.0;

        const auto spacingPixels =
            [projectedRadiusPixels](const u32 resolution)
            {
                return std::max(projectedRadiusPixels, 0.0) *
                    (0.5 * std::numbers::pi_v<f64>) /
                    static_cast<f64>(resolution - 1U);
            };

        std::size_t tier = 0U;
        while (tier + 1U < kTiers.size() &&
               kTiers[tier] < presentation.faceResolution)
        {
            ++tier;
        }

        while (tier + 1U < kTiers.size() &&
               spacingPixels(kTiers[tier]) > kUpgradeSpacingPixels)
        {
            ++tier;
        }

        while (tier > 0U &&
               spacingPixels(kTiers[tier - 1U]) < kDowngradeSpacingPixels)
        {
            --tier;
        }

        presentation.faceResolution = kTiers[tier];
    }

    const celestial_globe::MacroGlobeConfig
        globeConfig{
            .faceResolution = presentation.faceResolution,
            .footprintScale = 1.5
        };

    const auto* terrainCapability =
        session.World().
            Surfaces().
            Registry().
            FindTerrainSurface(body);

    const u64 geometryFingerprint =
        terrainCapability != nullptr &&
            terrainCapability->terrain.get() == &terrainSource
        ? celestial_globe::
              MacroGlobeFingerprint(
                  terrainCapability->terrain,
                  shape,
                  globeConfig)
        : celestial_globe::
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

    celestial_appearance::
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

    appearanceConfig.standingWaterEnabled =
        resolvedOcean.has_value();

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

StudioClipmapPlanStats
StudioViewportRenderer::ClipmapPlanStats(
    const std::string_view viewportId) const noexcept
{
    const auto found = clipmapPlanStats_.find(viewportId);
    return found == clipmapPlanStats_.end() ? StudioClipmapPlanStats{}
                                            : found->second;
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

void StudioViewportRenderer::SetHumanEyeAdaptationLocked(
    const std::string_view viewportId,
    const bool locked)
{
    luminanceHistogramPresentations_[std::string(viewportId)]
        .diagnostics.eyeAdaptationLocked = locked;
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

// The sky-view table depends smoothly on the observer's altitude, but it is keyed
// by an exact hash of the radius and rebuilt on the CPU (about 22 ms), so a climb
// or descent rebuilt it on every frame. Snapping the altitude to ~2% steps keeps
// the sky within a fraction of a pixel of exact while rebuilding it only a few
// times per altitude decade.
[[nodiscard]] f64 QuantizedSkyObserverRadius(
    const f64 observerRadiusMeters,
    const f64 bottomRadiusMeters) noexcept
{
    const f64 altitude =
        std::max(observerRadiusMeters - bottomRadiusMeters, 1.0);
    constexpr f64 kLogStep = 0.02;
    const f64 snapped = std::exp(
        std::round(std::log(altitude) / kLogStep) * kLogStep);
    return bottomRadiusMeters + snapped;
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
    lighting::LightingTimestampRecorder* const lightingTimestamps,
    const StudioComposeCpuTimingRecorder& cpuTimingRecorder)
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
    auto catalog = views.Catalog();
    std::stable_sort(
        catalog.begin(),
        catalog.end(),
        [](const StudioRenderViewInfo& a,
           const StudioRenderViewInfo& b)
        {
            return a.id == "studio.primary" &&
                b.id != "studio.primary";
        });
    rendered.reserve(catalog.size());

    std::optional<world_model::ResolvedVolumeDomain> selectedVolume;
    volume_fields::VolumeFieldStorage* sharedVolumeStorage = nullptr;
    std::optional<volume_fields::ImportedVolumeFields> sharedVolumeFields;

    if (volumeFields_ != nullptr && snapshot.hasWorld &&
        session.World().Selection().Ordered().size() == 1U)
    {
        auto volumeObject = session.World().Selection().Ordered().front();
        const auto selectedRecord = session.World().Objects().Find(volumeObject);
        if (selectedRecord.has_value() && selectedRecord->parent.has_value() &&
            (selectedRecord->type == world_model::kVolumeSourceType ||
             selectedRecord->type == world_model::kVolumeEffectorType))
        {
            volumeObject = *selectedRecord->parent;
        }
        selectedVolume = world_model::ResolveVolumeDomain(
            session.World().Objects(), volumeObject);
    }

    for (const auto& info : catalog)
    {
        if (!views.CompositionEnabled(info.id))
        {
            continue;
        }

        auto composeStageStarted = std::chrono::steady_clock::now();
        const auto recordComposeStage =
            [&](const std::string_view stage)
            {
                if (!cpuTimingRecorder)
                {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                const double stageMs =
                    std::chrono::duration<double, std::milli>(
                        now - composeStageStarted).count();
                cpuTimingRecorder(stage, stageMs);
                if (orbit::profiler::Enabled())
                {
                    const auto end = orbit::profiler::NowTicks();
                    const auto width = static_cast<u64>(
                        stageMs * orbit::profiler::TicksPerMillisecond());
                    orbit::profiler::RecordLaneSpan(
                        "Compose stages",
                        orbit::profiler::Intern(stage),
                        end > width ? end - width : 0U,
                        end);
                }
                composeStageStarted = now;
            };
        auto* view = views.Find(info.id);

        if (view == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView catalog contains a missing view.");
        }

        // Anti-aliasing camera jitter. The camera is re-derived from
        // navigation every frame; if it is still exactly what we jittered last
        // frame, start again from the un-jittered camera so jitter never
        // accumulates, then apply this frame's sub-pixel rotation.
        const auto antiAliasingMode =
            static_cast<post_process::AntiAliasingMode>(
                std::min<u8>(info.layers.antiAliasing, 2U));
        {
            auto& aa = antiAliasingPresentations_[info.id];
            auto& camera = view->Camera();

            const auto sameCamera =
                [](const render_view::CameraState& a,
                   const render_view::CameraState& b)
            {
                return a.forward.x == b.forward.x &&
                       a.forward.y == b.forward.y &&
                       a.forward.z == b.forward.z &&
                       a.up.x == b.up.x && a.up.y == b.up.y &&
                       a.up.z == b.up.z &&
                       a.localPositionMeters.x == b.localPositionMeters.x &&
                       a.localPositionMeters.y == b.localPositionMeters.y &&
                       a.localPositionMeters.z == b.localPositionMeters.z &&
                       a.verticalFovRadians == b.verticalFovRadians;
            };

            if (aa.hasBase && sameCamera(camera, aa.applied))
            {
                camera = aa.base;
            }
            aa.base = camera;
            aa.hasBase = true;

            if (antiAliasingMode == post_process::AntiAliasingMode::Taa &&
                view->SurfaceDebugMode() == lighting::SurfaceDebugMode::Lit)
            {
                post_process::ApplyCameraJitter(
                    camera.forward,
                    camera.up,
                    camera.verticalFovRadians,
                    view->Height(),
                    {post_process::TaaJitterPixels(aa.frameCounter)[0] *
                         info.layers.taaJitterScale,
                     post_process::TaaJitterPixels(aa.frameCounter)[1] *
                         info.layers.taaJitterScale},
                    camera.forward,
                    camera.up);
            }
            ++aa.frameCounter;
            aa.applied = camera;
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
                        PreviewRevision();

                auto& proxyPresentation =
                    visibilityProxyPresentations_[
                        info.id];

                // The GPU scenes are float32 relative to the origin they were
                // built at; refresh it when the camera has drifted away while
                // it is near the proxies (see ProxyGpuOriginIsStale).
                const bool gpuOriginStale =
                    proxyPresentation.surfaces.Ready() &&
                    lighting::ProxyGpuOriginIsStale(
                        view->Lighting().cameraPositionInFrameMeters,
                        proxyPresentation.surfaces.
                            GpuOriginInFrameMeters(),
                        proxyPresentation.surfaces.
                            CentroidInFrameMeters(),
                        proxyPresentation.surfaces.
                            BoundingRadiusMeters());

                const bool requiresRebuild =
                    gpuOriginStale ||
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

                    // The visible proxy surfaces need only the primitive
                    // buffer, so they do not depend on ray-query support.
                    proxyPresentation.surfaces.Rebuild(
                        *device_,
                        proxyPresentation.scene,
                        view->Lighting().
                            gpuOriginInFrameMeters);

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

        // Imported Static Meshes of the target body: placed every frame in
        // double precision relative to the camera, drawn by the mesh surface
        // pass. Models load on a worker thread and appear once resident.
        {
            auto& meshPresentation = staticMeshPresentations_[info.id];
            meshPresentation.instances.clear();
            meshPresentation.requested = 0U;
            meshPresentation.hasAnchor = false;

            if (logicalTarget->target.has_value() &&
                snapshot.hasWorld &&
                bodies != nullptr &&
                frames != nullptr)
            {
                const auto meshBody = logicalTarget->target->body;
                const auto* meshBodyRecord = bodies->FindBody(meshBody);
                const auto meshBodyObject =
                    session.World().Universe().ObjectForBody(meshBody);

                if (meshBodyRecord != nullptr && meshBodyObject.has_value())
                {
                    const auto targetFromBody =
                        frames->ResolveTransform(
                            meshBodyRecord->frame,
                            view->Lighting().frame,
                            atTime);

                    if (targetFromBody.has_value())
                    {
                        const auto projectRoot =
                            session.World().Project().RootDirectory()
                                .lexically_normal();
                        const auto& cameraInFrame =
                            view->Lighting().cameraPositionInFrameMeters;

                        for (const auto& mesh :
                             world_model::ResolveStaticMeshes(
                                 session.World().Objects(),
                                 *meshBodyObject))
                        {
                            ++meshPresentation.requested;

                            // Asset paths are project-relative and may not
                            // leave the project folder.
                            const auto absolute =
                                (projectRoot /
                                 std::filesystem::path(mesh.meshAsset))
                                    .lexically_normal();
                            if (absolute.string().rfind(
                                    projectRoot.string(), 0U) != 0U)
                            {
                                continue;
                            }

                            const mesh_render::MeshModel* model =
                                meshLibrary_->Acquire(absolute);
                            if (model == nullptr)
                            {
                                continue;
                            }

                            // (Not math::Compose: this translation unit
                            // renames that identifier for the renderer.)
                            const auto placementRotation = math::Multiply(
                                targetFromBody->rotation,
                                EulerDegreesToRotation(mesh.eulerDegrees));
                            const auto placementOrigin = math::TransformPoint(
                                *targetFromBody, mesh.positionMeters);

                            if (!meshPresentation.hasAnchor)
                            {
                                meshPresentation.hasAnchor = true;
                                meshPresentation.anchorInFrame =
                                    placementOrigin;
                                meshPresentation.targetFromBody =
                                    *targetFromBody;
                            }

                            meshPresentation.instances.push_back({
                                .model = model,
                                .rows = mesh_render::MakeInstanceRows(
                                    placementRotation,
                                    mesh.uniformScale,
                                    placementOrigin - cameraInFrame)});
                        }
                    }
                }
            }
        }

        const auto terrainRuntime =
            session.TerrainRuntime().
                Capture(
                    info.id);
        if (cpuTimingRecorder)
        {
            const auto now = std::chrono::steady_clock::now();
            cpuTimingRecorder(
                "early",
                std::chrono::duration<double, std::milli>(
                    now - composeStageStarted).count());
            composeStageStarted = now;
        }

        if (terrainRuntime.has_value() &&
            !session.TerrainRuntime().
                IsCurrent(
                    *terrainRuntime))
        {
            throw std::logic_error(
                "Studio terrain viewport runtime is stale for the current session generation.");
        }

        // Non-mesh geometry the mesh distance field also needs, so the sunlit
        // ground and nearby structures bounce light onto meshes: Visibility
        // Proxies (analytic boxes / spheres) and a terrain height patch.
        if (auto meshFound = staticMeshPresentations_.find(info.id);
            meshFound != staticMeshPresentations_.end())
        {
            auto& meshPresentation = meshFound->second;
            if (!meshPresentation.hasAnchor ||
                meshPresentation.instances.empty())
            {
                meshPresentation.sdfExtra.reset();
            }
            else
            {
                auto extra =
                    std::make_shared<mesh_render::SdfExtraGeometry>();
                const auto anchor = meshPresentation.anchorInFrame;

                if (const auto proxyFound =
                        visibilityProxyPresentations_.find(info.id);
                    !info.layers.bypassSdfProxies &&
                    proxyFound != visibilityProxyPresentations_.end() &&
                    proxyFound->second.scene.Frame() ==
                        view->Lighting().frame)
                {
                    // Centres come back relative to the anchor so float32
                    // keeps millimetres at planetary distances.
                    for (const auto& gpu :
                         proxyFound->second.scene.GpuPrimitives(anchor))
                    {
                        const bool box = gpu.centerType.w > 0.5F;
                        mesh_render::SdfProxyPrimitive primitive;
                        primitive.center = {
                            anchor.x + static_cast<f64>(gpu.centerType.x),
                            anchor.y + static_cast<f64>(gpu.centerType.y),
                            anchor.z + static_cast<f64>(gpu.centerType.z)};
                        primitive.box = box;
                        primitive.axisX = {
                            gpu.axisXExtent.x, gpu.axisXExtent.y,
                            gpu.axisXExtent.z};
                        primitive.axisY = {
                            gpu.axisYExtent.x, gpu.axisYExtent.y,
                            gpu.axisYExtent.z};
                        primitive.axisZ = {
                            gpu.axisZExtent.x, gpu.axisZExtent.y,
                            gpu.axisZExtent.z};
                        primitive.halfExtents = {
                            gpu.axisXExtent.w,
                            box ? gpu.axisYExtent.w : gpu.axisXExtent.w,
                            box ? gpu.axisZExtent.w : gpu.axisXExtent.w};
                        extra->primitives.push_back(primitive);
                    }
                }

                if (terrainRuntime.has_value() &&
                    !info.layers.bypassSdfTerrain)
                {
                    const auto& patchSource =
                        session.TerrainRuntime().TerrainSource(
                            *terrainRuntime);
                    const u64 sourceRevision = patchSource.Revision();
                    const f64 moved = math::Length(
                        anchor - meshPresentation.terrainPatchAnchor);
                    if (meshPresentation.terrainPatch == nullptr ||
                        meshPresentation.terrainPatchSourceRevision !=
                            sourceRevision ||
                        moved > 2.0)
                    {
                        constexpr u32 kCount = 193U;
                        constexpr f64 kCell = 1.0;
                        const auto bodyFromFrame =
                            math::Inverse(meshPresentation.targetFromBody);
                        const math::Double3 anchorBody =
                            math::TransformPoint(bodyFromFrame, anchor);
                        const f64 anchorRadius = math::Length(anchorBody);
                        if (anchorRadius > 1.0)
                        {
                            const math::Double3 upBody =
                                anchorBody * (1.0 / anchorRadius);
                            const math::Double3 helper =
                                std::abs(upBody.y) < 0.99
                                ? math::Double3{0.0, 1.0, 0.0}
                                : math::Double3{1.0, 0.0, 0.0};
                            math::Double3 eastBody =
                                math::Cross(helper, upBody);
                            eastBody = eastBody *
                                (1.0 / math::Length(eastBody));
                            const math::Double3 northBody =
                                math::Cross(upBody, eastBody);

                            const auto& rotation =
                                meshPresentation.targetFromBody.rotation;
                            const auto toFrame =
                                [&](const math::Double3& v)
                            {
                                return math::TransformVector(rotation, v);
                            };
                            const math::Double3 upFrame = toFrame(upBody);
                            const math::Double3 eastFrame =
                                toFrame(eastBody);
                            const math::Double3 northFrame =
                                toFrame(northBody);

                            auto patch = std::make_shared<
                                mesh_render::SdfTerrainPatch>();
                            patch->center = anchor;
                            patch->east = {
                                static_cast<f32>(eastFrame.x),
                                static_cast<f32>(eastFrame.y),
                                static_cast<f32>(eastFrame.z)};
                            patch->north = {
                                static_cast<f32>(northFrame.x),
                                static_cast<f32>(northFrame.y),
                                static_cast<f32>(northFrame.z)};
                            patch->up = {
                                static_cast<f32>(upFrame.x),
                                static_cast<f32>(upFrame.y),
                                static_cast<f32>(upFrame.z)};
                            patch->cellMeters = static_cast<f32>(kCell);
                            patch->count = kCount;
                            patch->revision =
                                ++meshPresentation.terrainPatchCounter;
                            patch->heights.resize(
                                static_cast<std::size_t>(kCount) * kCount);
                            const f64 half =
                                0.5 * static_cast<f64>(kCount - 1U);
                            for (u32 j = 0U; j < kCount; ++j)
                            {
                                for (u32 i = 0U; i < kCount; ++i)
                                {
                                    const math::Double3 planar =
                                        anchorBody +
                                        eastBody *
                                            ((static_cast<f64>(i) - half) *
                                             kCell) +
                                        northBody *
                                            ((static_cast<f64>(j) - half) *
                                             kCell);
                                    const f64 radius = math::Length(planar);
                                    const math::Double3 direction =
                                        planar * (1.0 / radius);
                                    const auto sample = patchSource.Sample({
                                        .unitDirection = direction,
                                        .footprintMeters = kCell,
                                        .planet =
                                            terrainRuntime->planet.id,
                                        .radialOffsetMeters = 0.0});
                                    const f64 elevation =
                                        std::isfinite(sample.elevationMeters)
                                        ? sample.elevationMeters
                                        : 0.0;
                                    const math::Double3 ground =
                                        direction *
                                        (terrainRuntime->planet
                                             .radiusMeters +
                                         elevation);
                                    patch->heights
                                        [static_cast<std::size_t>(j) *
                                             kCount + i] =
                                        static_cast<f32>(math::Dot(
                                            ground - anchorBody, upBody));
                                }
                            }
                            meshPresentation.terrainPatch = patch;
                            meshPresentation.terrainPatchAnchor = anchor;
                            meshPresentation.terrainPatchSourceRevision =
                                sourceRevision;
                        }
                    }
                    extra->terrain = meshPresentation.terrainPatch;
                }
                meshPresentation.sdfExtra = std::move(extra);
            }
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

        auto studioDirectLight =
            logicalTarget->target.has_value() &&
                    snapshot.hasWorld
                ? ResolveStudioDirectLight(
                      session,
                      logicalTarget->target->body,
                      atTime)
                : ResolvedStudioDirectLight{};

        // Cloud lab sun override: light the whole view (terrain, shadows, sky and
        // clouds) from the chosen sun at the lab cloud, so a terminator-lit cloud sits
        // on a terminator-lit landscape.
        if (const auto& lab = info.layers.cloudLab;
            lab.enabled && lab.overrideSun && studioDirectLight.direct.has_value())
        {
            if (const auto anchor = cloudLabAnchors_.find(info.id);
                anchor != cloudLabAnchors_.end() && anchor->second.valid)
            {
                const math::Double3 up = anchor->second.center;
                math::Double3 east = math::Cross(math::Double3{0.0, 1.0, 0.0}, up);
                if (math::Length(east) < 1.0e-6)
                {
                    east = math::Double3{1.0, 0.0, 0.0};
                }
                east = math::Normalize(east);
                const math::Double3 north = math::Cross(up, east);
                const f64 elevation =
                    static_cast<f64>(lab.sunElevationDegrees) * std::numbers::pi / 180.0;
                const f64 azimuth =
                    static_cast<f64>(lab.sunAzimuthDegrees) * std::numbers::pi / 180.0;
                const math::Double3 sun =
                    up * std::sin(elevation) +
                    (north * std::cos(azimuth) + east * std::sin(azimuth)) * std::cos(elevation);
                studioDirectLight.directionBody = {
                    static_cast<f32>(sun.x), static_cast<f32>(sun.y), static_cast<f32>(sun.z)};
            }
        }

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

                // Seasons: the weather model's circulation cells follow the
                // latitude of the sub-stellar point.
                celestial_clouds::CloudFieldConfig cloudConfig{};
                if (studioDirectLight.direct.has_value())
                {
                    cloudConfig.subsolarLatitudeRadians = std::asin(
                        std::clamp(
                            static_cast<f64>(
                                studioDirectLight.directionBody.y),
                            -1.0,
                            1.0));
                }

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

        if (!info.layers.ocean)
        {
            resolvedOceanForView.reset();
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
                QuantizedSkyObserverRadius(
                    std::max(
                        rawObserverRadius,
                        resolvedAtmosphere->
                            parameters.
                            bottomRadiusMeters +
                            1.0e-3),
                    resolvedAtmosphere->
                        parameters.
                        bottomRadiusMeters);

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
                    // The sky table lives in a sky frame (+Z = this observer's
                    // zenith, sun at azimuth 0), so the sun goes in as that
                    // frame's direction, not as a body-fixed one.
                    .sunDirectionBody =
                        celestial_atmosphere::SkyFrameSunDirection(
                            view->Camera().localPositionMeters,
                            {
                                studioDirectLight.
                                    directionBody.x,
                                studioDirectLight.
                                    directionBody.y,
                                studioDirectLight.
                                    directionBody.z
                            }),
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
        const auto systemBodies =
            snapshot.hasWorld && logicalTarget->target.has_value() &&
                    logicalTarget->mode == studio_session::ViewportMode::Perspective
                ? ResolveSystemBodyDraws(
                      session, content_, logicalTarget->target->body,
                      view->Camera(), atTime, width, height)
                : std::vector<celestial_far_render::FarBodyDraw>{};
        auto backgroundBodies = std::make_shared<std::vector<
            celestial_far_render::FarBodyDraw>>();
        auto foregroundBodies = std::make_shared<std::vector<
            celestial_far_render::FarBodyDraw>>();
        const f64 activeDistance = math::Length(
            view->Camera().localPositionMeters);
        // Draw far bodies before the active presentation and closer bodies
        // after it. Stable far-to-near order also handles mutual transits.
        for (const auto& draw : systemBodies)
        {
            (math::Length(draw.camera.localPositionMeters) < activeDistance
                 ? *foregroundBodies
                 : *backgroundBodies).push_back(draw);
        }
        const auto drawBackgroundBodies =
            [this, color, width, height, backgroundBodies](
                rhi::CommandList& commands)
            {
                for (const auto& draw : *backgroundBodies)
                {
                    farBodyRenderer_.Draw(
                        commands, *color, width, height, draw);
                }
            };
        const auto drawForegroundBodies =
            [this, color, width, height, foregroundBodies](
                rhi::CommandList& commands)
            {
                for (const auto& draw : *foregroundBodies)
                {
                    farBodyRenderer_.Draw(
                        commands, *color, width, height, draw);
                }
            };

        // Only the production-terrain presentation clears depth itself.
        // Every other presentation must start from an empty depth buffer, or
        // depth-driven passes (GI fallback, reflections, atmosphere, particles)
        // see phantom surfaces left behind by the previously viewed body.
        if (presentation !=
            StudioViewportPresentation::ProductionTerrain)
        {
            auto* staleDepth = &view->Depth();

            graph.AddPass(
                prefix + ".ClearDepth",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::RenderTarget,
                        .access =
                            render_graph::Access::Write
                    },
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::DepthWrite,
                        .access =
                            render_graph::Access::Write
                    }
                },
                [color, staleDepth](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    // Clears are deferred to the next rendering scope that
                    // binds the attachment; bind depth here so the clear
                    // actually executes this frame.
                    commands.ClearDepthTarget(
                        *staleDepth,
                        0.0F);
                    commands.SetRenderTargets(
                        *color,
                        *staleDepth);
                });
        }


        // Near-field representation weight and sea level, recorded by the
        // production-terrain case for the water pass added after lighting.
        f64 nearFieldWaterWeight = 0.0;
        f32 nearFieldSeaLevelMeters = 0.0F;

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
                        // The full clipmap renderer never hands over to the
                        // orbital globe: with no macro representation the
                        // resolver keeps the production clipmap at every range.
                        .macroDisplacementAvailable =
                            hasMacroGlobe &&
                            !info.layers.fullClipmap,
                        .complexFarAppearance =
                            hasMacroGlobe &&
                            !info.layers.fullClipmap &&
                            !studioDirectLight.
                                direct.has_value(),
                        .radiativeEmitter = false
                    },
                    .policy =
                        celestialQualityPolicy_
                };
            // The view's LOD bias keeps richer representations longer (+) or
            // hands over to coarser ones sooner (-).
            representationInput.policy.qualityScale =
                std::clamp(
                    representationInput.policy.qualityScale *
                        std::exp2(
                            static_cast<f64>(
                                info.layers.lodBiasStops)),
                    0.05,
                    64.0);

            const celestial_representation::
                RepresentationSubjectId
                representationSubject{
                    .high =
                        terrainRuntime->body.high,
                    .low =
                        terrainRuntime->body.low
                };

            auto representationDecision =
                representationTracker_.ResolveFor(
                    representationSubject,
                    representationInput);

            auto representationBlend =
                celestial_representation::
                    ResolveRepresentationBlend(
                        representationInput,
                        representationDecision);

            // Full clipmap renderer: the production clipmap is the only terrain
            // representation, from the ground to orbit. The resolver would hand
            // over to a smooth globe or impostor as the body shrinks on screen;
            // that choice is overridden here so no other representation draws.
            if (info.layers.fullClipmap)
            {
                representationDecision.representation =
                    celestial_representation::Representation::ProductionSurface;
                representationDecision.lowerFidelityNeighbor =
                    celestial_representation::Representation::ProductionSurface;
                representationDecision.hysteresisHeld = false;
                representationBlend.richer =
                    celestial_representation::Representation::ProductionSurface;
                representationBlend.lower =
                    celestial_representation::Representation::ProductionSurface;
                representationBlend.richerWeight = 1.0;
                representationBlend.lowerWeight = 0.0;
                representationBlend.overlapping = false;
            }

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
                info.layers.productionSurface
                    ? weightFor(
                          celestial_representation::
                              Representation::
                                  ProductionSurface)
                    : 0.0;
            const f64 macroWeight =
                weightFor(
                    celestial_representation::
                        Representation::
                            MacroDisplacedGlobe);

            nearFieldWaterWeight = productionWeight;
            nearFieldSeaLevelMeters = static_cast<f32>(
                terrainDescription.global.seaLevelMeters);

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
            if (cpuTimingRecorder)
            {
                const auto now = std::chrono::steady_clock::now();
                cpuTimingRecorder(
                    "celestial",
                    std::chrono::duration<double, std::milli>(
                        now - composeStageStarted).count());
                composeStageStarted = now;
            }

            // The field generator depends only on the planet and its terrain
            // source, not on the clipmap. The adaptive coverage tier changes the
            // clipmap at a few altitudes; rebuilding the generator there
            // recompiled its compute shader on the frame thread (a ~3 s freeze
            // each time), so a clipmap-only change keeps it and rebuilds just
            // the renderer.
            const bool recreateGenerator =
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
                        terrainSourceRevision;

            const terrain_view::ClipmapConfig effectiveClipmap =
                EffectiveClipmapConfig(terrainRuntime->clipmap, info.layers);
            const bool recreate =
                recreateGenerator ||
                terrain.renderer == nullptr ||
                !SameClipmapConfig(
                    terrain.clipmap,
                    effectiveClipmap);

            if (recreate)
            {
                terrain_render::
                    TerrainPreviewConfig
                    config{};

                config.clipmap =
                    effectiveClipmap;
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
                config.drySurface =
                    !resolvedOceanForView.has_value();

                if (recreateGenerator)
                {
                    // The renderer references the generator; drop it first.
                    terrain.renderer.reset();
                    terrain.fieldGenerator =
                        std::make_unique<
                            terrain_gpu::
                                GpuFieldGenerator>(
                                    *device_,
                                    *compiler_,
                                    terrainRuntime->
                                        planet,
                                    *analytic);
                }

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
                    effectiveClipmap;
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

            if (info.layers.physicalPages)
            {
                terrain.renderer->
                    SetPhysicalPages(
                        physicalPages.pages,
                        physicalPages.generation);
            }
            else
            {
                // A distinct generation so toggling back re-applies the pages.
                terrain.renderer->SetPhysicalPages({}, 0U);
            }

            const auto particleBodyIdentity =
                VolumeParticleBodyIdentityWords(
                    terrainRuntime->body);
            std::vector<
                volume_render::VolumeParticleTerrainCollisionPage>
                particleCollisionPages;
            particleCollisionPages.reserve(
                physicalPages.pages.size());
            for (const auto& page : physicalPages.pages)
            {
                if (!page.IsValid())
                {
                    continue;
                }
                particleCollisionPages.push_back({
                    .samples = page.samples,
                    .resolution = page.resolution,
                    .face = static_cast<u32>(page.address.tile.face),
                    .level = page.address.tile.level,
                    .tileX = page.address.tile.x,
                    .tileY = page.address.tile.y,
                    .bodyIdentity = particleBodyIdentity
                });
            }
            volumeParticleRenderer_.UpdateTerrainCollisionPages(
                particleBodyIdentity,
                particleCollisionPages);

            const auto surfaceEffects =
                BuildVolumeSurfaceEffectRenderBatch(
                    terrainRuntime->body,
                    studio_session::VolumeSurfaceEffects().Stamps(),
                    terrain_render::SurfaceEffectGpuBinding::MaximumStampCount);

            terrain.renderer->
                SetSurfaceEffects(
                    info.layers.surfaceEffects
                        ? surfaceEffects.stamps
                        : decltype(surfaceEffects.stamps){});
            terrain.renderer->
                SetDrySurface(
                    !resolvedOceanForView.has_value());

            // Dynamic clipmap levels: which of the ladder's levels this camera
            // needs, planned from distance to the ground rather than a fixed ring.
            {
                terrain_view::ClipmapPlannerConfig planner{};
                planner.enabled = info.layers.dynamicClipmaps;
                // The LOD bias moves the planner's target spacing too: +1 stop
                // asks for twice the samples per pixel.
                planner.pixelsPerVertex =
                    static_cast<f64>(info.layers.clipmapPixelsPerVertex) *
                    std::exp2(-static_cast<f64>(info.layers.lodBiasStops));
                terrain.renderer->SetClipmapPlanner(planner);
                terrain.renderer->SetLevelFadeSeconds(
                    static_cast<f64>(info.layers.clipmapFadeSeconds));
                // "Tint terrain by clipmap level": the clipmap shader colours
                // each level differently, so the active set is visible.
                const auto overlays = views.TerrainDiagnosticOverlays(info.id);
                terrain.renderer->SetDebugVisuals(
                    overlays.clipmapLevels, false, overlays.clipmapSampleHealth, overlays.clipmapHoleView,
                    overlays.clipmapProjectionView, overlays.clipmapShadingView);
                terrain.renderer->SetWireframe(overlays.clipmapWireframe);
                terrain.renderer->SetClipmapFrozen(overlays.clipmapFreeze);

                const f64 groundElevation = analytic->Sample({
                    .unitDirection = math::Normalize(
                        terrainRuntime->observer.meters),
                    .footprintMeters = 500.0,
                    .planet = terrainRuntime->planet.id,
                    .radialOffsetMeters = 0.0
                }).elevationMeters;
                terrain.renderer->SetGroundElevationHint(groundElevation);

                // The ground the clipmap actually draws under the camera (read
                // back from the GPU a few frames late) is the floor navigation
                // uses next to the CPU terrain.
                const auto drawnGround = terrain.renderer->RenderedGround();
                // The CPU terrain at exactly the vertices the GPU value came from
                // (same positions, same footprint), to tell a real generator
                // mismatch from a difference in what is being compared.
                f64 cpuAtDrawnGround = 0.0;
                if (drawnGround.valid)
                {
                    cpuAtDrawnGround = -1.0e30;
                    for (const auto& cornerDirection : drawnGround.cornerDirections)
                    {
                        cpuAtDrawnGround = std::max(
                            cpuAtDrawnGround,
                            analytic->Sample({
                                .unitDirection = cornerDirection,
                                .footprintMeters = drawnGround.footprintMeters,
                                .planet = terrainRuntime->planet.id,
                                .radialOffsetMeters = 0.0
                            }).elevationMeters);
                    }
                }
                if (drawnGround.valid)
                {
                    views.SetRenderedGround(
                        info.id,
                        drawnGround.direction,
                        drawnGround.elevationMeters);
                }

                const auto& clipmapPlan = terrain.renderer->ClipmapPlan();
                const auto& streaming = terrain.renderer->StreamingStats();
                std::vector<StudioClipmapLevelStats> drawnLevels;
                for (const auto& level : terrain.renderer->ClipmapLevels())
                {
                    if (!level.active)
                    {
                        continue;
                    }
                    drawnLevels.push_back({
                        .level = level.level,
                        .spacingMeters = level.spacingMeters,
                        .halfExtentMeters = level.halfExtentMeters,
                        .bandInnerMeters = level.bandInnerMeters,
                        .bandOuterMeters = level.bandOuterMeters,
                        .gridResolution = level.gridResolution,
                        .drawnVertices = level.drawnVertices});
                }
                clipmapPlanStats_.insert_or_assign(
                    info.id,
                    StudioClipmapPlanStats{
                        .valid = true,
                        .dynamic = clipmapPlan.dynamic,
                        .banded = terrain.renderer->ClipmapBanded(),
                        .levels = std::move(drawnLevels),
                        .frozen = overlays.clipmapFreeze,
                        .wireframe = overlays.clipmapWireframe,
                        .ladderLevels = streaming.ladderLevels,
                        .firstLevel = clipmapPlan.firstLevel,
                        .lastLevel = clipmapPlan.lastLevel,
                        .nearestGroundMeters = streaming.nearestGroundMeters,
                        .visibleArcMeters = streaming.visibleArcMeters,
                        .requiredSpacingMeters = streaming.requiredSpacingMeters,
                        .finestSpacingMeters = clipmapPlan.finestSpacingMeters,
                        .coarsestHalfExtentMeters =
                            clipmapPlan.coarsestHalfExtentMeters,
                        .planChanges = streaming.planChanges,
                        .generatedSamples = streaming.cumulativeGeneratedSamples,
                        .groundElevationMeters = groundElevation,
                        .renderedGroundValid = drawnGround.valid,
                        .renderedGroundElevationMeters =
                            drawnGround.elevationMeters,
                        .renderedGroundCpuElevationMeters = cpuAtDrawnGround,
                        .renderedGroundFootprintMeters =
                            drawnGround.footprintMeters});
            }

            const auto camera =
                TerrainCameraFromBodyCamera(
                    view->Camera(),
                    terrain.renderer->CameraFrame());

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
                     drawBackgroundBodies,
                     framesInFlight =
                        framesInFlight_](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *color,
                            {
                                .red = 0.0F,
                                .green = 0.0F,
                                .blue = 0.0F,
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

                        drawBackgroundBodies(commands);

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

            // Keep a closed globe behind the local terrain at every altitude.
            recordComposeStage("terrain");
            if (hasMacroGlobe &&
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
                        representationDecision.
                            projectedRadiusPixels,
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
                     depth,
                     farPresentation,
                     surfaceBaseRoughness,
                     surfaceNormalMetallic,
                     surfaceEmissionClass,
                     shape = *shape,
                     globeCamera,
                     studioDirectLight,
                     resolvedOceanForView,
                     globeLodBias = info.layers.lodBiasStops,
                     macroGlobeLayer = (info.layers.macroGlobe && !info.layers.fullClipmap),
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
                            if (transitionGlobe == nullptr ||
                                !macroGlobeLayer)
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
                                        resolvedOceanForView.has_value(),
                                    .lodBiasStops = globeLodBias
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
                                    macroLighting,
                                    depth);
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
                            .texture = targets.depth,
                            .state = rhi::ResourceState::DepthWrite,
                            .access = render_graph::Access::Write
                        },
                        {
                            .texture = targets.surfaceEmissionClass,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [color,
                     depth,
                     surfaceBaseRoughness,
                     surfaceNormalMetallic,
                     surfaceEmissionClass,
                     clearForFarOnly,
                     drawBackgroundBodies,
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
                            commands.ClearDepthTarget(*depth, 0.0F);
                            const std::array<rhi::Texture*, 4> clearTargets{
                                color, surfaceBaseRoughness,
                                surfaceNormalMetallic, surfaceEmissionClass};
                            commands.ClearColorTarget(
                                *color,
                                {
                                    .red = 0.0F,
                                    .green = 0.0F,
                                    .blue = 0.0F,
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
                            commands.SetRenderTargets(clearTargets, depth);
                            drawBackgroundBodies(commands);
                        }

                        if (richer == celestial_representation::Representation::ProductionSurface)
                        {
                            // Depth composes the whole sphere with the local patch;
                            // a global alpha fade cannot represent spatial coverage.
                            drawRepresentation(commands,
                                celestial_representation::Representation::MacroDisplacedGlobe,
                                1.0F);
                            return;
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
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

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
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

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
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);
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

            const f64 globeProjectedRadiusPixels =
                [&]()
                {
                    const auto globeCamera =
                        view->Camera();
                    const f64 radius =
                        ReferenceRadiusForShape(*shape);
                    const f64 distance =
                        std::max(
                            math::Length(
                                globeCamera.localPositionMeters),
                            radius * 1.000001);
                    const f64 angularRadius =
                        std::asin(
                            std::clamp(
                                radius / distance,
                                0.0,
                                1.0));
                    return std::tan(angularRadius) /
                        std::max(
                            std::tan(
                                static_cast<f64>(
                                    globeCamera.verticalFovRadians) *
                                0.5),
                            1.0e-6) *
                        static_cast<f64>(height) *
                        0.5;
                }();

            auto* globe =
                EnsureMacroGlobePresentation(
                    info.id,
                    session,
                    logicalTarget->target->body,
                    *shape,
                    *macroGlobeSurface->terrain,
                    globeProjectedRadiusPixels,
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
                 resolvedOceanForView,
                 drawBackgroundBodies,
                 globeLodBias = info.layers.lodBiasStops,
                 macroGlobeLayer = (info.layers.macroGlobe && !info.layers.fullClipmap)](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
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

                    drawBackgroundBodies(commands);

                    if (globe == nullptr || !macroGlobeLayer)
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
                                    resolvedOceanForView.has_value(),
                                .lodBiasStops = globeLodBias
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
                     drawBackgroundBodies,
                     compactDraw](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *color,
                            {
                                .red = 0.0F,
                                .green = 0.0F,
                                .blue = 0.0F,
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

                        drawBackgroundBodies(commands);

                        compactObjectRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            compactDraw);
                    });

                transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

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

                        pointRadiometricIntensity =
                            static_cast<f32>(
                                celestial_radiometry::
                                    EncodeIrradianceSceneLinear(
                                        pointIrradiance));

                        // A resolved stellar disc is an extended source whose
                        // pixels carry its surface radiance (radiance is
                        // invariant with distance). Encode it in the same
                        // scene units as the shared direct-lighting pass,
                        // where scene value = radiance / reference irradiance
                        // (a white Lambertian surface at 1 AU reads 1/pi).
                        // The previous per-pixel solid-angle irradiance left
                        // a resolved sun orders of magnitude too dark.
                        resolvedRadiometricIntensity =
                            static_cast<f32>(
                                radiative.
                                    radiative.
                                    surfaceRadianceWattsPerSquareMeterSteradian /
                                kStudioReferenceIrradianceWattsPerSquareMeter);
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
                     drawBackgroundBodies,
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
                                .red = 0.0F,
                                .green = 0.0F,
                                .blue = 0.0F,
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

                        drawBackgroundBodies(commands);

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
                transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

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
                     drawBackgroundBodies,
                     camera,
                     bodyShape,
                     bodyMaterial](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        commands.ClearColorTarget(
                            *color,
                            {0.0F, 0.0F, 0.0F, 1.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceBaseRoughness,
                            {0.0F, 0.0F, 0.0F, 1.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceNormalMetallic,
                            {0.0F, 1.0F, 0.0F, 0.0F});
                        commands.ClearColorTarget(
                            *bodySurfaceEmissionClass,
                            {0.0F, 0.0F, 0.0F, 0.0F});

                        drawBackgroundBodies(commands);

                        bodyRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            bodyShape,
                            camera,
                            bodyMaterial,
                            false);
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

        case StudioViewportPresentation::FlatMap:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            // The map itself is drawn after the output transform (see the
            // ".FlatMap" pass below) so exposure and tone mapping leave it
            // alone; the scene underneath is just cleared.
            std::optional<StudioFlatMapRenderer::SourceBinding> flatMapSource;
            std::optional<math::Double3> flatMapMarker;
            if (terrainRuntime.has_value())
            {
                flatMapSource = StudioFlatMapRenderer::SourceBinding{
                    .terrain =
                        &session.TerrainRuntime().TerrainSource(
                            *terrainRuntime),
                    .planet = terrainRuntime->planet.id,
                    .radiusMeters = terrainRuntime->planet.radiusMeters,
                    .revision =
                        terrainRuntime->terrainSourceRevision ^
                        (terrainRuntime->surfaceSourceRevision *
                         0x9E3779B97F4A7C15ULL)
                };
                if (math::LengthSquared(terrainRuntime->observer.meters) >
                    0.0)
                {
                    flatMapMarker = terrainRuntime->observer.meters;
                }
            }
            flatMapRenderer_.Advance(
                info.id,
                flatMapSource.has_value() ? &*flatMapSource : nullptr,
                info.flatMapLayer,
                flatMapMarker,
                8.0);

            graph.AddPass(
                prefix + ".FlatMapClear",
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
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
                            .alpha = 1.0F
                        });
                });
            break;
        }

        case StudioViewportPresentation::Blank:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

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
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
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

        render_graph::BufferHandle
            sharedRadianceCellsHandle{};
        render_graph::BufferHandle
            sharedRadianceLevelsHandle{};
        u32 sharedRadianceLevelCount =
            0U;

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

            // Starlight reaches the ground through the atmosphere: tint the
            // direct term by the same transmittance LUT the sky and clouds use,
            // evaluated at the surface under the observer (sea level; terrain
            // relief changes it by a few percent). Without an atmosphere the
            // light stays untinted.
            math::Float3 stellarTransmittance{1.0F, 1.0F, 1.0F};
            if (terrainRuntime.has_value())
            {
                if (const auto atmosphereFound =
                        atmospherePresentations_.find(info.id);
                    atmosphereFound != atmospherePresentations_.end() &&
                    atmosphereFound->second.staticLuts != nullptr)
                {
                    const auto up =
                        math::Normalize(terrainRuntime->observer.meters);
                    const auto toStar =
                        math::Normalize(math::Double3{
                            static_cast<f64>(studioDirectLight.directionBody.x),
                            static_cast<f64>(studioDirectLight.directionBody.y),
                            static_cast<f64>(studioDirectLight.directionBody.z)});
                    const auto transmittance =
                        celestial_atmosphere::SunTransmittanceAt(
                            atmosphereFound->second.parameters,
                            *atmosphereFound->second.staticLuts,
                            atmosphereFound->second.parameters.bottomRadiusMeters,
                            math::Dot(up, toStar));
                    stellarTransmittance = {
                        static_cast<f32>(transmittance.x),
                        static_cast<f32>(transmittance.y),
                        static_cast<f32>(transmittance.z)};
                }
            }

            const lighting::DirectionalLight
                directLight{
                    .directionToLight =
                        studioDirectLight.directionBody,
                    .colorLinear = stellarTransmittance,
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
                finalGather.radianceResidency->
                    ConfigureGpuSnapshotFrameSlots(
                        framesInFlight_);
            }

            recordComposeStage("render");
            // The near-field indirect stack (screen-space final gather,
            // radiance-cache fallback, hybrid and exact reflections) only has
            // data within the camera-centred radiance clipmap. When the
            // nearest visible surface of the target body lies beyond that
            // coverage -- a planet seen from orbit -- it can only inject
            // error: a convex body cannot illuminate itself, and screen-space
            // gathers/reflections across an entire disc produce spurious
            // night-side light and budget seams. Direct lighting remains.
            bool nearFieldIndirect = true;

            if (shape.has_value())
            {
                const auto& clipmapConfig =
                    finalGather.radianceResidency->Config();
                const f64 clipmapHalfExtentMeters =
                    lighting::RadianceCellSizeMeters(
                        clipmapConfig,
                        clipmapConfig.levelCount > 0U
                            ? clipmapConfig.levelCount - 1U
                            : 0U) *
                    static_cast<f64>(
                        clipmapConfig.cellsPerAxis) *
                    0.5;
                const f64 cameraAltitudeMeters =
                    math::Length(
                        view->Camera().
                            localPositionMeters) -
                    ReferenceRadiusForShape(
                        *shape);

                nearFieldIndirect =
                    cameraAltitudeMeters <=
                    clipmapHalfExtentMeters;
            }
            // Bisecting switch: skip the indirect lighting (final gather and hybrid reflections).
            if (info.layers.bypassIndirectLighting)
            {
                nearFieldIndirect = false;
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
                    1.0F / 60.0F,
                    false));
            recordComposeStage("gi_prepare");

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
            // Do not invent a direction-independent fill light for a body
            // with no resolved atmospheric sky. The former scalar fallback
            // made night-side clipmap cells emit diffuse light even when
            // terrain correctly occluded the star.
            radianceEstimateSettings.
                ambientIrradianceScale = 0.0F;

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

            const auto authoredVolumeEmission =
                ResolveAuthoredEmissiveVolumes(
                    session.World().Objects());

            emissiveVolumes.insert(
                emissiveVolumes.end(),
                authoredVolumeEmission.begin(),
                authoredVolumeEmission.end());

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

            recordComposeStage("gi_prepare");
            const auto updateListStarted =
                std::chrono::steady_clock::now();
            lighting::RadianceResidencyStats radianceStats;
            const auto radianceUpdates =
                finalGather.radianceResidency->BuildUpdateList(
                    lightingView.cameraPositionInFrameMeters,
                    lightingPlan.radianceCacheUpdates,
                    &radianceStats);
            if (cpuTimingRecorder)
            {
                cpuTimingRecorder(
                    "gi_update_count",
                    static_cast<double>(radianceUpdates.size()));
                const auto now = std::chrono::steady_clock::now();
                cpuTimingRecorder(
                    "gi_update_list",
                    std::chrono::duration<double, std::milli>(
                        now - updateListStarted).count());
                composeStageStarted = now;
            }

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
                // The sky is estimated as its own channel (full strength,
                // occluded by terrain and proxies) rather than folded into the
                // one-bounce L1; direct lighting applies it as fill.
                const auto estimate =
                    lighting::EstimateRadianceCellWithSky(
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
                        estimate.indirect,
                        estimate.sky,
                        radianceEstimateSettings.diffuseTransportScale,
                        1U,
                        radianceSourceRevision));
            }
            recordComposeStage("gi_estimate");

            const auto snapshotBuildStarted =
                std::chrono::steady_clock::now();
            const auto& radianceSnapshot =
                finalGather.radianceResidency->BuildGpuSnapshotRef(
                    lightingView);
            const auto snapshotBuildFinished =
                std::chrono::steady_clock::now();

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

            if (finalGather.radianceCellsBuffers.size() !=
                framesInFlight_)
            {
                finalGather.radianceCellsBuffers.resize(framesInFlight_);
            }
            if (finalGather.radianceLevelsBuffers.size() !=
                framesInFlight_)
            {
                finalGather.radianceLevelsBuffers.resize(framesInFlight_);
            }
            const u32 radianceFrameSlot = frameIndex % framesInFlight_;
            auto& radianceCellsBuffer =
                finalGather.radianceCellsBuffers[radianceFrameSlot];
            auto& radianceLevelsBuffer =
                finalGather.radianceLevelsBuffers[radianceFrameSlot];
            if (radianceCellsBuffer == nullptr ||
                radianceCellsBuffer->SizeBytes() != radianceCellBytes)
            {
                radianceCellsBuffer = device_->CreateBuffer({
                    .sizeBytes = radianceCellBytes,
                    .usage = rhi::BufferUsage::Structured,
                    .memory = rhi::MemoryUsage::HostVisible,
                    .initialState = rhi::ResourceState::ShaderResource
                });
                finalGather.radianceResidency->
                    ConfigureGpuSnapshotFrameSlots(framesInFlight_);
            }
            if (radianceLevelsBuffer == nullptr ||
                radianceLevelsBuffer->SizeBytes() != radianceLevelBytes)
            {
                radianceLevelsBuffer = device_->CreateBuffer({
                    .sizeBytes = radianceLevelBytes,
                    .usage = rhi::BufferUsage::Structured,
                    .memory = rhi::MemoryUsage::HostVisible,
                    .initialState = rhi::ResourceState::ShaderResource
                });
            }

            const auto radianceCellsHandle = graph.ImportBuffer(
                prefix + ".RadianceCacheCells",
                *radianceCellsBuffer,
                rhi::ResourceState::ShaderResource);
            const auto radianceLevelsHandle = graph.ImportBuffer(
                prefix + ".RadianceCacheLevels",
                *radianceLevelsBuffer,
                rhi::ResourceState::ShaderResource);

            auto* radianceCellDestination =
                radianceCellsBuffer->Map();
            for (const auto cellIndex :
                 finalGather.radianceResidency->
                     GpuSnapshotDirtyCellIndices(radianceFrameSlot))
            {
                std::memcpy(
                    radianceCellDestination +
                        static_cast<std::size_t>(cellIndex) *
                            sizeof(lighting::GpuRadianceCell),
                    radianceSnapshot.cells.data() + cellIndex,
                    sizeof(lighting::GpuRadianceCell));
            }
            radianceCellsBuffer->Unmap();
            finalGather.radianceResidency->
                ClearGpuSnapshotDirtyCellIndices(radianceFrameSlot);

            auto* radianceLevelDestination =
                radianceLevelsBuffer->Map();
            std::memcpy(
                radianceLevelDestination,
                radianceSnapshot.levels.data(),
                static_cast<std::size_t>(
                    radianceSnapshot.levels.size()) *
                    sizeof(lighting::GpuRadianceLevelInfo));
            radianceLevelsBuffer->Unmap();
            const auto snapshotUploadFinished =
                std::chrono::steady_clock::now();
            if (cpuTimingRecorder)
            {
                cpuTimingRecorder(
                    "gi_snapshot_build",
                    std::chrono::duration<double, std::milli>(
                        snapshotBuildFinished - snapshotBuildStarted).count());
                cpuTimingRecorder(
                    "gi_snapshot_upload",
                    std::chrono::duration<double, std::milli>(
                        snapshotUploadFinished - snapshotBuildFinished).count());
            }
            recordComposeStage("gi_snapshot");

            const u32 radianceLevelCount =
                static_cast<u32>(
                    radianceSnapshot.levels.size());

            sharedRadianceCellsHandle =
                radianceCellsHandle;
            sharedRadianceLevelsHandle =
                radianceLevelsHandle;
            sharedRadianceLevelCount =
                radianceLevelCount;

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

            rhi::Buffer* particleLightGridForDirect =
                volumeParticleRenderer_.ParticleLightGridReady()
                    ? &volumeParticleRenderer_.ParticleLightGrid()
                    : nullptr;

            // Cloud shadows: sun transmittance through the cloud layer at each
            // visible surface, at half resolution, read by the direct lighting.
            rhi::Texture* cloudShadowTexture = nullptr;
            std::optional<render_graph::TextureUse> cloudShadowUse;
            if (const auto cloudFound = cloudPresentations_.find(info.id);
                info.layers.clouds &&
                !info.layers.bypassCloudShadow &&
                cloudFound != cloudPresentations_.end() &&
                cloudFound->second.gpu != nullptr &&
                cloudFound->second.field != nullptr &&
                !cloudFound->second.field->layers.empty() &&
                logicalTarget->target.has_value() &&
                cloudFound->second.body == logicalTarget->target->body &&
                (info.layers.fullClipmap || !info.layers.macroGlobe) &&
                studioDirectLight.direct.has_value() &&
                logicalTarget->mode != studio_session::ViewportMode::Debug)
            {
                if (const auto shadowAtmosphere =
                        atmospherePresentations_.find(info.id);
                    shadowAtmosphere != atmospherePresentations_.end() &&
                    shadowAtmosphere->second.body == logicalTarget->target->body)
                {
                    const u32 shadowWidth = std::max(1U, (width + 1U) / 2U);
                    const u32 shadowHeight = std::max(1U, (height + 1U) / 2U);
                    auto& shadowTarget = cloudShadowTargets_[info.id];
                    if (shadowTarget == nullptr ||
                        shadowTarget->Width() != shadowWidth ||
                        shadowTarget->Height() != shadowHeight)
                    {
                        shadowTarget = device_->CreateTexture({
                            .width = shadowWidth,
                            .height = shadowHeight,
                            .format = rhi::TextureFormat::RGBA16_Float,
                            .initialState = rhi::ResourceState::ShaderResource});
                    }
                    cloudShadowTexture = shadowTarget.get();
                    const auto shadowHandle = graph.ImportTexture(
                        prefix + ".CloudShadowTarget",
                        *cloudShadowTexture,
                        rhi::ResourceState::ShaderResource);

                    auto* shadowGpu = cloudFound->second.gpu.get();
                    const auto shadowLayer =
                        cloudFound->second.field->layers.front().parameters;
                    const f64 shadowReferenceRadius =
                        shadowAtmosphere->second.parameters.bottomRadiusMeters;
                    const auto shadowCamera = view->Camera();
                    math::Float3 labShadowSun{};
                    bool labShadowSunOverridden = false;
                    const auto shadowLab = ResolveCloudLab(
                        cloudLabAnchors_[info.id],
                        info.layers.cloudLab,
                        shadowCamera,
                        shadowReferenceRadius,
                        labShadowSun,
                        labShadowSunOverridden);
                    const celestial_atmosphere::AtmosphereRenderView shadowView{
                        .cameraPositionMeters = shadowCamera.localPositionMeters,
                        .forward = shadowCamera.forward,
                        .up = shadowCamera.up,
                        .verticalFovRadians = shadowCamera.verticalFovRadians,
                        .nearPlaneMeters = shadowCamera.nearPlaneMeters,
                        .farPlaneMeters = shadowCamera.farPlaneMeters,
                        .sunDirection = studioDirectLight.directionBody,
                        .irradianceScale = studioDirectLight.irradianceScale};

                    graph.AddPass(
                        prefix + ".CloudShadow",
                        {
                            {
                                .texture = targets.depth,
                                .state = rhi::ResourceState::DepthRead,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = shadowHandle,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         lightingDepth,
                         shadowGpu,
                         shadowLayer,
                         shadowReferenceRadius,
                         shadowView,
                         shadowLab,
                         shadowTexture = cloudShadowTexture,
                         shadowWidth,
                         shadowHeight](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            cloudRenderer_.DrawShadow(
                                commands,
                                *lightingDepth,
                                *shadowGpu,
                                *shadowTexture,
                                shadowWidth,
                                shadowHeight,
                                shadowReferenceRadius,
                                shadowLayer,
                                shadowView,
                                shadowLab);
                        });

                    cloudShadowUse = render_graph::TextureUse{
                        .texture = shadowHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read};
                }
            }

            // Authored Visibility Proxies as visible, lit geometry: drawn into
            // the surface buffer after the terrain so direct sun, the proxy sun
            // shadow, the final gather and the radiance cache all treat them as
            // ordinary rigid surfaces.
            if (!info.layers.bypassProxySurfaces &&
                logicalTarget->mode != studio_session::ViewportMode::Debug)
            {
                if (const auto surfaceFound =
                        visibilityProxyPresentations_.find(info.id);
                    surfaceFound != visibilityProxyPresentations_.end() &&
                    surfaceFound->second.surfaces.Ready())
                {
                    graph.AddPass(
                        prefix + ".ProxySurfaces",
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
                        [this,
                         geometry = &surfaceFound->second.surfaces,
                         color,
                         lightingBaseRoughness,
                         lightingNormalMetallic,
                         lightingEmissionClass,
                         lightingDepth,
                         width,
                         height,
                         lightingView](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            proxySurfaceRenderer_.Draw(
                                commands,
                                *geometry,
                                *color,
                                *lightingBaseRoughness,
                                *lightingNormalMetallic,
                                *lightingEmissionClass,
                                *lightingDepth,
                                width,
                                height,
                                lightingView);
                        });
                }
            }

            // Imported Static Meshes (glTF/GLB) as lit, textured rigid surfaces
            // in the same surface buffer, after the terrain and proxies.
            if (!info.layers.bypassMeshSurfaces &&
                logicalTarget->mode != studio_session::ViewportMode::Debug)
            {
                if (const auto meshFound =
                        staticMeshPresentations_.find(info.id);
                    meshFound != staticMeshPresentations_.end() &&
                    // Also while nothing is resident yet: the pass is what
                    // pumps the library (uploads) that makes models resident.
                    (!meshFound->second.instances.empty() ||
                     meshFound->second.requested > 0U))
                {
                    graph.AddPass(
                        prefix + ".MeshSurfaces",
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
                        [this,
                         instances = meshFound->second.instances,
                         sdfExtra = meshFound->second.sdfExtra,
                         color,
                         lightingBaseRoughness,
                         lightingNormalMetallic,
                         lightingEmissionClass,
                         lightingDepth,
                         width,
                         height,
                         lightingView](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            meshSurfaceRenderer_.Draw(
                                commands,
                                *meshLibrary_,
                                instances,
                                *color,
                                *lightingBaseRoughness,
                                *lightingNormalMetallic,
                                *lightingEmissionClass,
                                *lightingDepth,
                                width,
                                height,
                                lightingView,
                                sdfExtra.get());
                        });

                    // Light the mesh distance field's surface voxels (sun,
                    // sky and multi-bounce) for the world-space GI fallback.
                    {
                        mesh_render::SdfLightingInput sdfLight;
                        sdfLight.toSun = studioDirectLight.directionBody;
                        sdfLight.sunIrradiance =
                            studioDirectLight.direct.has_value()
                                ? studioDirectLight.irradianceScale
                                : 0.0F;
                        if (const auto skyFound =
                                atmospherePresentations_.find(info.id);
                            skyFound != atmospherePresentations_.end() &&
                            skyFound->second.skyView != nullptr)
                        {
                            sdfLight.skyIrradiance =
                                AtmosphereSkyIrradianceSummary(
                                    skyFound->second.skyView.get());
                        }
                        const auto& cameraFrame =
                            view->Lighting().cameraPositionInFrameMeters;
                        const f64 radial = math::Length(cameraFrame);
                        if (radial > 1.0)
                        {
                            sdfLight.up = {
                                static_cast<f32>(cameraFrame.x / radial),
                                static_cast<f32>(cameraFrame.y / radial),
                                static_cast<f32>(cameraFrame.z / radial)};
                        }
                        sdfLight.frame =
                            antiAliasingPresentations_[info.id].frameCounter;

                        graph.AddPass(
                            prefix + ".MeshSdfLighting",
                            {},
                            [this, sdfLight](
                                rhi::CommandList& commands,
                                const render_graph::Resources&)
                            {
                                meshSdfScene_.Light(commands, sdfLight);
                            });
                    }
                }
            }

            // Sun visibility against authored Visibility Proxies: one hardware
            // ray per visible pixel toward the star, read by direct lighting.
            // Terrain and sky are not proxies, so only authored structures cast.
            rhi::Texture* proxySunShadowTexture = nullptr;
            std::optional<render_graph::TextureUse> proxySunShadowUse;

            // Imported Static Meshes cast sun shadows through a sun-space
            // depth map (works without ray-query hardware); it is folded into
            // the same sun-visibility texture the proxy pass writes.
            std::optional<mesh_render::MeshShadowFrame> meshShadowFrame;
            std::vector<mesh_render::MeshInstance> meshShadowInstances;
            if (!info.layers.bypassProxySunShadow &&
                !info.layers.bypassMeshSurfaces &&
                studioDirectLight.direct.has_value() &&
                logicalTarget->mode != studio_session::ViewportMode::Debug)
            {
                if (const auto meshShadowFound =
                        staticMeshPresentations_.find(info.id);
                    meshShadowFound != staticMeshPresentations_.end() &&
                    !meshShadowFound->second.instances.empty())
                {
                    meshShadowInstances = meshShadowFound->second.instances;
                    meshShadowFrame = mesh_render::BuildMeshShadowFrame(
                        meshShadowInstances,
                        studioDirectLight.directionBody,
                        view->Lighting().cameraPositionInFrameMeters);

                    // The sun's real angular size (emitter radius over its
                    // distance) sets how fast shadows soften with distance.
                    if (meshShadowFrame.has_value() &&
                        studioDirectLight.direct.has_value() &&
                        bodies != nullptr &&
                        studioDirectLight.direct->sourceDistanceMeters > 0.0)
                    {
                        if (const auto* emitter = bodies->FindBody(
                                studioDirectLight.direct->emitter);
                            emitter != nullptr)
                        {
                            const f64 sine = std::clamp(
                                ReferenceRadiusForShape(emitter->shape) /
                                    studioDirectLight.direct->sourceDistanceMeters,
                                0.0, 0.5);
                            meshShadowFrame->sunTanHalfAngle =
                                static_cast<f32>(std::tan(std::asin(sine)));
                        }
                    }
                    if (meshShadowFrame.has_value())
                    {
                        // Scale for art direction / debugging (0 = hard shadows).
                        meshShadowFrame->sunTanHalfAngle *=
                            info.layers.meshShadowSoftness;
                    }
                }
            }

            if (!info.layers.bypassProxySunShadow &&
                studioDirectLight.direct.has_value() &&
                logicalTarget->mode != studio_session::ViewportMode::Debug)
            {
                const auto proxyFound =
                    visibilityProxyPresentations_.find(info.id);
                const bool proxiesReady =
                    proxySunShadowRenderer_.Supported() &&
                    proxyFound != visibilityProxyPresentations_.end() &&
                    proxyFound->second.hardware != nullptr &&
                    proxyFound->second.hardware->Ready();

                if (proxiesReady || meshShadowFrame.has_value())
                {
                    auto& shadowTarget = proxySunShadowTargets_[info.id];
                    if (shadowTarget == nullptr ||
                        shadowTarget->Width() != width ||
                        shadowTarget->Height() != height)
                    {
                        shadowTarget = device_->CreateTexture({
                            .width = width,
                            .height = height,
                            .format = rhi::TextureFormat::RGBA16_Float,
                            .initialState = rhi::ResourceState::ShaderResource,
                            .allowUnorderedAccess = true});
                    }
                    proxySunShadowTexture = shadowTarget.get();
                    const auto proxyShadowHandle = graph.ImportTexture(
                        prefix + ".ProxySunShadowTarget",
                        *proxySunShadowTexture,
                        rhi::ResourceState::ShaderResource);

                    // Sky fill for proxy surfaces uses the same atmosphere
                    // summary the radiance cache does; zero without a sky.
                    math::Float3 proxySkyIrradiance{};
                    if (const auto skyFound =
                            atmospherePresentations_.find(info.id);
                        skyFound != atmospherePresentations_.end() &&
                        skyFound->second.skyView != nullptr)
                    {
                        proxySkyIrradiance =
                            AtmosphereSkyIrradianceSummary(
                                skyFound->second.skyView.get());
                    }

                    if (proxiesReady)
                    graph.AddPass(
                        prefix + ".ProxySunShadow",
                        {
                            {
                                .texture = targets.surfaceNormalMetallic,
                                .state = rhi::ResourceState::ShaderResource,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = targets.surfaceEmissionClass,
                                .state = rhi::ResourceState::ShaderResource,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = targets.depth,
                                .state = rhi::ResourceState::DepthRead,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = proxyShadowHandle,
                                .state = rhi::ResourceState::UnorderedAccess,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         proxyHardware = proxiesReady
                             ? proxyFound->second.hardware.get()
                             : nullptr,
                         lightingNormalMetallic,
                         lightingEmissionClass,
                         lightingDepth,
                         proxySkyIrradiance,
                         shadowTexture = proxySunShadowTexture,
                         width,
                         height,
                         lightingView,
                         sunDirection = studioDirectLight.directionBody](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            proxySunShadowRenderer_.Draw(
                                commands,
                                *proxyHardware,
                                *shadowTexture,
                                *lightingNormalMetallic,
                                *lightingEmissionClass,
                                *lightingDepth,
                                width,
                                height,
                                lightingView,
                                sunDirection,
                                lighting::ProxySunShadowSettings{
                                    .skyIrradiance = proxySkyIrradiance});
                        });

                    if (meshShadowFrame.has_value())
                    {
                        auto& mapTargets = meshShadowTargets_[info.id];
                        const u32 mapSize = meshShadowFrame->mapSize;
                        if (mapTargets.color == nullptr ||
                            mapTargets.color->Width() != mapSize)
                        {
                            mapTargets.color = device_->CreateTexture({
                                .width = mapSize,
                                .height = mapSize,
                                .format = rhi::TextureFormat::R32_Float,
                                .initialState =
                                    rhi::ResourceState::ShaderResource});
                            mapTargets.depth = device_->CreateTexture({
                                .width = mapSize,
                                .height = mapSize,
                                .format = rhi::TextureFormat::D32_Float,
                                .initialState =
                                    rhi::ResourceState::DepthWrite});
                            mapTargets.skyColor = device_->CreateTexture({
                                .width = mesh_render::kMeshSkyAtlasWidth,
                                .height = mesh_render::kMeshSkyAtlasHeight,
                                .format = rhi::TextureFormat::R32_Float,
                                .initialState =
                                    rhi::ResourceState::ShaderResource});
                            mapTargets.skyDepth = device_->CreateTexture({
                                .width = mesh_render::kMeshSkyAtlasWidth,
                                .height = mesh_render::kMeshSkyAtlasHeight,
                                .format = rhi::TextureFormat::D32_Float,
                                .initialState =
                                    rhi::ResourceState::DepthWrite});
                        }

                        const auto skyColorHandle = graph.ImportTexture(
                            prefix + ".MeshSkyAtlas",
                            *mapTargets.skyColor,
                            rhi::ResourceState::ShaderResource);
                        const auto skyDepthHandle = graph.ImportTexture(
                            prefix + ".MeshSkyDepth",
                            *mapTargets.skyDepth,
                            rhi::ResourceState::DepthWrite);
                        const auto meshLocalUp = mesh_render::MeshLocalUp(
                            *meshShadowFrame,
                            view->Lighting().cameraPositionInFrameMeters);

                        graph.AddPass(
                            prefix + ".MeshSkyMap",
                            {
                                {
                                    .texture = skyColorHandle,
                                    .state = rhi::ResourceState::RenderTarget,
                                    .access = render_graph::Access::Write
                                },
                                {
                                    .texture = skyDepthHandle,
                                    .state = rhi::ResourceState::DepthWrite,
                                    .access = render_graph::Access::Write
                                }
                            },
                            [this,
                             instances = meshShadowInstances,
                             frame = *meshShadowFrame,
                             localUp = meshLocalUp,
                             colorAtlas = mapTargets.skyColor.get(),
                             depthAtlas = mapTargets.skyDepth.get()](
                                rhi::CommandList& commands,
                                const render_graph::Resources&)
                            {
                                meshShadowMapRenderer_.DrawSky(
                                    commands,
                                    *meshLibrary_,
                                    instances,
                                    frame,
                                    localUp,
                                    *colorAtlas,
                                    *depthAtlas);
                            });

                        const auto mapColorHandle = graph.ImportTexture(
                            prefix + ".MeshShadowMap",
                            *mapTargets.color,
                            rhi::ResourceState::ShaderResource);
                        const auto mapDepthHandle = graph.ImportTexture(
                            prefix + ".MeshShadowDepth",
                            *mapTargets.depth,
                            rhi::ResourceState::DepthWrite);

                        graph.AddPass(
                            prefix + ".MeshShadowMap",
                            {
                                {
                                    .texture = mapColorHandle,
                                    .state = rhi::ResourceState::RenderTarget,
                                    .access = render_graph::Access::Write
                                },
                                {
                                    .texture = mapDepthHandle,
                                    .state = rhi::ResourceState::DepthWrite,
                                    .access = render_graph::Access::Write
                                }
                            },
                            [this,
                             instances = meshShadowInstances,
                             frame = *meshShadowFrame,
                             colorMap = mapTargets.color.get(),
                             depthMap = mapTargets.depth.get()](
                                rhi::CommandList& commands,
                                const render_graph::Resources&)
                            {
                                meshShadowMapRenderer_.Draw(
                                    commands,
                                    *meshLibrary_,
                                    instances,
                                    frame,
                                    *colorMap,
                                    *depthMap);
                            });

                        graph.AddPass(
                            prefix + ".MeshSunShadow",
                            {
                                {
                                    .texture = targets.surfaceNormalMetallic,
                                    .state = rhi::ResourceState::ShaderResource,
                                    .access = render_graph::Access::Read
                                },
                                {
                                    .texture = targets.surfaceEmissionClass,
                                    .state = rhi::ResourceState::ShaderResource,
                                    .access = render_graph::Access::Read
                                },
                                {
                                    .texture = targets.depth,
                                    .state = rhi::ResourceState::DepthRead,
                                    .access = render_graph::Access::Read
                                },
                                {
                                    .texture = mapColorHandle,
                                    .state = rhi::ResourceState::ShaderResource,
                                    .access = render_graph::Access::Read
                                },
                                {
                                    .texture = skyColorHandle,
                                    .state = rhi::ResourceState::ShaderResource,
                                    .access = render_graph::Access::Read
                                },
                                {
                                    .texture = proxyShadowHandle,
                                    .state = rhi::ResourceState::UnorderedAccess,
                                    .access = render_graph::Access::Write
                                }
                            },
                            [this,
                             lightingNormalMetallic,
                             lightingEmissionClass,
                             lightingDepth,
                             frame = *meshShadowFrame,
                             colorMap = mapTargets.color.get(),
                             skyAtlas = mapTargets.skyColor.get(),
                             skyFill = mesh_render::MeshSkyFill{
                                 .irradiance = proxySkyIrradiance,
                                 .localUp = meshLocalUp},
                             shadowTexture = proxySunShadowTexture,
                             initialize = !proxiesReady,
                             width,
                             height,
                             lightingView](
                                rhi::CommandList& commands,
                                const render_graph::Resources&)
                            {
                                meshSunShadowRenderer_.Draw(
                                    commands,
                                    *shadowTexture,
                                    *lightingNormalMetallic,
                                    *lightingEmissionClass,
                                    *lightingDepth,
                                    *colorMap,
                                    *skyAtlas,
                                    width,
                                    height,
                                    lightingView,
                                    frame,
                                    skyFill,
                                    initialize);
                            });
                    }

                    proxySunShadowUse = render_graph::TextureUse{
                        .texture = proxyShadowHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read};
                }
            }

            const auto withCloudShadow =
                [&cloudShadowUse, &proxySunShadowUse](
                    std::vector<render_graph::TextureUse> uses)
            {
                if (cloudShadowUse.has_value())
                {
                    uses.push_back(*cloudShadowUse);
                }
                if (proxySunShadowUse.has_value())
                {
                    uses.push_back(*proxySunShadowUse);
                }
                return uses;
            };

            // The sky-only radiance cache fill is read by direct lighting, so
            // the cache buffers join its inputs when it is active.
            const bool skyCacheFill =
                nearFieldIndirect &&
                radianceLevelCount > 0U &&
                !info.layers.bypassSkyCache;
            constexpr f32 kSkyCacheStrength = 1.0F;

            std::vector<render_graph::BufferUse> directBufferUses{
                {
                    .buffer = localLightsHandle,
                    .state = rhi::ResourceState::ShaderResource,
                    .access = render_graph::Access::Read
                },
                {
                    .buffer = localOffsetsHandle,
                    .state = rhi::ResourceState::ShaderResource,
                    .access = render_graph::Access::Read
                },
                {
                    .buffer = localIndicesHandle,
                    .state = rhi::ResourceState::ShaderResource,
                    .access = render_graph::Access::Read
                }
            };
            if (skyCacheFill)
            {
                directBufferUses.push_back({
                    .buffer = radianceCellsHandle,
                    .state = rhi::ResourceState::ShaderResource,
                    .access = render_graph::Access::Read});
                directBufferUses.push_back({
                    .buffer = radianceLevelsHandle,
                    .state = rhi::ResourceState::ShaderResource,
                    .access = render_graph::Access::Read});
            }

            graph.AddPass(
                prefix + ".SharedDirectLighting",
                withCloudShadow({
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
                }),
                directBufferUses,
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
                 particleLightGridForDirect,
                 cloudShadowTexture,
                 proxySunShadowTexture,
                 skyCacheFill,
                 radianceCellsHandle,
                 radianceLevelsHandle,
                 radianceLevelCount,
                 lightingTimestamps,
                 frameIndex,
                 nearFieldIndirect](
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
                        localLightGrid,
                        particleLightGridForDirect,
                        cloudShadowTexture,
                        proxySunShadowTexture,
                        skyCacheFill
                            ? &resources.Buffer(radianceCellsHandle)
                            : nullptr,
                        skyCacheFill
                            ? &resources.Buffer(radianceLevelsHandle)
                            : nullptr,
                        skyCacheFill ? radianceLevelCount : 0U,
                        kSkyCacheStrength,
                        // The sky/ground fill floor tracks the stellar
                        // irradiance actually reaching this body, so distant
                        // planets are not washed flat by a fixed 3.5% floor.
                        lighting::DirectLightingSettings{
                            // Direct sun and resolved atmospheric sky are
                            // the only non-emissive terms. A uniform ground
                            // fill leaks daylight onto the clipmap's night
                            // side, so ambient comes from the atmosphere/GI
                            // sky summary rather than a constant floor.
                            .ambientIrradianceScale =
                                0.0F
                        });

                    if (lightingTimestamps != nullptr)
                    {
                        lightingTimestamps->EndSection(
                            commands,
                            frameIndex,
                            lighting::LightingGpuSection::Direct);
                    }
                });

            if (nearFieldIndirect)
            {
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

                gatherSettings.intensity = info.layers.giIntensity;
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
                     particleLightGridForDirect,
                     lightingTimestamps,
                     frameIndex,
                     useSdfGi = !info.layers.bypassSdfGi](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        // World-space fallback: the merged mesh distance field.
                        lighting::SdfGatherInput sdfGather;
                        if (const auto& volume = meshSdfScene_.Volume();
                            useSdfGi && volume.ready &&
                            volume.radiance != nullptr)
                        {
                            sdfGather.distance = volume.distance;
                            sdfGather.albedo = volume.albedo;
                            sdfGather.normal = volume.normal;
                            sdfGather.radiance = volume.radiance;
                            sdfGather.originInFrameMeters =
                                volume.originInFrameMeters;
                            sdfGather.voxelSize = volume.voxelSize;
                            sdfGather.dimensions = volume.dimensions;
                        }

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
                            particleLightGridForDirect,
                            gatherSettings,
                            sdfGather.distance != nullptr ? &sdfGather
                                                          : nullptr);
                    });

                if (radianceLevelCount > 0U &&
                    !info.layers.bypassRadianceCache)
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
                         lightingEmissionClass,
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
                                    *lightingEmissionClass,
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
                     height,
                     coverageView = info.layers.indirectCoverageView,
                     giOnlyView = info.layers.giOnlyView](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        finalGatherRenderer_.Combine(
                            commands,
                            *color,
                            *currentIndirect,
                            *gatherScratch,
                            width,
                            height,
                            1.0F,
                            coverageView,
                            giOnlyView);
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

                if (radianceLevelCount > 0U &&
                    !info.layers.bypassHybridReflections)
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
                         lightingEmissionClass,
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
                                    *lightingEmissionClass,
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
            }
            else
            {
                // Resume from a clean history when the view returns to the
                // near field instead of reprojecting stale indirect light.
                finalGather.hasHistory = false;
                finalGather.previousView = {};
            }

            // Near-field standing water is its own object: a flat surface at
            // sea level, alpha-blended over the lit scene, after lighting, GI
            // and reflections and before the atmosphere. The terrain pass drew
            // the true bed; this pass tests against its depth (the shoreline)
            // and measures the water column with it.
            if (presentation ==
                    StudioViewportPresentation::ProductionTerrain &&
                nearFieldWaterWeight > 0.0 &&
                !info.layers.bypassNearFieldWater &&
                resolvedOceanForView.has_value() &&
                terrainRuntime.has_value())
            {
                if (const auto waterTerrain =
                        terrainPresentations_.find(info.id);
                    waterTerrain != terrainPresentations_.end() &&
                    waterTerrain->second.renderer != nullptr)
                {
                    terrain_render::TerrainWaterOptics water{};
                    const auto& optical = resolvedOceanForView->optical;
                    water.absorptionPerMeter = {
                        static_cast<f32>(optical.absorptionPerMeter.x),
                        static_cast<f32>(optical.absorptionPerMeter.y),
                        static_cast<f32>(optical.absorptionPerMeter.z)};
                    water.refractiveIndex =
                        static_cast<f32>(optical.refractiveIndex);
                    water.deepColor = {
                        static_cast<f32>(optical.deepWaterColor.x),
                        static_cast<f32>(optical.deepWaterColor.y),
                        static_cast<f32>(optical.deepWaterColor.z)};
                    water.deepColorDepthMeters =
                        static_cast<f32>(optical.deepColorDepthMeters);
                    water.seaLevelMeters = nearFieldSeaLevelMeters;
                    water.sunDirectionBody = studioDirectLight.directionBody;
                    water.sunIrradiance = studioDirectLight.irradianceScale;
                    water.skyIrradiance =
                        radianceEstimateSettings.skyIrradianceLinear;
                    water.opacity = static_cast<f32>(nearFieldWaterWeight);

                    auto* waterRenderer = waterTerrain->second.renderer.get();
                    waterRenderer->SetWaterOptics(water);

                    const auto waterCamera =
                        TerrainCameraFromBodyCamera(
                            view->Camera(),
                            waterRenderer->CameraFrame());

                    graph.AddPass(
                        prefix + ".NearFieldWater",
                        {
                            {
                                .texture = targets.color,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            },
                            {
                                .texture = targets.depth,
                                .state = rhi::ResourceState::DepthRead,
                                .access = render_graph::Access::Read
                            }
                        },
                        [color,
                         lightingDepth,
                         waterRenderer,
                         waterCamera,
                         width,
                         height,
                         frameIndex](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            const std::array<rhi::Texture*, 1> waterTargets{
                                color};
                            commands.SetRenderTargetsReadOnlyDepth(
                                waterTargets,
                                *lightingDepth);
                            waterRenderer->DrawWater(
                                commands,
                                frameIndex,
                                width,
                                height,
                                waterCamera,
                                *lightingDepth);
                        });
                }
            }

            recordComposeStage("gi");
            // Physical atmosphere: attenuate the lit scene along each view
            // ray and add single + multiple in-scattering, for both the
            // limb seen from orbit and aerial perspective near the ground.
            if (const auto atmosphereFound =
                    atmospherePresentations_.find(
                        info.id);
                atmosphereFound !=
                        atmospherePresentations_.end() &&
                    atmosphereFound->second.gpu != nullptr &&
                    logicalTarget->target.has_value() &&
                    atmosphereFound->second.body ==
                        logicalTarget->target->body &&
                    studioDirectLight.direct.has_value() &&
                    !info.layers.bypassAtmosphere &&
                    logicalTarget->mode !=
                        studio_session::ViewportMode::Debug)
            {
                auto& scratch =
                    atmosphereScratch_[info.id];

                if (scratch == nullptr ||
                    scratch->Width() != width ||
                    scratch->Height() != height)
                {
                    scratch =
                        device_->CreateTexture({
                            .width = width,
                            .height = height,
                            .format =
                                rhi::TextureFormat::
                                    RGBA16_Float,
                            .initialState =
                                rhi::ResourceState::
                                    ShaderResource
                        });
                }

                const auto scratchHandle =
                    graph.ImportTexture(
                        prefix + ".AtmosphereScratch",
                        *scratch,
                        rhi::ResourceState::
                            ShaderResource);

                auto* atmosphereScratch =
                    scratch.get();
                auto* atmosphereLuts =
                    atmosphereFound->second.gpu.get();
                const auto atmosphereParameters =
                    atmosphereFound->second.parameters;
                const auto atmosphereCamera =
                    view->Camera();

                const celestial_atmosphere::
                    AtmosphereRenderView
                    atmosphereView{
                        .cameraPositionMeters =
                            atmosphereCamera.
                                localPositionMeters,
                        .forward =
                            atmosphereCamera.forward,
                        .up =
                            atmosphereCamera.up,
                        .verticalFovRadians =
                            atmosphereCamera.
                                verticalFovRadians,
                        .nearPlaneMeters =
                            atmosphereCamera.
                                nearPlaneMeters,
                        .farPlaneMeters =
                            atmosphereCamera.
                                farPlaneMeters,
                        .sunDirection =
                            studioDirectLight.
                                directionBody,
                        .irradianceScale =
                            studioDirectLight.
                                irradianceScale
                    };

                graph.AddPass(
                    prefix + ".Atmosphere",
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
                            .texture = targets.depth,
                            .state =
                                rhi::ResourceState::
                                    DepthRead,
                            .access =
                                render_graph::Access::
                                    Read
                        },
                        {
                            .texture = scratchHandle,
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
                     lightingDepth,
                     atmosphereLuts,
                     atmosphereScratch,
                     width,
                     height,
                     atmosphereParameters,
                     atmosphereView](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        atmosphereRenderer_.Draw(
                            commands,
                            *color,
                            *lightingDepth,
                            *atmosphereLuts,
                            *atmosphereScratch,
                            width,
                            height,
                            atmosphereParameters,
                            atmosphereView);
                    });

                graph.AddPass(
                    prefix + ".AtmosphereCopyBack",
                    {
                        {
                            .texture = scratchHandle,
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
                     atmosphereScratch,
                     color,
                     width,
                     height](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        debugComposite_.Draw(
                            commands,
                            *atmosphereScratch,
                            *color,
                            width,
                            height);
                    });

                // Not drawing clouds this frame leaves the history stale.
                if (!info.layers.clouds)
                {
                    if (const auto stale = cloudTemporal_.find(info.id);
                        stale != cloudTemporal_.end())
                    {
                        stale->second.valid = false;
                    }
                }

                // Cloud shell in the near-field/clipmap views, composited AFTER the atmosphere pass:
                // the clouds sit inside the atmosphere, so the atmosphere pass must not treat them as
                // surfaces at the terrain depth. The cloud shader applies its own aerial perspective
                // over the camera-to-cloud distance.
                if (const auto cloudFound = cloudPresentations_.find(info.id);
                    info.layers.clouds &&
                    (info.layers.fullClipmap || !info.layers.macroGlobe) &&
                    cloudFound != cloudPresentations_.end() &&
                    cloudFound->second.gpu != nullptr &&
                    cloudFound->second.field != nullptr &&
                    !cloudFound->second.field->layers.empty() &&
                    cloudFound->second.body == logicalTarget->target->body)
                {
                    auto* cloudGpu = cloudFound->second.gpu.get();
                    const auto cloudLayer =
                        cloudFound->second.field->layers.front().parameters;
                    const f64 cloudReferenceRadius =
                        atmosphereFound->second.parameters.bottomRadiusMeters;
                    // Cloud lab: one isolated cloud, optionally lit by a chosen sun.
                    auto cloudView = atmosphereView;
                    math::Float3 labSun = cloudView.sunDirection;
                    bool labSunOverridden = false;
                    const auto cloudLab = ResolveCloudLab(
                        cloudLabAnchors_[info.id],
                        info.layers.cloudLab,
                        atmosphereCamera,
                        cloudReferenceRadius,
                        labSun,
                        labSunOverridden);
                    if (labSunOverridden)
                    {
                        cloudView.sunDirection = labSun;
                    }

                    // Half-resolution march, then a depth-bounded bilinear composite.
                    const f32 cloudScale =
                        std::clamp(info.layers.cloudResolutionScale, 0.25F, 1.0F);
                    const f32 cloudGodrays =
                        std::clamp(info.layers.cloudGodrayStrength, 0.0F, 2.0F);
                    const bool cloudLightVolume = info.layers.cloudLightVolume;
                    auto& cloudVolumeSlot = cloudLightVolumes_[info.id];
                    if (cloudVolumeSlot == nullptr)
                    {
                        cloudVolumeSlot = cloudRenderer_.CreateLightVolume();
                    }
                    auto* cloudVolume = cloudVolumeSlot.get();
                    const f32 cloudVolumeDebug = info.layers.cloudVolumeDebugAltitude;
                    const u32 cloudWidth = std::max(
                        1U, static_cast<u32>(std::ceil(static_cast<f32>(width) * cloudScale)));
                    const u32 cloudHeight = std::max(
                        1U, static_cast<u32>(std::ceil(static_cast<f32>(height) * cloudScale)));
                    auto& cloudTarget = cloudTargets_[info.id];
                    if (cloudTarget == nullptr ||
                        cloudTarget->Width() != cloudWidth ||
                        cloudTarget->Height() != cloudHeight)
                    {
                        cloudTarget = device_->CreateTexture({
                            .width = cloudWidth,
                            .height = cloudHeight,
                            .format = rhi::TextureFormat::RGBA16_Float,
                            .initialState = rhi::ResourceState::ShaderResource});
                    }
                    auto* cloudTexture = cloudTarget.get();
                    const auto cloudHandle = graph.ImportTexture(
                        prefix + ".CloudTarget",
                        *cloudTexture,
                        rhi::ResourceState::ShaderResource);

                    // Temporal accumulation: the march is resolved against last frame's
                    // result into one of two persistent targets (see CloudRenderer::Resolve).
                    auto& temporal = cloudTemporal_[info.id];
                    const bool temporalOn = info.layers.cloudTemporal;
                    if (temporal.resolved[0] == nullptr ||
                        temporal.resolved[0]->Width() != cloudWidth ||
                        temporal.resolved[0]->Height() != cloudHeight)
                    {
                        for (auto& resolved : temporal.resolved)
                        {
                            resolved = device_->CreateTexture({
                                .width = cloudWidth,
                                .height = cloudHeight,
                                .format = rhi::TextureFormat::RGBA16_Float,
                                .initialState = rhi::ResourceState::ShaderResource});
                        }
                        temporal.valid = false;
                    }
                    const celestial_clouds::CloudRenderer::CloudResolveView nowView{
                        .cameraPositionMeters = cloudView.cameraPositionMeters,
                        .forward = cloudView.forward,
                        .up = cloudView.up,
                        .verticalFovRadians = cloudView.verticalFovRadians,
                        .aspect = static_cast<f32>(cloudWidth) / static_cast<f32>(cloudHeight)};
                    const auto sameLab =
                        [](const celestial_clouds::CloudLab& a, const celestial_clouds::CloudLab& b)
                    {
                        return a.enabled == b.enabled &&
                            a.centerDirection.x == b.centerDirection.x &&
                            a.centerDirection.y == b.centerDirection.y &&
                            a.centerDirection.z == b.centerDirection.z &&
                            a.radiusMeters == b.radiusMeters && a.type == b.type &&
                            a.coverage == b.coverage && a.cirrus == b.cirrus &&
                            a.precipitation == b.precipitation &&
                            a.heightScale == b.heightScale && a.maturity == b.maturity &&
                            a.organisation == b.organisation && a.density == b.density &&
                            a.cirrusSheet == b.cirrusSheet &&
                            a.seed == b.seed;
                    };
                    if (!sameLab(temporal.previousLab, cloudLab))
                    {
                        // The cloud changed under the history (lab placed or edited).
                        temporal.valid = false;
                    }
                    const u32 writeIndex = temporal.writeIndex;
                    auto* resolvedTexture = temporal.resolved[writeIndex].get();
                    auto* historyTexture = temporal.resolved[1U - writeIndex].get();
                    const bool historyValid = temporalOn && temporal.valid;
                    const auto previousView = temporal.previous;
                    const u32 cloudFrame = temporal.frame++;
                    const auto resolvedHandle = graph.ImportTexture(
                        prefix + ".CloudResolved",
                        *resolvedTexture,
                        rhi::ResourceState::ShaderResource);
                    const auto historyHandle = graph.ImportTexture(
                        prefix + ".CloudHistory",
                        *historyTexture,
                        rhi::ResourceState::ShaderResource);

                    // The sun optical-depth cache update is its own pass so the GPU
                    // pass timer reports it separately from the ray march. It declares
                    // the same cloud target as the march, which keeps it ordered
                    // before it; the recorded commands are unchanged.
                    graph.AddPass(
                        prefix + ".CloudLightVolume",
                        {
                            {
                                .texture = cloudHandle,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         cloudGpu,
                         cloudLayer,
                         cloudReferenceRadius,
                         atmosphereParameters,
                         cloudView,
                         cloudLab,
                         cloudFrame,
                         cloudLightVolume,
                         cloudVolume](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            cloudRenderer_.UpdateLightVolume(
                                commands,
                                *cloudVolume,
                                *cloudGpu,
                                cloudReferenceRadius,
                                cloudLayer,
                                atmosphereParameters,
                                cloudView,
                                cloudLab,
                                cloudFrame,
                                cloudLightVolume);
                        });

                    graph.AddPass(
                        prefix + ".Clouds",
                        {
                            {
                                .texture = targets.depth,
                                .state = rhi::ResourceState::DepthRead,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = cloudHandle,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         lightingDepth,
                         atmosphereLuts,
                         cloudTexture,
                         cloudGpu,
                         cloudLayer,
                         cloudReferenceRadius,
                         cloudWidth,
                         cloudHeight,
                         atmosphereParameters,
                         cloudView,
                         cloudLab,
                         cloudFrame,
                         cloudGodrays,
                         cloudVolumeDebug,
                         cloudVolume](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            cloudRenderer_.Draw(
                                commands,
                                *cloudVolume,
                                *lightingDepth,
                                *atmosphereLuts,
                                *cloudGpu,
                                *cloudTexture,
                                cloudWidth,
                                cloudHeight,
                                cloudReferenceRadius,
                                cloudLayer,
                                atmosphereParameters,
                                cloudView,
                                cloudLab,
                                cloudFrame,
                                cloudGodrays,
                                cloudVolumeDebug);
                        });

                    graph.AddPass(
                        prefix + ".CloudsResolve",
                        {
                            {
                                .texture = cloudHandle,
                                .state = rhi::ResourceState::ShaderResource,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = historyHandle,
                                .state = rhi::ResourceState::ShaderResource,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = resolvedHandle,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         cloudTexture,
                         historyTexture,
                         resolvedTexture,
                         cloudWidth,
                         cloudHeight,
                         cloudReferenceRadius,
                         cloudLayer,
                         nowView,
                         previousView,
                         historyValid](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            // A fifth of the result comes from the new march: about five frames
                            // of averaging, quick enough to follow weather and motion.
                            cloudRenderer_.Resolve(
                                commands,
                                *cloudTexture,
                                *historyTexture,
                                *resolvedTexture,
                                cloudWidth,
                                cloudHeight,
                                cloudReferenceRadius +
                                    0.5 * (cloudLayer.baseAltitudeMeters +
                                           cloudLayer.topAltitudeMeters),
                                nowView,
                                previousView,
                                historyValid,
                                0.2F);
                        });

                    temporal.previous = nowView;
                    temporal.previousLab = cloudLab;
                    temporal.valid = true;
                    temporal.writeIndex = 1U - writeIndex;

                    graph.AddPass(
                        prefix + ".CloudsComposite",
                        {
                            {
                                .texture = resolvedHandle,
                                .state = rhi::ResourceState::ShaderResource,
                                .access = render_graph::Access::Read
                            },
                            {
                                .texture = targets.color,
                                .state = rhi::ResourceState::RenderTarget,
                                .access = render_graph::Access::Write
                            }
                        },
                        [this,
                         resolvedTexture,
                         color,
                         width,
                         height](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            cloudRenderer_.Composite(
                                commands,
                                *resolvedTexture,
                                *color,
                                width,
                                height);
                        });
                }
            }

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

        if (!foregroundBodies->empty())
        {
            graph.AddPass(
                prefix + ".ForegroundBodies",
                {{
                    .texture = targets.color,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                }},
                [drawForegroundBodies](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    drawForegroundBodies(commands);
                });
        }

        if (drawPathDebug &&
            presentation ==
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

                StudioClipmapActiveRange activeRange{};
                if (const auto plan = clipmapPlanStats_.find(info.id);
                    plan != clipmapPlanStats_.end() && plan->second.valid &&
                    plan->second.dynamic)
                {
                    activeRange = {
                        .valid = true,
                        .firstLevel = plan->second.firstLevel,
                        .lastLevel = plan->second.lastLevel,
                        .suppressRings = plan->second.banded};
                }

                auto lines =
                    BuildTerrainDiagnosticOverlayLines(
                        diagnostics,
                        *terrainRuntime,
                        source,
                        pages,
                        view->Camera(),
                        activeRange);

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

        if (selectedVolume.has_value())
        {
            if (!sharedVolumeFields.has_value())
            {
                auto& runtimeVolume = *selectedVolume;

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
                        // One camera owns shared simulation residency. A map
                        // or second viewport must not recenter the same fields
                        // after the first viewport has queued GPU work.
                        const auto* followView = views.Find("studio.primary");
                        if (followView == nullptr ||
                            !followView->Camera().frame.IsValid())
                        {
                            followView = view;
                        }
                        runtimeVolume.centerMeters =
                            followView->Camera().
                                localPositionMeters;
                    }
                }

                volumeFields_->RemoveMissing(
                    session.World().Objects());

                sharedVolumeStorage = &volumeFields_->Ensure(runtimeVolume);

                static_cast<void>(
                    volumeFields_->
                        SyncAuthoredInputs(
                            session.World().Objects(),
                            runtimeVolume.object));

                sharedVolumeFields =
                    sharedVolumeStorage->Import(
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
                            *sharedVolumeStorage,
                            *sharedVolumeFields,
                            frameIndex %
                                framesInFlight_);

                    universalVolumeRenderer_.
                        RemoveMissing(
                            session.World().Objects());
                }
            }

            // Field allocation, input synchronization and the solver step are
            // shared by every viewport; only presentation is view-dependent.
            const auto& runtimeVolume = *selectedVolume;
            auto& fieldStorage = *sharedVolumeStorage;
            const auto& importedFields = *sharedVolumeFields;

            {
                if (surfaceVolumeSolver_ != nullptr)
                {
                    std::optional<scene::ObjectId>
                        volumeLightRoot;

                    if (logicalTarget->
                            target.has_value() &&
                        snapshot.hasWorld)
                    {
                        volumeLightRoot =
                            session.World().
                                Universe().
                                ObjectForBody(
                                    logicalTarget->
                                        target->body);
                    }

                    const auto volumeLocalLights =
                        ResolveStudioLocalLights(
                            session,
                            view->Lighting(),
                            volumeLightRoot);

                    render_graph::BufferHandle previousParticleLightGrid{};
                    if (volumeParticleRenderer_.ParticleLightGridReady())
                    {
                        previousParticleLightGrid = graph.ImportBuffer(
                            prefix + ".ParticleLightGridPrevious",
                            volumeParticleRenderer_.ParticleLightGrid(),
                            rhi::ResourceState::ShaderResource);
                    }

                    const lighting::DirectionalLight
                        volumeStellarLight{
                            .directionToLight =
                                studioDirectLight.
                                    directionBody,
                            .colorLinear = {
                                1.0F,
                                1.0F,
                                1.0F
                            },
                            .irradianceScale =
                                studioDirectLight.
                                    irradianceScale
                        };

                    universalVolumeRenderer_.
                        AddPasses(
                            graph,
                            prefix +
                                ".UniversalVolume",
                            info.id,
                            targets.color,
                            targets.depth,
                            width,
                            height,
                            view->Camera(),
                            view->Lighting(),
                            runtimeVolume,
                            fieldStorage,
                            importedFields,
                            volumeStellarLight,
                            volumeLocalLights,
                            sharedRadianceCellsHandle,
                            sharedRadianceLevelsHandle,
                            sharedRadianceLevelCount,
                            previousParticleLightGrid,
                            view->Lighting().change !=
                                lighting::
                                    LightingViewChange::
                                        None);

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
            const auto particleTerrainCollisionPages =
                volumeParticleRenderer_.TerrainCollisionPagesSnapshot();

            recordComposeStage("gi_passes");
            recordComposeStage("atmosphere");
            // M38 particle simulation is shared by all Studio viewports. The
            // authoritative output generation advances GPU state exactly once;
            // later viewports only render that already-advanced state.
            const auto& particleOutput =
                studio_session::VolumeParticleOutputs();
            const u64 outputGeneration =
                particleOutput.Diagnostics().generation;

            if (outputGeneration < particleOutputGeneration_)
            {
                volumeParticleRenderer_.Reset();
                particleOutputGeneration_ = 0U;
                particleSimulationTimeValid_ = false;
                particlePresentationOriginMeters_ = {};
            }

            bool advanceParticleState = false;
            f64 particleDeltaSeconds = 0.0;
            math::Double3 previousParticleOrigin =
                particlePresentationOriginMeters_;
            math::Double3 nextParticleOrigin =
                particlePresentationOriginMeters_;

            if (outputGeneration > 0U &&
                outputGeneration != particleOutputGeneration_)
            {
                nextParticleOrigin =
                    view->Camera().localPositionMeters;
                previousParticleOrigin =
                    particleOutputGeneration_ == 0U
                        ? nextParticleOrigin
                        : particlePresentationOriginMeters_;

                const auto particleSpawns =
                    BuildVolumeParticleRenderBatch(
                        particleOutput.Events(),
                        nextParticleOrigin);
                volumeParticleRenderer_.SetSpawns(
                    particleSpawns);

                if (particleSimulationTimeValid_)
                {
                    particleDeltaSeconds =
                        static_cast<f64>(
                            (atTime - particleSimulationTime_).count()) /
                        1000000.0;
                    if (!std::isfinite(particleDeltaSeconds) ||
                        particleDeltaSeconds < 0.0)
                    {
                        particleDeltaSeconds = 0.0;
                    }
                }

                particleOutputGeneration_ =
                    outputGeneration;
                particleSimulationTime_ = atTime;
                particleSimulationTimeValid_ = true;
                particlePresentationOriginMeters_ =
                    nextParticleOrigin;
                advanceParticleState = true;
            }

            if (particleOutputGeneration_ > 0U)
            {
                const auto camera =
                    view->Camera();
                std::optional<scene::ObjectId> particleLightRoot;
                if(logicalTarget->target.has_value() && snapshot.hasWorld)
                {
                    particleLightRoot=session.World().Universe().ObjectForBody(logicalTarget->target->body);
                }
                const auto particleLocalLights=ResolveStudioLocalLights(session,view->Lighting(),particleLightRoot);
                const lighting::DirectionalLight particleStellarLight{.directionToLight=studioDirectLight.directionBody,.colorLinear={1.0F,1.0F,1.0F},.irradianceScale=studioDirectLight.irradianceScale};
                const math::Double3 cameraRelativeToParticleOrigin{
                    camera.localPositionMeters.x -
                        particlePresentationOriginMeters_.x,
                    camera.localPositionMeters.y -
                        particlePresentationOriginMeters_.y,
                    camera.localPositionMeters.z -
                        particlePresentationOriginMeters_.z
                };
                auto* particleColor = color;
                auto* particleDepth = &view->Depth();
            const u32 particleFrameIndex =
                frameIndex % framesInFlight_;
            const u64 particleTemporalHistoryKey =
                StableViewportHash(info.id);

                graph.AddPass(
                    prefix + ".VolumeParticles",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::RenderTarget,
                            .access =
                                render_graph::Access::Write
                        },
                        {
                            .texture = targets.depth,
                            .state =
                                rhi::ResourceState::DepthRead,
                            .access =
                                render_graph::Access::Read
                        }
                    },
                    [this,
                     particleColor,
                     particleDepth,
                     width,
                     height,
                     camera,
                     cameraRelativeToParticleOrigin,
                     particleFrameIndex,
                     particleTemporalHistoryKey,
                     particleStellarLight,
                     particleLocalLights,
                     advanceParticleState,
                     particleDeltaSeconds,
                     previousParticleOrigin,
                     nextParticleOrigin,
                     particleTerrainCollisionPages](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        if (advanceParticleState)
                        {
                            volumeParticleRenderer_.Advance(
                                commands,
                                particleFrameIndex,
                                particleDeltaSeconds,
                                previousParticleOrigin,
                                nextParticleOrigin);
                            volumeParticleRenderer_.ApplyTerrainCollision(
                                commands,
                                particleTerrainCollisionPages,
                                particleDeltaSeconds);
                        }

                        volumeParticleRenderer_.Draw(
                            commands,
                            *particleColor,
                            *particleDepth,
                            width,
                            height,
                            camera,
                            cameraRelativeToParticleOrigin,
                            particleFrameIndex,
                            particleTemporalHistoryKey,
                            particleStellarLight,
                            particleLocalLights);
                    });

                // The particle pass leaves depth in DepthRead; return it to
                // the persistent DepthWrite state RenderView imports next frame.
                graph.AddPass(
                    prefix + ".RestoreDepthWriteAfterParticles",
                    {
                        {
                            .texture = targets.depth,
                            .state =
                                rhi::ResourceState::DepthWrite,
                            .access =
                                render_graph::Access::Write
                        }
                    },
                    [](
                        rhi::CommandList&,
                        const render_graph::Resources&)
                    {
                    });
            }
        }

        // Mesh distance-field debug view: sphere traces the merged field and
        // shows it in place of (or beside) the rasterised scene.
        if (info.layers.sdfDebugView != 0U &&
            meshSdfScene_.Volume().ready &&
            view->SurfaceDebugMode() == lighting::SurfaceDebugMode::Lit &&
            width > 0U && height > 0U)
        {
            auto& scratch = sdfDebugScratch_[info.id];
            if (scratch == nullptr || scratch->Width() != width ||
                scratch->Height() != height)
            {
                scratch = device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource,
                    .allowUnorderedAccess = true});
            }
            const auto scratchHandle = graph.ImportTexture(
                prefix + ".SdfDebugScratch",
                *scratch,
                rhi::ResourceState::ShaderResource);

            graph.AddPass(
                prefix + ".SdfDebug",
                {
                    {.texture = targets.color,
                     .state = rhi::ResourceState::ShaderResource,
                     .access = render_graph::Access::Read},
                    {.texture = scratchHandle,
                     .state = rhi::ResourceState::UnorderedAccess,
                     .access = render_graph::Access::Write}
                },
                [this,
                 color,
                 scratch = scratch.get(),
                 width,
                 height,
                 lightingView = view->Lighting(),
                 sun = studioDirectLight.directionBody,
                 mode = static_cast<u32>(info.layers.sdfDebugView)](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    meshSdfDebugRenderer_.Draw(
                        commands,
                        meshSdfScene_.Volume(),
                        *color,
                        *scratch,
                        width,
                        height,
                        lightingView,
                        sun,
                        mode);
                });
            graph.AddPass(
                prefix + ".SdfDebugCopyBack",
                {
                    {.texture = scratchHandle,
                     .state = rhi::ResourceState::ShaderResource,
                     .access = render_graph::Access::Read},
                    {.texture = targets.color,
                     .state = rhi::ResourceState::RenderTarget,
                     .access = render_graph::Access::Write}
                },
                [this,
                 color,
                 scratch = scratch.get(),
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    debugComposite_.Draw(
                        commands, *scratch, *color, width, height);
                });
        }

        // Anti-aliasing of the HDR scene colour, after every scene pass and
        // before exposure / tone mapping. TAA reprojects its history with the
        // (jittered) camera pair and depth; frames without usable history use
        // FXAA instead.
        if (antiAliasingMode != post_process::AntiAliasingMode::Off &&
            view->SurfaceDebugMode() == lighting::SurfaceDebugMode::Lit &&
            width > 0U && height > 0U)
        {
            auto& aa = antiAliasingPresentations_[info.id];

            if (aa.scratch == nullptr || aa.width != width ||
                aa.height != height)
            {
                const auto make = [&]
                {
                    return device_->CreateTexture({
                        .width = width,
                        .height = height,
                        .format = rhi::TextureFormat::RGBA16_Float,
                        .initialState = rhi::ResourceState::ShaderResource,
                        .allowUnorderedAccess = true});
                };
                aa.scratch = make();
                aa.history[0] = make();
                aa.history[1] = make();
                aa.width = width;
                aa.height = height;
                aa.hasHistory = false;
                aa.readIndex = 0U;
            }

            // Reproject with the UN-jittered cameras: the history then stays
            // in a fixed screen grid (a static scene reads the same history
            // pixel every frame) and the jitter only changes what each frame
            // samples. Reprojecting with the jittered pair would resample the
            // history at a different sub-pixel phase each frame and shimmer.
            const auto& camera = aa.base;
            const post_process::TaaCamera currentCamera{
                .positionMeters = camera.localPositionMeters,
                .forward = camera.forward,
                .up = camera.up,
                .verticalFovRadians = camera.verticalFovRadians,
                .nearPlaneMeters = camera.nearPlaneMeters,
                .farPlaneMeters = camera.farPlaneMeters};

            const auto useDepth = render_graph::TextureUse{
                .texture = targets.depth,
                .state = rhi::ResourceState::DepthRead,
                .access = render_graph::Access::Read};
            const auto colorRead = render_graph::TextureUse{
                .texture = targets.color,
                .state = rhi::ResourceState::ShaderResource,
                .access = render_graph::Access::Read};
            const auto colorWrite = render_graph::TextureUse{
                .texture = targets.color,
                .state = rhi::ResourceState::RenderTarget,
                .access = render_graph::Access::Write};

            if (antiAliasingMode == post_process::AntiAliasingMode::Taa)
            {
                const u32 readIndex = aa.readIndex;
                const u32 writeIndex = 1U - readIndex;
                const bool historyValid =
                    aa.hasHistory &&
                    post_process::TaaHistoryUsable(
                        aa.previousCamera, currentCamera);

                const auto historyReadHandle = graph.ImportTexture(
                    prefix + ".TaaHistoryRead",
                    *aa.history[readIndex],
                    rhi::ResourceState::ShaderResource);
                const auto historyWriteHandle = graph.ImportTexture(
                    prefix + ".TaaHistoryWrite",
                    *aa.history[writeIndex],
                    rhi::ResourceState::ShaderResource);

                if (historyValid)
                {
                    graph.AddPass(
                        prefix + ".TaaResolve",
                        {
                            colorRead,
                            useDepth,
                            {.texture = historyReadHandle,
                             .state = rhi::ResourceState::ShaderResource,
                             .access = render_graph::Access::Read},
                            {.texture = historyWriteHandle,
                             .state = rhi::ResourceState::UnorderedAccess,
                             .access = render_graph::Access::Write}
                        },
                        [this,
                         color,
                         depthTexture = &view->Depth(),
                         historyRead = aa.history[readIndex].get(),
                         historyWrite = aa.history[writeIndex].get(),
                         width,
                         height,
                         currentCamera,
                         previousCamera = aa.previousCamera](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            antiAliasingRenderer_.Taa(
                                commands,
                                *color,
                                *depthTexture,
                                *historyRead,
                                *historyWrite,
                                width,
                                height,
                                currentCamera,
                                previousCamera,
                                true);
                        });
                }
                else
                {
                    graph.AddPass(
                        prefix + ".TaaFallbackFxaa",
                        {
                            colorRead,
                            {.texture = historyWriteHandle,
                             .state = rhi::ResourceState::UnorderedAccess,
                             .access = render_graph::Access::Write}
                        },
                        [this,
                         color,
                         historyWrite = aa.history[writeIndex].get(),
                         width,
                         height](
                            rhi::CommandList& commands,
                            const render_graph::Resources&)
                        {
                            antiAliasingRenderer_.Fxaa(
                                commands, *color, *historyWrite, width, height);
                        });
                }

                graph.AddPass(
                    prefix + ".TaaCopyBack",
                    {
                        {.texture = historyWriteHandle,
                         .state = rhi::ResourceState::ShaderResource,
                         .access = render_graph::Access::Read},
                        colorWrite
                    },
                    [this,
                     color,
                     historyWrite = aa.history[writeIndex].get(),
                     width,
                     height](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        debugComposite_.Draw(
                            commands, *historyWrite, *color, width, height);
                    });

                aa.readIndex = writeIndex;
                aa.hasHistory = true;
                aa.previousCamera = currentCamera;
            }
            else
            {
                const auto scratchHandle = graph.ImportTexture(
                    prefix + ".FxaaScratch",
                    *aa.scratch,
                    rhi::ResourceState::ShaderResource);

                graph.AddPass(
                    prefix + ".FxaaResolve",
                    {
                        colorRead,
                        {.texture = scratchHandle,
                         .state = rhi::ResourceState::UnorderedAccess,
                         .access = render_graph::Access::Write}
                    },
                    [this,
                     color,
                     scratch = aa.scratch.get(),
                     width,
                     height](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        antiAliasingRenderer_.Fxaa(
                            commands, *color, *scratch, width, height);
                    });

                graph.AddPass(
                    prefix + ".FxaaCopyBack",
                    {
                        {.texture = scratchHandle,
                         .state = rhi::ResourceState::ShaderResource,
                         .access = render_graph::Access::Read},
                        colorWrite
                    },
                    [this,
                     color,
                     scratch = aa.scratch.get(),
                     width,
                     height](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        debugComposite_.Draw(
                            commands, *scratch, *color, width, height);
                    });

                aa.hasHistory = false;
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

                if (!histogram.diagnostics.eyeAdaptationLocked)
                {
                    // The display's reference white and peak luminance (nits)
                    // are the tone-mapping config's: the eye protects the
                    // highlights against what that display can actually show.
                    auto eyeConfig =
                        histogram.diagnostics.eyeConfig;
                    eyeConfig.referenceWhiteNits =
                        histogram.diagnostics.toneMapping.
                            referenceWhiteNits;
                    eyeConfig.highlightTargetNits =
                        histogram.diagnostics.toneMapping.
                            peakNits;

                    histogram.diagnostics.eyeState =
                        post_process::
                            UpdateHumanEyeAdaptation(
                                histogram.diagnostics.eyeState,
                                histogram.diagnostics.statistics,
                                eyeDeltaSeconds,
                                eyeConfig);
                }

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

        if (presentation == StudioViewportPresentation::FlatMap)
        {
            graph.AddPass(
                prefix + ".FlatMap",
                {
                    {
                        .texture = targets.display,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 displayColor,
                 width,
                 height,
                 viewId = info.id,
                 frameSlot = frameIndex % framesInFlight_](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    flatMapRenderer_.Draw(
                        commands,
                        *displayColor,
                        width,
                        height,
                        viewId,
                        frameSlot);
                });
        }

        if (logicalTarget->mode == studio_session::ViewportMode::BodyMap &&
            terrainRuntime.has_value())
        {
            // Graticule and observer marker over the globe view, drawn after
            // the output transform like the flat map.
            graph.AddPass(
                prefix + ".GlobeOverlay",
                {
                    {
                        .texture = targets.display,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 displayColor,
                 width,
                 height,
                 camera = view->Camera(),
                 radius = terrainRuntime->planet.radiusMeters,
                 marker = math::LengthSquared(
                              terrainRuntime->observer.meters) > 0.0
                     ? std::optional<math::Double3>(
                           terrainRuntime->observer.meters)
                     : std::nullopt](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    flatMapRenderer_.DrawGlobeOverlay(
                        commands,
                        *displayColor,
                        width,
                        height,
                        camera,
                        radius,
                        marker);
                });
        }

        rendered.push_back({
            .id = info.id,
            .targets = targets,
            .targeted = shape.has_value()
        });
        recordComposeStage("post");
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
