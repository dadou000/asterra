#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
struct StudioRenderViewInfo
{
    std::string id;
    studio_session::ViewportMode mode{
        studio_session::ViewportMode::Perspective};
    u32 width{1};
    u32 height{1};
    bool hasTarget{false};
};

// Owns the actual resizable GPU RenderViews corresponding to logical Studio
// viewport target slots. Target state remains in StudioSession; this class owns
// only presentation resources and never persists BodyId/FrameId authority.
class StudioRenderViewSet
{
public:
    StudioRenderViewSet(
        rhi::Device& device,
        studio_session::StudioSession& session) noexcept;
    ~StudioRenderViewSet();

    StudioRenderViewSet(
        const StudioRenderViewSet&) = delete;
    StudioRenderViewSet& operator=(
        const StudioRenderViewSet&) = delete;

    void CreateDefaults();

    void Create(
        std::string id,
        studio_session::ViewportMode mode,
        bool followActiveBody,
        render_view::RenderViewDesc desc);

    [[nodiscard]] bool Destroy(
        std::string_view id) noexcept;

    void Resize(
        std::string_view id,
        u32 width,
        u32 height);

    // Applies generation-validated target cameras to every GPU view. Missing
    // targets are normal for blank worlds and leave a neutral unbound camera.
    [[nodiscard]] u32 Refresh(
        const studio_session::StudioRuntimeSnapshot& snapshot);

    [[nodiscard]] render_view::RenderView* Find(
        std::string_view id) noexcept;
    [[nodiscard]] const render_view::RenderView* Find(
        std::string_view id) const noexcept;

    [[nodiscard]] std::vector<StudioRenderViewInfo>
    Catalog() const;

private:
    void RequireCurrentSnapshot(
        const studio_session::StudioRuntimeSnapshot& snapshot) const;

    rhi::Device* device_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::map<
        std::string,
        std::unique_ptr<render_view::RenderView>,
        std::less<>>
        views_;
};
} // namespace orbit::studio_ui
