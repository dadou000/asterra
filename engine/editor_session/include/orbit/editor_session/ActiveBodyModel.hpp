#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <optional>
#include <string>

namespace orbit::editor_session
{
struct ActiveBodyTarget
{
    scene::ObjectId semanticObject{};
    universe::BodyId body{};
    frames::FrameId frame{};
    std::string name;
    f64 referenceRadiusMeters{0.0};
    u64 sessionGeneration{0};
    u64 universeGeneration{0};
    u64 sourceRevision{0};
};

// Resolves Studio body focus from authoritative semantic selection into the
// current UniverseComposition. No runtime body identity is persisted here:
// targets are reconstructed whenever the world session generation or semantic
// revision changes.
class ActiveBodyModel
{
public:
    explicit ActiveBodyModel(
        EditorWorldSession& session) noexcept;

    // Refreshes UniverseComposition if semantic authority changed, then
    // follows the first selected object that belongs to a celestial body.
    // If selection is unrelated, the previous valid target is retained. A
    // fresh world session falls back to the first composed body.
    [[nodiscard]] bool Refresh();

    // Resolves an object (or one of its descendants) to the nearest composed
    // celestial body without changing the shared active body.
    [[nodiscard]] std::optional<ActiveBodyTarget>
    Resolve(scene::ObjectId object);

    // Explicit shared viewport/body focus. Descendants resolve to their
    // nearest celestial-body ancestor. Throws when no composed body exists.
    void Focus(scene::ObjectId object);

    void Clear() noexcept;

    [[nodiscard]] const std::optional<ActiveBodyTarget>&
    Active() const noexcept;

private:
    [[nodiscard]] std::optional<scene::ObjectId>
    ResolveBodyObject(scene::ObjectId object) const;

    [[nodiscard]] std::optional<scene::ObjectId>
    FirstBodyObject() const;

    [[nodiscard]] ActiveBodyTarget
    BuildTarget(scene::ObjectId object) const;

    EditorWorldSession* session_{nullptr};
    std::optional<ActiveBodyTarget> active_;
    u64 observedGeneration_{~u64{0}};
};
} // namespace orbit::editor_session
