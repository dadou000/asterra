#pragma once
#include <orbit/path_geometry/RoadVehicleFixture.hpp>
#include <orbit/path_geometry/RoadJunctionPublication.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orbit::path_geometry
{
// Read-only route derived from one EXPLICIT authored junction movement.
// No proximity connections, lane changes, traffic signs or pathfinder guesses.
struct RoadVehicleRoute
{
    frames::FrameId frame{};
    u64 packetSignature{0};
    scene::ObjectId incomingEdge{},outgoingEdge{};
    std::vector<math::Double3> waypoints;
    [[nodiscard]] bool Valid() const noexcept
    {
        return frame && packetSignature && incomingEdge && outgoingEdge &&
            waypoints.size()>=3U;
    }
};
[[nodiscard]] std::optional<RoadVehicleRoute> CompileRoadVehicleRoute(
    const RoadJunctionScenePacket& packet,
    scene::ObjectId fromEdge,scene::ObjectId toEdge,
    u32 incomingLane = 0U,u32 outgoingLane = 0U);

struct RoadVehicleRuntimeActor
{
    scene::ObjectId id{};
    RoadVehicleState state{};
    RoadVehicleParameters parameters{};
    RoadVehicleInput controls{};
    RoadVehicleRoute route{};
    bool followRoute{false};
    u32 nextWaypoint{0};
    f64 targetSpeedMetersPerSecond{5.0};
    RoadVehicleStepReport lastContact{};
};
struct RoadVehicleWorldStep
{
    u32 completedSteps{0};
    u32 actorCount{0};
    u64 tick{0};
    u64 packetSignature{0};
    bool success{false};
    std::string error;
};

// Persistent runtime-only actors. Explicit ID (not an authored scene object).
// A frame/packet revision mismatch is an error until the caller rebinds.
// Each fixed step is ALL-OR-NOTHING for the active actor states.
class RoadVehicleRuntimeWorld
{
public:
    [[nodiscard]] bool Bind(const RoadContactSceneOwner& contact);
    void Clear() noexcept;
    [[nodiscard]] bool Spawn(const RoadContactSceneOwner& contact,
        scene::ObjectId id,const RoadVehicleState& initial,
        RoadVehicleParameters parameters = {});
    [[nodiscard]] bool Remove(scene::ObjectId id);
    [[nodiscard]] bool SetControls(scene::ObjectId id,RoadVehicleInput input);
    [[nodiscard]] bool FollowRoute(scene::ObjectId id,RoadVehicleRoute route,
        f64 targetSpeedMetersPerSecond=5.0);
    [[nodiscard]] bool StopFollowing(scene::ObjectId id);
    [[nodiscard]] const RoadVehicleRuntimeActor* Find(scene::ObjectId id) const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept {return actors_.size();}
    [[nodiscard]] u64 Tick() const noexcept {return tick_;}
    [[nodiscard]] frames::FrameId Frame() const noexcept {return frame_;}
    [[nodiscard]] u64 PacketSignature() const noexcept {return signature_;}
    [[nodiscard]] RoadVehicleWorldStep Step(
        const RoadContactSceneOwner& contact,u32 count=1U);
private:
    std::map<scene::ObjectId,RoadVehicleRuntimeActor> actors_;
    frames::FrameId frame_{};
    u64 worldGeneration_{0};
    u64 signature_{0};
    u64 tick_{0};
};
} // namespace orbit::path_geometry
