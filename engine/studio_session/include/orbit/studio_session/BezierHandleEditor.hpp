#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/WorldBoundPathNetwork.hpp>

#include <optional>

namespace orbit::studio_session
{
enum class BezierHandle : u8
{
    Start,
    End
};

// Transactional edit session for one persistent Bezier edge handle. A whole
// viewport drag is committed as one undoable command transaction; intermediate
// drag samples never become independent undo entries.
class BezierHandleEditor
{
public:
    BezierHandleEditor(
        editor_session::EditorWorldSession& world,
        WorldBoundPathNetwork& paths) noexcept;
    ~BezierHandleEditor();

    BezierHandleEditor(
        const BezierHandleEditor&) = delete;
    BezierHandleEditor& operator=(
        const BezierHandleEditor&) = delete;

    void Begin(
        scene::ObjectId edge,
        BezierHandle handle);
    void Update(math::Double3 handleMeters);
    void Commit();
    void Cancel() noexcept;

    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] std::optional<scene::ObjectId> Edge() const noexcept;
    [[nodiscard]] std::optional<BezierHandle> Handle() const noexcept;
    [[nodiscard]] math::Double3 StartHandle() const noexcept;
    [[nodiscard]] math::Double3 EndHandle() const noexcept;

private:
    void Clear() noexcept;

    editor_session::EditorWorldSession* world_{nullptr};
    WorldBoundPathNetwork* paths_{nullptr};
    std::optional<scene::ObjectId> edge_;
    std::optional<BezierHandle> handle_;
    math::Double3 startHandle_{};
    math::Double3 endHandle_{};
    u64 worldGeneration_{0};
    bool ownsTransaction_{false};
};
} // namespace orbit::studio_session
