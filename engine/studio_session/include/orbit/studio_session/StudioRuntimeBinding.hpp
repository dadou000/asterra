#pragma once

#include <orbit/editor_session/ActiveBodyModel.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <optional>

namespace orbit::studio_session
{
class StudioSession;

// Immutable per-frame identity/provenance snapshot. It deliberately owns no
// pointers into a world session or universe composition.
struct StudioRuntimeSnapshot
{
    bool hasWorld{false};
    u64 worldGeneration{0};
    u64 universeGeneration{0};
    std::optional<editor_session::ActiveBodyTarget> activeBody;

    bool compositionChanged{false};
    bool activeBodyChanged{false};
    bool viewportTargetsChanged{false};
    bool pathNetworkRebound{false};
    bool pathRoutingRebound{false};
    bool pathProductsInvalidated{false};
    u32 pluginsReloaded{0};
};

// Safe application seam between OrbitStudio's frame loop and StudioSession.
// Refresh() advances all world/universe-derived services in dependency order,
// then returns only value provenance. Registry/service access requires that
// exact snapshot to still match the current world + universe generations.
class StudioRuntimeBinding
{
public:
    explicit StudioRuntimeBinding(
        StudioSession& session) noexcept;

    [[nodiscard]] StudioRuntimeSnapshot Refresh();
    [[nodiscard]] StudioRuntimeSnapshot Capture() const;

    [[nodiscard]] bool IsCurrent(
        const StudioRuntimeSnapshot& snapshot) const noexcept;

    [[nodiscard]] const frames::FrameGraph& Frames(
        const StudioRuntimeSnapshot& snapshot) const;

    [[nodiscard]] const universe::BodyRegistry& Bodies(
        const StudioRuntimeSnapshot& snapshot) const;

    // Returns a value copy so render/UI code cannot retain a pointer into a
    // BodyRegistry that may be replaced by the next semantic edit.
    [[nodiscard]] std::optional<universe::CelestialBody>
    ActiveBodyRecord(
        const StudioRuntimeSnapshot& snapshot) const;

    [[nodiscard]] path_routing::RoutePlanner& Routes(
        const StudioRuntimeSnapshot& snapshot);

    [[nodiscard]] UniverseBoundPathCache& PathProducts(
        const StudioRuntimeSnapshot& snapshot);

    [[nodiscard]] const UniverseBoundPathCache& PathProducts(
        const StudioRuntimeSnapshot& snapshot) const;

private:
    void RequireCurrent(
        const StudioRuntimeSnapshot& snapshot) const;

    StudioSession* session_{nullptr};
};
} // namespace orbit::studio_session
