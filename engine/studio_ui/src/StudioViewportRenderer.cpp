#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/celestial_radiometry/Radiometry.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>
#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>
#include <orbit/world_model/CelestialAtmosphereBinding.hpp>
#include <orbit/world_model/CelestialCloudBinding.hpp>
#include <orbit/world_model/CelestialGiantBinding.hpp>
#include <orbit/world_model/CelestialOceanBinding.hpp>
#include <orbit/world_model/CelestialRadiometryBinding.hpp>
#include <orbit/world_model/CelestialRingBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/world_model/LocalLightBinding.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuPhysicalPageComposite.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace orbit::studio_ui
{
namespace
{
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
      pathRenderer_(device, compiler),
      debugComposite_(device, compiler),
      directLightingRenderer_(device, compiler),
      surfaceDebugRenderer_(device, compiler),
      displayResolveRenderer_(device, compiler),
      colorLutRenderer_(device, compiler),
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

void StudioViewportRenderer::SetColorLut(
    post_process::ColorLutData lut)
{
    if (device_ == nullptr)
    {
        throw std::logic_error(
            "Studio viewport renderer has no device for LUT replacement.");
    }

    colorLut_ =
        std::make_unique<
            post_process::GpuColorLut>(
                *device_,
                lut);
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
    const terrain::TerrainSource& terrainSource)
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

    if (recreate)
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

std::vector<StudioRenderedView>
StudioViewportRenderer::Compose(
    render_graph::RenderGraph& graph,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    studio_session::StudioRuntimeBinding& runtime,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug,
    const u32 frameIndex)
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
                                    true
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

                if (cloudPresentation.field ==
                        nullptr ||
                    cloudPresentation.body !=
                        logicalTarget->
                            target->body ||
                    cloudPresentation.fingerprint !=
                        cloudFingerprint)
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
                }

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

            if (rings.nearMesh == nullptr ||
                rings.farMesh == nullptr ||
                rings.body !=
                    logicalTarget->
                        target->body ||
                rings.fingerprint !=
                    semanticFingerprint ||
                rings.referenceRadiusMeters !=
                    referenceRadius)
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
            }

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
            ringPresentations_.erase(
                info.id);
            ringDiagnostics_.erase(
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

            if (staticChanged)
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
            }

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

            if (presentation.skyView ==
                    nullptr ||
                presentation.skyFingerprint !=
                    skyFingerprint)
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
            }

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
                    }
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
                        *macroGlobeSurface->terrain);

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
                    *macroGlobeSurface->terrain);

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
                        }
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

                if (resolvedGiantForView.has_value())
                {
                    auto& giantPresentation =
                        giantPresentations_[info.id];

                    if (giantPresentation.appearance ==
                            nullptr ||
                        giantPresentation.body !=
                            logicalTarget->
                                target->body ||
                        giantPresentation.fingerprint !=
                            resolvedGiantForView->
                                fingerprint)
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
                    }

                    giantAppearanceSummary =
                        giantPresentation.summary;
                }
                else
                {
                    giantPresentations_.erase(
                        info.id);
                    giantDiagnostics_.erase(
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
                     bodyShape](
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
                            camera);
                        bodyRenderer_.DrawSurfaceData(
                            commands,
                            *bodySurfaceBaseRoughness,
                            *bodySurfaceNormalMetallic,
                            *bodySurfaceEmissionClass,
                            width,
                            height,
                            bodyShape,
                            camera);
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
            presentation ==
                StudioViewportPresentation::ProductionTerrain ||
            presentation ==
                StudioViewportPresentation::MacroGlobe ||
            presentation ==
                StudioViewportPresentation::BodyPreview;

        if (usesPhysicalSurfaceLighting)
        {
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
                 localIndicesHandle](
                    rhi::CommandList& commands,
                    const render_graph::Resources&
                        resources)
                {
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
                });

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
                        pathProducts,
                        atTime);
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

        if (colorLut_ == nullptr)
        {
            throw std::logic_error(
                "Studio viewport LUT correction has no GPU LUT.");
        }

        auto* displayLinear =
            &view->DisplayLinear();
        auto* displayColor =
            &view->DisplayColor();
        auto* colorLut =
            colorLut_.get();
        const auto displayResolveSettings =
            displayResolveSettings_;
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
                 displayResolveSettings](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    displayResolveRenderer_.Draw(
                        commands,
                        *color,
                        *displayLinear,
                        width,
                        height,
                        displayResolveSettings);
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
             displayLinear,
             displayColor,
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
                    *displayColor,
                    width,
                    height,
                    *colorLut,
                    colorLutSettings);
            });

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

    return rendered;
}
} // namespace orbit::studio_ui
