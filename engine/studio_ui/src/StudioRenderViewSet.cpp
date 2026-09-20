#include <orbit/studio_ui/StudioRenderViewSet.hpp>

#include <orbit/studio_ui/StudioViewportCamera.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
StudioRenderViewSet::StudioRenderViewSet(
    rhi::Device& device,
    studio_session::StudioSession& session) noexcept
    : device_(&device),
      session_(&session)
{
}

StudioRenderViewSet::~StudioRenderViewSet() = default;

void StudioRenderViewSet::CreateDefaults()
{
    if (!views_.contains("studio.primary"))
    {
        Create(
            "studio.primary",
            studio_session::ViewportMode::Perspective,
            true,
            {
                .width = 960,
                .height = 640
            });
    }

    if (!views_.contains("studio.map"))
    {
        Create(
            "studio.map",
            studio_session::ViewportMode::BodyMap,
            true,
            {
                .width = 640,
                .height = 480
            });
    }
}

void StudioRenderViewSet::Create(
    std::string id,
    const studio_session::ViewportMode mode,
    const bool followActiveBody,
    render_view::RenderViewDesc desc)
{
    if (device_ == nullptr || session_ == nullptr)
    {
        throw std::logic_error(
            "Studio render-view set has no device/session binding.");
    }

    if (id.empty())
    {
        throw std::invalid_argument(
            "Studio render-view ID must not be empty.");
    }

    if (views_.contains(id) ||
        session_->Viewports().Find(id) != nullptr)
    {
        throw std::invalid_argument(
            "Studio render-view ID is already in use.");
    }

    desc.width = std::max(desc.width, 1U);
    desc.height = std::max(desc.height, 1U);

    const std::string targetId = id;
    session_->Viewports().Register(
        targetId,
        mode,
        followActiveBody);

    try
    {
        debugFields_.emplace(
            targetId,
            terrain_debug::TerrainDebugField::Uplift);
        debugPhysicalPageLevels_.emplace(
            targetId,
            static_cast<u8>(8));

        auto view =
            std::make_unique<render_view::RenderView>(
                *device_,
                desc);
        views_.emplace(
            std::move(id),
            std::move(view));
    }
    catch (...)
    {
        debugFields_.erase(targetId);
        debugPhysicalPageLevels_.erase(targetId);
        debugPhysicalPages_.erase(targetId);
        liveDebugPages_.erase(targetId);
        static_cast<void>(
            session_->Viewports().Unregister(
                targetId));
        throw;
    }
}

bool StudioRenderViewSet::Destroy(
    const std::string_view id) noexcept
{
    const auto found = views_.find(id);

    if (found == views_.end())
    {
        return false;
    }

    const std::string ownedId = found->first;
    views_.erase(found);
    debugFields_.erase(ownedId);
    debugPhysicalPageLevels_.erase(ownedId);
    debugPhysicalPages_.erase(ownedId);
    liveDebugPages_.erase(ownedId);

    if (session_ != nullptr)
    {
        static_cast<void>(
            session_->Viewports().Unregister(ownedId));
    }

    return true;
}

void StudioRenderViewSet::Resize(
    const std::string_view id,
    const u32 width,
    const u32 height)
{
    auto* view = Find(id);

    if (view == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    view->Resize(
        std::max(width, 1U),
        std::max(height, 1U));
}

void StudioRenderViewSet::SetDebugField(
    const std::string_view id,
    const terrain_debug::TerrainDebugField field)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    static_cast<void>(
        terrain_debug::Descriptor(field));

    debugFields_[std::string(id)] = field;
}

terrain_debug::TerrainDebugField
StudioRenderViewSet::DebugField(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found = debugFields_.find(id);
    if (found == debugFields_.end())
    {
        return terrain_debug::TerrainDebugField::Uplift;
    }

    return found->second;
}

void StudioRenderViewSet::SetDebugPhysicalPageLevel(
    const std::string_view id,
    const u8 level)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    if (level > 30U)
    {
        throw std::invalid_argument(
            "Physical terrain page tile level must be in [0,30].");
    }

    debugPhysicalPageLevels_[std::string(id)] = level;
    debugPhysicalPages_.erase(std::string(id));
    liveDebugPages_.erase(std::string(id));
}

u8 StudioRenderViewSet::DebugPhysicalPageLevel(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        debugPhysicalPageLevels_.find(id);

    return found == debugPhysicalPageLevels_.end()
        ? static_cast<u8>(8)
        : found->second;
}

bool StudioRenderViewSet::SelectDebugPhysicalPage(
    const std::string_view id,
    const f32 u,
    const f32 v)
{
    auto* view = Find(id);

    if (view == nullptr || session_ == nullptr)
    {
        return false;
    }

    const auto* target =
        session_->Viewports().Find(id);

    if (target == nullptr)
    {
        return false;
    }

    const auto selection =
        PhysicalPageAtViewportPoint(
            *target,
            view->Camera(),
            view->Width(),
            view->Height(),
            u,
            v,
            DebugPhysicalPageLevel(id));

    if (!selection.has_value())
    {
        return false;
    }

    debugPhysicalPages_.insert_or_assign(
        std::string(id),
        *selection);
    liveDebugPages_.erase(std::string(id));
    return true;
}

