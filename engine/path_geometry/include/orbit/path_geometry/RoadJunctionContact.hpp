#pragma once
#include <orbit/path_geometry/RoadJunctionPublication.hpp>
#include <cstddef>

namespace orbit::path_geometry
{
struct RoadContactProbe
{
    frames::FrameId frame{};
    math::Double3 origin{};
    math::Double3 direction{0.0,-1.0,0.0};
    f64 maximumDistanceMeters{100.0};
    std::size_t maximumTriangles{100000U};
};
struct RoadContactHit
{
    bool hit{false};
    bool budgetExceeded{false};
    scene::ObjectId owner{}; // edge or semantic junction node
    bool junctionPatch{false};
    f64 distanceMeters{0.0};
    math::Double3 position{};
    math::Double3 normal{};
    std::size_t testedTriangles{0U};
};
// CPU contact diagnostic / deterministic candidate for a future native
// physics cooker. Never treats this as a registered physics shape.
// It consumes the SAME clipped edge + junction collision packet that
// Studio's Vulkan renderer draws, not untrimmed source corridors.
[[nodiscard]] RoadContactHit ProbeRoadJunctionContact(
    const RoadJunctionScenePacket& packet,
    RoadContactProbe probe);
} // namespace orbit::path_geometry
