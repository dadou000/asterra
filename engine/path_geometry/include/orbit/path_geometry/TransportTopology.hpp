#pragma once
#include <orbit/paths/PathNetwork.hpp>
#include <span>
#include <vector>

namespace orbit::path_geometry
{
// Temporary derived graph, never a replacement for authored PathNetworkRecord.
enum class TransportMode : u8 { Road, Rail, Other };
struct TransportNode
{
    scene::ObjectId id{};
    paths::NetworkId network{};
};
struct TransportLink
{
    scene::ObjectId id{};
    paths::NetworkId network{};
    scene::ObjectId start{};
    scene::ObjectId end{};
    TransportMode mode{TransportMode::Road};
    bool bidirectional{true};
};
enum class TransportTopologyIssue : u8
{
    InvalidNode, DuplicateNode, InvalidLink, DuplicateLink,
    MissingEndpoint, NetworkMismatch, SelfLoop
};
struct TransportTopologyFinding
{
    TransportTopologyIssue issue{TransportTopologyIssue::InvalidNode};
    scene::ObjectId object{};
};
struct TransportArc
{
    scene::ObjectId link{}, from{}, to{};
    TransportMode mode{TransportMode::Road};
};
struct TransportJunction
{
    scene::ObjectId node{};
    std::vector<scene::ObjectId> incidentLinks;
    u32 roadLinks{0}, railLinks{0};
    bool explicitLevelCrossing{false};
};
struct TransportComponent
{
    TransportMode mode{TransportMode::Road};
    std::vector<scene::ObjectId> nodes, links;
};
struct TransportTopology
{
    std::vector<TransportArc> arcs;
    std::vector<TransportJunction> junctions;
    std::vector<TransportComponent> components;
    std::vector<TransportTopologyFinding> findings;
    [[nodiscard]] bool Valid() const noexcept { return findings.empty(); }
};
// Explicit stable node identity, not proximity. Modes NEVER route into each
// other, even when a level crossing creates an explicit conflict node.
[[nodiscard]] TransportTopology CompileTransportTopology(
    std::span<const TransportNode> nodes,
    std::span<const TransportLink> links);
} // namespace orbit::path_geometry