std::optional<StudioPhysicalPageSelection>
StudioRenderViewSet::DebugPhysicalPage(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        debugPhysicalPages_.find(id);

    return found == debugPhysicalPages_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::shared_ptr<const terrain_debug::TerrainDebugPageData>
StudioRenderViewSet::LiveDebugPage(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        liveDebugPages_.find(id);

    return found == liveDebugPages_.end()
        ? nullptr
        : found->second;
}

u32 StudioRenderViewSet::Refresh(
    const studio_session::StudioRuntimeSnapshot& snapshot)
{
    RequireCurrentSnapshot(snapshot);

    u32 targetedViews = 0;

    for (auto& [id, view] : views_)
    {
        const auto* target =
            session_->Viewports().Find(id);

        if (target == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView lost its logical viewport target slot.");
        }

        std::optional<
            render_view::CameraState>
            camera;

        if (target->mode ==
                studio_session::ViewportMode::Perspective)
        {
            const auto terrain =
                session_->TerrainRuntime().
                    Capture(id);

            if (terrain.has_value())
            {
                if (!session_->
                        TerrainRuntime().
                        IsCurrent(*terrain))
                {
                    throw std::logic_error(
                        "Studio RenderView received a stale terrain runtime snapshot.");
                }

                camera =
                    ComposeTerrainViewportCamera(
                        *target,
                        *terrain);
            }
        }

        if (!camera.has_value())
        {
            camera =
                ComposeViewportCamera(
                    *target,
                    snapshot.
                        universeGeneration);
        }

        if (!camera.has_value())
        {
            view->Camera() = {};
            debugPhysicalPages_.erase(id);
            liveDebugPages_.erase(id);
            continue;
        }

        ApplyViewportCamera(
            *view,
            *camera);

        const auto selected =
            debugPhysicalPages_.find(id);

        if (target->target.has_value() &&
            selected != debugPhysicalPages_.end())
        {
            const world::PlanetId expectedPlanet{
                .high = target->target->body.high,
                .low = target->target->body.low
            };

            if (selected->second.address.planet !=
                expectedPlanet)
            {
                debugPhysicalPages_.erase(selected);
                liveDebugPages_.erase(id);
            }
        }

        if (target->mode ==
                studio_session::ViewportMode::Debug &&
            !debugPhysicalPages_.contains(id))
        {
            static_cast<void>(
                SelectDebugPhysicalPage(
                    id,
                    0.5F,
                    0.5F));
        }

        const auto selectedPage =
            debugPhysicalPages_.find(id);

        if (target->mode ==
                studio_session::ViewportMode::Debug &&
            selectedPage != debugPhysicalPages_.end())
        {
            auto live =
                session_->TerrainDebugPages().Find(
                    selectedPage->second.address);

            if (live != nullptr)
            {
                liveDebugPages_.insert_or_assign(
                    id,
                    std::move(live));
            }
            else
            {
                liveDebugPages_.erase(id);
            }
        }
        else
        {
            liveDebugPages_.erase(id);
        }

        ++targetedViews;
    }

    return targetedViews;
}

render_view::RenderView* StudioRenderViewSet::Find(
    const std::string_view id) noexcept
{
    const auto found = views_.find(id);
    return found == views_.end()
        ? nullptr
        : found->second.get();
}

const render_view::RenderView* StudioRenderViewSet::Find(
    const std::string_view id) const noexcept
{
    const auto found = views_.find(id);
    return found == views_.end()
        ? nullptr
        : found->second.get();
}

std::vector<StudioRenderViewInfo>
StudioRenderViewSet::Catalog() const
{
    std::vector<StudioRenderViewInfo> result;
    result.reserve(views_.size());

    for (const auto& [id, view] : views_)
    {
        const auto* target =
            session_ != nullptr
                ? session_->Viewports().Find(id)
                : nullptr;

        result.push_back({
            .id = id,
            .mode = target != nullptr
                ? target->mode
                : studio_session::ViewportMode::Perspective,
            .width = view->Width(),
            .height = view->Height(),
            .hasTarget =
                target != nullptr &&
                target->target.has_value(),
            .debugField =
                DebugField(id),
            .debugPhysicalPageLevel =
                DebugPhysicalPageLevel(id),
            .debugPhysicalPage =
                DebugPhysicalPage(id),
            .hasLiveDebugPage =
                LiveDebugPage(id) != nullptr
        });
    }

    return result;
}

void StudioRenderViewSet::RequireCurrentSnapshot(
    const studio_session::StudioRuntimeSnapshot& snapshot) const
{
    if (session_ == nullptr)
    {
        throw std::logic_error(
            "Studio render-view set has no session binding.");
    }

    const auto& world = session_->World();

    if (snapshot.hasWorld != world.HasWorld() ||
        snapshot.worldGeneration != world.Generation() ||
        snapshot.universeGeneration != world.UniverseGeneration())
    {
        throw std::logic_error(
            "Studio RenderView refresh received a stale runtime snapshot.");
    }
}
} // namespace orbit::studio_ui
