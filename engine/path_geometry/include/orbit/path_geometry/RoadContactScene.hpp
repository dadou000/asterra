#pragma once

#include <orbit/path_geometry/RoadJunctionContact.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace orbit::path_geometry
{
// Physics-facing static surface generation. The data is isolated from the
// authored source and can be replaced as one unit at a simulation checkpoint.
// This is NOT a registered rigid-body/vehicle collision shape.
struct RoadContactBounds
{
    math::Double3 minimum{};
    math::Double3 maximum{};
};
struct RoadContactShape
{
    scene::ObjectId owner{};
    bool junctionPatch{false};
    RoadContactBounds bounds{};
    std::vector<CollisionTriangle> triangles;
};
struct RoadContactSceneGeneration
{
    frames::FrameId frame{};
    u64 worldGeneration{0};
    u64 packetSignature{0};
    std::vector<RoadContactShape> shapes;
    std::size_t triangleCount{0};
    [[nodiscard]] bool Valid() const noexcept
    {
        return frame && worldGeneration && packetSignature &&
            !shapes.empty() && triangleCount>0;
    }
};
struct RoadContactSceneBuild
{
    RoadContactSceneGeneration generation;
    std::string error;
    [[nodiscard]] bool Ready() const noexcept
    {
        return error.empty() && generation.Valid();
    }
};

// Prepares a contact generation OFF the active simulation scene: duplicates,
// frame mismatches, mismatched collision/mesh indices and malformed or
// unbounded vertices reject the entire candidate, never individual bodies.
[[nodiscard]] RoadContactSceneBuild PrepareRoadContactScene(
    const RoadJunctionScenePacket& packet,
    std::size_t maximumShapes = 512U,
    std::size_t maximumTriangles = 250000U);

class RoadContactSceneOwner
{
public:
    // A single move activates one complete scene. Caller invokes this only
    // at the session/simulation update boundary, not mid physics integration.
    [[nodiscard]] bool Commit(RoadContactSceneBuild candidate);
    void Clear() noexcept;
    [[nodiscard]] const RoadContactSceneGeneration* Snapshot() const noexcept;
    [[nodiscard]] const std::string& LastError() const noexcept
    {
        return lastError_;
    }
    // Double-precision bounded ray contact, broadphase culls entire shapes.
    [[nodiscard]] RoadContactHit Probe(RoadContactProbe probe) const;
private:
    RoadContactSceneGeneration active_;
    std::string lastError_;
};
} // namespace orbit::path_geometry
