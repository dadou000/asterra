#pragma once
#include <orbit/path_geometry/RoadJunctionConstruction.hpp>
#include <span>
#include <string>
#include <vector>

namespace orbit::path_geometry
{
// One coherent immutable replacement set: the caller must render/collide
// exclusively from these edge products and patches, never overlay raw edges.
struct RoadJunctionPatchProduct
{
    scene::ObjectId node{};
    frames::FrameId frame{};
    u64 dependencySignature{0};
    PathMesh mesh;
    std::vector<CollisionTriangle> collision;
};
struct RoadJunctionPublishedEdge
{
    scene::ObjectId edge{};
    u64 rawBuildSignature{0};
    PathDerivedProduct product;
};
struct RoadJunctionScenePacket
{
    frames::FrameId frame{};
    u64 worldGeneration{0};
    u64 buildSignature{0};
    std::vector<RoadJunctionPublishedEdge> edges;
    std::vector<RoadJunctionPatchProduct> patches;
    std::vector<JunctionMovement> connections;
    [[nodiscard]] bool Valid() const noexcept {
        return frame && worldGeneration && buildSignature &&
            !edges.empty() && !patches.empty();
    }
};
enum class RoadJunctionPublicationIssue : u8
{
    InvalidInput, IncompleteConstruction, MissingSource, DirtySource,
    ChangedSource, MixedFrames, SharedEdge, InvalidCollision,
    InvalidNavigation, BudgetExceeded
};
struct RoadJunctionPublicationFinding
{
    RoadJunctionPublicationIssue issue{RoadJunctionPublicationIssue::InvalidInput};
    scene::ObjectId object{};
    std::string message;
};
struct RoadJunctionPublicationResult
{
    RoadJunctionScenePacket packet;
    std::vector<RoadJunctionPublicationFinding> findings;
    [[nodiscard]] bool Ready() const noexcept {
        return findings.empty() && packet.Valid();
    }
};
// Builds a complete candidate locally; never mutates input caches.
// Current slice disallows two junction patches trimming the SAME edge.
// Therefore either all participating roads are replaced or none are.
[[nodiscard]] RoadJunctionPublicationResult AssembleRoadJunctionScene(
    frames::FrameId frame,
    u64 worldGeneration,
    std::span<const RoadJunctionConstruction> junctions,
    std::span<const PathDerivedProduct* const> rawEdges,
    std::span<const scene::ObjectId> dirtyEdges = {},
    std::size_t maximumVertices = 262144U);

class RoadJunctionScenePublisher
{
public:
    // Transaction: failure retains the last immutable packet. Caller must
    // separately fence it against changes to the owning world/source graph.
    [[nodiscard]] bool Publish(RoadJunctionPublicationResult candidate);
    [[nodiscard]] const RoadJunctionScenePacket* Snapshot() const noexcept;
    [[nodiscard]] const std::vector<RoadJunctionPublicationFinding>& Findings() const noexcept;
    void Clear() noexcept;
private:
    RoadJunctionScenePacket snapshot_;
    std::vector<RoadJunctionPublicationFinding> findings_;
};
} // namespace orbit::path_geometry
