#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/paths/PathNetwork.hpp>

#include <memory>

namespace orbit::studio_session
{
// Owns the PathNetworkService facade for exactly one open authoring world.
// PathNetworkService retains references into ObjectStore/CommandService, so it
// must be reconstructed whenever EditorWorldSession switches documents.
class WorldBoundPathNetwork
{
public:
    explicit WorldBoundPathNetwork(
        editor_session::EditorWorldSession& world) noexcept;

    WorldBoundPathNetwork(
        const WorldBoundPathNetwork&) = delete;
    WorldBoundPathNetwork& operator=(
        const WorldBoundPathNetwork&) = delete;

    // Reconstructs the path facade after a world-generation change. Returns
    // true when the binding changed, including when the active world closed.
    [[nodiscard]] bool RefreshBinding();

    [[nodiscard]] bool HasService() const noexcept;

    // Refreshes before exposing the facade and throws while no world is open.
    [[nodiscard]] paths::PathNetworkService& Service();
    [[nodiscard]] const paths::PathNetworkService& Service() const;

    [[nodiscard]] u64 ObservedWorldGeneration() const noexcept;
    [[nodiscard]] u64 BindingGeneration() const noexcept;

private:
    editor_session::EditorWorldSession* world_{nullptr};
    std::unique_ptr<paths::PathNetworkService> service_;
    u64 observedWorldGeneration_{~u64{0}};
    u64 bindingGeneration_{0};
};
} // namespace orbit::studio_session
