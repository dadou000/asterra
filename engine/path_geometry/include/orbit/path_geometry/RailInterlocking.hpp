#pragma once
#include <orbit/scene/ObjectStore.hpp>
#include <map>
#include <string>

namespace orbit::path_geometry
{
// Independent rail route authority prototype. Switch state is NOT derived
// from a drawn Bézier intersection, and must eventually persist with its
// authored turnout object and signalling network.
enum class SwitchLeg : u8 { Straight, Diverging };
struct RailSwitchDefinition
{
    scene::ObjectId node{}, stem{}, straight{}, diverging{};
};
enum class RailLockResult : u8
{
    Granted, MissingSwitch, InvalidDefinition, InvalidRoute,
    Busy, Occupied, WrongOwner
};
struct RailSwitchState
{
    RailSwitchDefinition definition{};
    SwitchLeg aligned{SwitchLeg::Straight};
    scene::ObjectId owner{}; // stable route reservation ID; zero means unlocked
    bool occupied{false};
};
class RailInterlocking
{
public:
    [[nodiscard]] RailLockResult Define(RailSwitchDefinition definition);
    [[nodiscard]] RailLockResult Reserve(scene::ObjectId node,
        scene::ObjectId route, scene::ObjectId from, scene::ObjectId to);
    [[nodiscard]] RailLockResult Release(scene::ObjectId node,
        scene::ObjectId route);
    [[nodiscard]] RailLockResult SetOccupied(scene::ObjectId node, bool occupied);
    [[nodiscard]] RailLockResult SetAlignment(scene::ObjectId node,
        SwitchLeg leg);
    [[nodiscard]] bool MayTraverse(scene::ObjectId node,
        scene::ObjectId from, scene::ObjectId to,
        scene::ObjectId route) const noexcept;
    [[nodiscard]] const RailSwitchState* Find(scene::ObjectId node) const noexcept;
    void Clear() noexcept { switches_.clear(); }
private:
    std::map<scene::ObjectId,RailSwitchState> switches_;
};
} // namespace orbit::path_geometry
