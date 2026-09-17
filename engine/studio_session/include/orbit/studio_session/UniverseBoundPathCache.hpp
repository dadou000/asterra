#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace orbit::studio_session
{
// Derived path geometry is cache-only data expressed in a composed runtime
// frame. UniverseComposition may replace the complete FrameGraph while the
// semantic PathEdge remains unchanged, so all frame-bound products must be
// discarded when EditorWorldSession::UniverseGeneration changes.
class UniverseBoundPathCache
{
public:
    explicit UniverseBoundPathCache(
        editor_session::EditorWorldSession& world) noexcept;

    [[nodiscard]] bool RefreshBinding();

    void Store(
        path_geometry::PathDerivedProduct product,
        std::optional<u64> routeGeneration = std::nullopt);

    [[nodiscard]] const path_geometry::PathDerivedProduct*
    Find(scene::ObjectId edge) const noexcept;

    [[nodiscard]] std::optional<u64>
    RouteGeneration(scene::ObjectId edge) const noexcept;

    [[nodiscard]] bool Erase(scene::ObjectId edge) noexcept;
    void Clear() noexcept;

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;

    [[nodiscard]] std::vector<const path_geometry::PathDerivedProduct*>
    Products() const;

    [[nodiscard]] u64 ObservedUniverseGeneration() const noexcept;
    [[nodiscard]] u64 BindingGeneration() const noexcept;

private:
    editor_session::EditorWorldSession* world_{nullptr};
    std::unordered_map<
        scene::ObjectId,
        path_geometry::PathDerivedProduct>
        products_;
    std::unordered_map<scene::ObjectId, u64>
        routeGenerations_;
    u64 observedUniverseGeneration_{~u64{0}};
    u64 bindingGeneration_{0};
};
} // namespace orbit::studio_session
