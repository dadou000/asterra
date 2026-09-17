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

StudioRenderViewSet::~StudioRenderViewSet()
{
    if (session_ == nullptr)
    {
        return;
    }

    auto& targets = session_->Viewports();

    for (const auto& [id, view] : views_)
    {
        static_cast<void>(view);
        static_cast<void>(targets.Unregister(id));
    }
}

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

    session_->Viewports().Register(
        id,
        mode,
        followActiveBody);

    try
    {
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
        static_cast<void>(
            session_->Viewports().Unregister(id));
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

        const auto camera =
            ComposeViewportCamera(
                *target,
                snapshot.universeGeneration);

        if (!camera.has_value())
        {
            view->Camera() = {};
            continue;
        }

        ApplyViewportCamera(
            *view,
            *camera);
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
                target->target.has_value()
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
