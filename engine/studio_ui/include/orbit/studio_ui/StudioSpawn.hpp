#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
// One Spawn Location object (world_model::kSpawnLocationType) as the camera
// uses it. The position is body-local; the camera lands on the terrain below
// it (the same vantage a double-click gives) and then takes yaw/pitch.
struct StudioSpawnLocation
{
    scene::ObjectId id{};
    scene::ObjectId body{};
    std::string name;
    bool enabled{true};
    math::Double3 positionMeters{};
    // Camera height above the terrain under the position; the terrain
    // navigation keeps its own ground clearance as a floor.
    f64 eyeHeightMeters{2.0};
    f64 yawDegrees{0.0};
    f64 pitchDegrees{0.0};
};

// Every Spawn Location in the world, depth-first in sort order, enabled or
// not. The first enabled one is the project's spawn.
[[nodiscard]] std::vector<StudioSpawnLocation> ListSpawnLocations(
    const scene::ObjectStore& objects);

[[nodiscard]] std::optional<StudioSpawnLocation> FindProjectSpawn(
    const scene::ObjectStore& objects);

// Puts the camera of `viewId` on the spawn. A view looking at another body is
// first focused on the spawn's body (selecting it makes it the shared active body,
// like pressing Focus); returns false until that body's terrain runtime is
// current so a caller can retry on the next frame. Never edits the world.
[[nodiscard]] bool GoToSpawn(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    std::string_view viewId,
    const StudioSpawnLocation& spawn);

// spawn.list / spawn.go / spawn.set_here (docs/ORBIT_MCP.md). `session` and
// `views` must outlive the dispatcher.
void RegisterStudioSpawnRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session);
} // namespace orbit::studio_ui
