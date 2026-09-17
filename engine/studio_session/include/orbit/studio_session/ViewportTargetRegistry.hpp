#pragma once

#include <orbit/editor_session/ActiveBodyModel.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
enum class ViewportMode : u8
{
    Perspective,
    BodyMap,
    Debug
};

struct ViewportTargetState
{
    std::string id;
    ViewportMode mode{ViewportMode::Perspective};
    bool followActiveBody{true};
    std::optional<scene::ObjectId> pinnedSemanticObject;
    std::optional<editor_session::ActiveBodyTarget> target;
};

// Authoritative per-RenderView target state. Viewports keep independent body
// targets while sharing one semantic/runtime universe. Runtime IDs are always
// reconstructed from semantic authority after a world/session generation
// change and are never persisted as editor authority.
class ViewportTargetRegistry
{
public:
    ViewportTargetRegistry(
        editor_session::EditorWorldSession& world,
        editor_session::ActiveBodyModel& activeBody) noexcept;

    void Register(
        std::string id,
        ViewportMode mode = ViewportMode::Perspective,
        bool followActiveBody = true);

    [[nodiscard]] bool Unregister(
        std::string_view id) noexcept;

    void SetMode(
        std::string_view id,
        ViewportMode mode);

    void FollowActiveBody(
        std::string_view id);

    void PinToObject(
        std::string_view id,
        scene::ObjectId object);

    [[nodiscard]] bool Refresh();

    [[nodiscard]] const ViewportTargetState*
    Find(std::string_view id) const noexcept;

    [[nodiscard]] std::vector<ViewportTargetState>
    Catalog() const;

private:
    [[nodiscard]] ViewportTargetState&
    Require(std::string_view id);

    editor_session::EditorWorldSession* world_{nullptr};
    editor_session::ActiveBodyModel* activeBody_{nullptr};
    std::map<std::string, ViewportTargetState, std::less<>> views_;
    u64 observedGeneration_{~u64{0}};
};
} // namespace orbit::studio_session
