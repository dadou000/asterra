#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>
#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuPhysicalPageComposite.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

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
      pathRenderer_(device, compiler),
      debugComposite_(device, compiler)
{
    if (framesInFlight_ == 0U)
    {
        throw std::invalid_argument(
            "Studio terrain rendering requires at least one frame-in-flight slot.");
    }
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

    const bool recreate =
        presentation.product == nullptr ||
        presentation.appearanceProduct ==
            nullptr ||
        presentation.body != body ||
        presentation.sourceRevision !=
            sourceRevision ||
        presentation.fingerprint !=
            geometryFingerprint ||
        presentation.appearanceFingerprint !=
            appearanceFingerprint;

    if (recreate)
    {
        const auto mesh =
            celestial_globe::
                BuildMacroGlobe(
                    terrainSource,
                    shape,
                    globeConfig);

        const auto appearance =
            celestial_appearance::
                BuildPlanetaryAppearance(
                    terrainSource,
                    sphericalPlanet->
                        radiusMeters,
                    appearanceConfig);

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
        presentation.appearanceFingerprint =
            appearanceFingerprint;
        presentation.appearanceTexels =
            static_cast<u32>(
                appearance.texels.size());
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
            macroGlobePresentations_.erase(
                info.id);

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

            graph.AddPass(
                prefix + ".ProductionTerrain",
                {
                    {
                        .texture = targets.color,
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

                    commands.ClearDepthTarget(
                        *depth,
                        0.0F);

                    commands.SetRenderTargets(
                        *color,
                        *depth);

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
            break;
        }

        case StudioViewportPresentation::TerrainDebug:
        {
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
            terrainPresentations_.erase(
                info.id);

            if (device_ == nullptr ||
                macroGlobeSurface == nullptr ||
                macroGlobeSurface->terrain == nullptr ||
                !logicalTarget->target.has_value())
            {
                throw std::logic_error(
                    "Studio macro-globe presentation lost its terrain authority, device, or target body.");
            }

            auto& presentation =
                macroGlobePresentations_[
                    info.id];

            const celestial_globe::MacroGlobeConfig
                globeConfig{
                    .faceResolution = 33U,
                    .footprintScale = 1.5
                };

            const u64 fingerprint =
                celestial_globe::
                    MacroGlobeFingerprint(
                        *macroGlobeSurface->terrain,
                        *shape,
                        globeConfig);

            const u64 sourceRevision =
                macroGlobeSurface->
                    terrain->
                    Revision();

            const auto sphericalPlanet =
                session.World().
                    Surfaces().
                    Registry().
                    SphericalPlanetDefinition(
                        logicalTarget->target->body);

            if (!sphericalPlanet.has_value())
            {
                throw std::logic_error(
                    "Studio planetary appearance currently requires the spherical terrain body contract.");
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
                        *macroGlobeSurface->terrain,
                        sphericalPlanet->
                            radiusMeters,
                        appearanceConfig);

            const bool recreate =
                presentation.product == nullptr ||
                presentation.appearanceProduct ==
                    nullptr ||
                presentation.body !=
                    logicalTarget->target->body ||
                presentation.sourceRevision !=
                    sourceRevision ||
                presentation.fingerprint !=
                    fingerprint ||
                presentation.appearanceFingerprint !=
                    appearanceFingerprint;

            if (recreate)
            {
                const auto mesh =
                    celestial_globe::
                        BuildMacroGlobe(
                            *macroGlobeSurface->terrain,
                            *shape,
                            globeConfig);

                const auto appearance =
                    celestial_appearance::
                        BuildPlanetaryAppearance(
                            *macroGlobeSurface->terrain,
                            sphericalPlanet->
                                radiusMeters,
                            appearanceConfig);

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
                    logicalTarget->target->body;
                presentation.sourceRevision =
                    sourceRevision;
                presentation.fingerprint =
                    fingerprint;
                presentation.appearanceFingerprint =
                    appearanceFingerprint;
                presentation.appearanceTexels =
                    static_cast<u32>(
                        appearance.texels.size());
            }

            auto* globe =
                presentation.product.get();
            const auto camera =
                view->Camera();

            graph.AddPass(
                prefix + ".MacroGlobe",
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
                 globe,
                 camera](
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

                    macroGlobeRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        *globe,
                        camera);
                });

            break;
        }

        case StudioViewportPresentation::BodyPreview:
        {
            macroGlobePresentations_.erase(
                info.id);

            terrainPresentations_.erase(
                info.id);

            const auto camera = view->Camera();
            const auto bodyShape = *shape;

            graph.AddPass(
                prefix + ".Body",
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
                 bodyShape](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    bodyRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        bodyShape,
                        camera);
                });

            if (frames != nullptr &&
                !products.empty())
            {
                const auto* frameGraph = frames;
                const auto pathProducts = products;

                graph.AddPass(
                    prefix + ".Paths",
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
                     frameGraph,
                     pathProducts,
                     atTime,
                     drawPathDebug](
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
                            std::span<
                                const path_geometry::PathDerivedProduct* const>(
                                    pathProducts.data(),
                                    pathProducts.size()),
                            drawPathDebug);
                    });
            }
            break;
        }

        case StudioViewportPresentation::Blank:
        {
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
