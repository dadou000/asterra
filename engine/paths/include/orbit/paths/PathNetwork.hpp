#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/core/StrongId.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <optional>
#include <string>
#include <variant>

namespace orbit::paths
{
struct NetworkIdTag;
using NetworkId = core::StrongId<NetworkIdTag>;

inline constexpr schema::TypeId kPathNetworkType{
    .high = 0x4f52424954504154ULL,
    .low = 0x484e4554574f524bULL
};
inline constexpr schema::TypeId kPathNodeType{
    .high = 0x4f52424954504154ULL,
    .low = 0x484e4f4445000001ULL
};
inline constexpr schema::TypeId kPathEdgeType{
    .high = 0x4f52424954504154ULL,
    .low = 0x4845444745000001ULL
};

inline constexpr schema::PropertyId kNetworkProfile{
    .high = 0x4f52424954504154ULL,
    .low = 0x4850524f46494c45ULL
};
inline constexpr schema::PropertyId kAnchorKind{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e43484b494eULL
};
inline constexpr schema::PropertyId kAnchorFrame{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e434652414dULL
};
inline constexpr schema::PropertyId kAnchorPosition{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e43504f5349ULL
};
inline constexpr schema::PropertyId kAnchorBody{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e43424f4459ULL
};
inline constexpr schema::PropertyId kAnchorSurface{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e4353555246ULL
};
inline constexpr schema::PropertyId kAnchorEntity{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e43454e5449ULL
};
inline constexpr schema::PropertyId kAnchorSocket{
    .high = 0x4f52424954504154ULL,
    .low = 0x48414e43534f434bULL
};
inline constexpr schema::PropertyId kEdgeStartNode{
    .high = 0x4f52424954504154ULL,
    .low = 0x4845444753544152ULL
};
inline constexpr schema::PropertyId kEdgeEndNode{
    .high = 0x4f52424954504154ULL,
    .low = 0x48454447454e4401ULL
};
inline constexpr schema::PropertyId kEdgeMode{
    .high = 0x4f52424954504154ULL,
    .low = 0x484544474d4f4445ULL
};
inline constexpr schema::PropertyId kBezierStartHandle{
    .high = 0x4f52424954504154ULL,
    .low = 0x4842455a53544152ULL
};
inline constexpr schema::PropertyId kBezierEndHandle{
    .high = 0x4f52424954504154ULL,
    .low = 0x4842455a454e4401ULL
};
inline constexpr schema::PropertyId kEdgeProfileOverride{
    .high = 0x4f52424954504154ULL,
    .low = 0x4845444750524f46ULL
};

struct FramePointAnchor
{
    frames::FrameId frame{};
    math::Double3 localMeters{};

    [[nodiscard]] bool operator==(
        const FramePointAnchor&) const noexcept = default;
};

// x = latitude radians, y = longitude radians, z = offset metres.
struct SurfaceAnchor
{
    universe::BodyId body{};
    math::Double3 coordinate{};

    [[nodiscard]] bool operator==(
        const SurfaceAnchor&) const noexcept = default;
};

struct EntitySocketAnchor
{
    scene::ObjectId entity{};
    std::string socket;
    math::Double3 localMeters{};

    [[nodiscard]] bool operator==(
        const EntitySocketAnchor&) const noexcept = default;
};

using PathAnchor = std::variant<
    FramePointAnchor,
    SurfaceAnchor,
    EntitySocketAnchor>;

enum class EdgeMode : u8
{
    Direct,
    Bezier
};

struct PathNetworkRecord
{
    NetworkId id{};
    scene::ObjectId object{};
    std::string name;
    std::string profileAsset;
};

struct PathNodeRecord
{
    scene::ObjectId id{};
    NetworkId network{};
    std::string name;
    PathAnchor anchor;
};

struct PathEdgeRecord
{
    scene::ObjectId id{};
    NetworkId network{};
    scene::ObjectId startNode{};
    scene::ObjectId endNode{};
    EdgeMode mode{EdgeMode::Direct};
    math::Double3 startHandleMeters{};
    math::Double3 endHandleMeters{};
    std::string profileOverride;
};

void RegisterSchemas(schema::SchemaRegistry& schemas);

class PathNetworkService
{
public:
    PathNetworkService(
        scene::ObjectStore& objects,
        commands::CommandService& commands);

    [[nodiscard]] PathNetworkRecord CreateNetwork(
        std::string name,
        std::optional<scene::ObjectId> parent = std::nullopt,
        std::string profileAsset = {});

    [[nodiscard]] PathNodeRecord CreateNode(
        NetworkId network,
        std::string name,
        PathAnchor anchor);

    [[nodiscard]] PathEdgeRecord ConnectDirect(
        scene::ObjectId startNode,
        scene::ObjectId endNode,
        std::string name = "Direct Edge");

    [[nodiscard]] PathEdgeRecord ConnectBezier(
        scene::ObjectId startNode,
        scene::ObjectId endNode,
        math::Double3 startHandleMeters,
        math::Double3 endHandleMeters,
        std::string name = "Bezier Edge");

    void SetNodeAnchor(
        scene::ObjectId node,
        PathAnchor anchor);

    void SetBezierHandles(
        scene::ObjectId edge,
        math::Double3 startHandleMeters,
        math::Double3 endHandleMeters);

    void SetProfile(
        scene::ObjectId pathObject,
        std::string profileAsset);

    [[nodiscard]] std::optional<PathNetworkRecord>
    FindNetwork(NetworkId network) const;

    [[nodiscard]] std::optional<PathNodeRecord>
    FindNode(scene::ObjectId node) const;

    [[nodiscard]] std::optional<PathEdgeRecord>
    FindEdge(scene::ObjectId edge) const;

private:
    [[nodiscard]] scene::ObjectId NetworkObject(
        NetworkId network) const;

    [[nodiscard]] NetworkId RequireNodeNetwork(
        scene::ObjectId node) const;

    void WriteAnchor(
        scene::ObjectId node,
        const PathAnchor& anchor);

    scene::ObjectStore& objects_;
    commands::CommandService& commands_;
};
} // namespace orbit::paths
