#include <orbit/paths/PathNetwork.hpp>

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orbit::paths
{
namespace
{
[[nodiscard]] NetworkId ToNetworkId(
    const scene::ObjectId object) noexcept
{
    return {
        .high = object.high,
        .low = object.low
    };
}

[[nodiscard]] scene::ObjectId ToObjectId(
    const NetworkId network) noexcept
{
    return {
        .high = network.high,
        .low = network.low
    };
}

[[nodiscard]] schema::ObjectReferenceValue ToReference(
    const scene::ObjectId object) noexcept
{
    return {
        .high = object.high,
        .low = object.low
    };
}

[[nodiscard]] scene::ObjectId FromReference(
    const schema::ObjectReferenceValue value) noexcept
{
    return {
        .high = value.high,
        .low = value.low
    };
}

template <typename T>
[[nodiscard]] T PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    T fallback)
{
    const auto stored =
        objects.GetProperty(
            object,
            property);

    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* value =
        std::get_if<T>(
            &*stored);

    if (value == nullptr)
    {
        throw std::runtime_error(
            "Stored path property has an unexpected schema type.");
    }

    return *value;
}

[[nodiscard]] const char* ModeName(
    const EdgeMode mode) noexcept
{
    return mode == EdgeMode::Bezier
        ? "bezier"
        : "direct";
}

[[nodiscard]] EdgeMode ParseMode(
    const std::string& value)
{
    if (value == "direct")
    {
        return EdgeMode::Direct;
    }

    if (value == "bezier")
    {
        return EdgeMode::Bezier;
    }

    throw std::runtime_error(
        "Stored path edge has an invalid connection mode.");
}
} // namespace

void RegisterSchemas(
    schema::SchemaRegistry& schemas)
{
    schemas.RegisterType({
        .id = kPathNetworkType,
        .displayName = "Path Network",
        .category = "Paths",
        .properties = {
            {
                .id = kNetworkProfile,
                .name = "Path Profile",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            }
        }
    });

    schemas.RegisterType({
        .id = kPathNodeType,
        .displayName = "Path Node",
        .category = "Paths",
        .properties = {
            {
                .id = kAnchorKind,
                .name = "Anchor Kind",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string("frame")
            },
            {
                .id = kAnchorFrame,
                .name = "Frame",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            },
            {
                .id = kAnchorPosition,
                .name = "Local Position",
                .kind = schema::PropertyKind::Vector3,
                .unit = "m",
                .defaultValue = math::Double3{}
            },
            {
                .id = kAnchorBody,
                .name = "Surface Body",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            },
            {
                .id = kAnchorSurface,
                .name = "Surface Coordinate",
                .kind = schema::PropertyKind::Vector3,
                .unit = "rad,rad,m",
                .defaultValue = math::Double3{}
            },
            {
                .id = kAnchorEntity,
                .name = "Entity",
                .kind = schema::PropertyKind::ObjectReference,
                .defaultValue = schema::ObjectReferenceValue{}
            },
            {
                .id = kAnchorSocket,
                .name = "Socket",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            }
        }
    });

    schemas.RegisterType({
        .id = kPathEdgeType,
        .displayName = "Path Edge",
        .category = "Paths",
        .properties = {
            {
                .id = kEdgeStartNode,
                .name = "Start Node",
                .kind = schema::PropertyKind::ObjectReference,
                .defaultValue = schema::ObjectReferenceValue{}
            },
            {
                .id = kEdgeEndNode,
                .name = "End Node",
                .kind = schema::PropertyKind::ObjectReference,
                .defaultValue = schema::ObjectReferenceValue{}
            },
            {
                .id = kEdgeMode,
                .name = "Connection Mode",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string("direct")
            },
            {
                .id = kBezierStartHandle,
                .name = "Start Handle",
                .kind = schema::PropertyKind::Vector3,
                .unit = "m",
                .defaultValue = math::Double3{}
            },
            {
                .id = kBezierEndHandle,
                .name = "End Handle",
                .kind = schema::PropertyKind::Vector3,
                .unit = "m",
                .defaultValue = math::Double3{}
            },
            {
                .id = kEdgeProfileOverride,
                .name = "Profile Override",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            }
        }
    });
}

PathNetworkService::PathNetworkService(
    scene::ObjectStore& objects,
    commands::CommandService& commands)
    : objects_(objects),
      commands_(commands)
{
}

PathNetworkRecord PathNetworkService::CreateNetwork(
    std::string name,
    const std::optional<scene::ObjectId> parent,
    std::string profileAsset)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Path network name must not be empty.");
    }

    commands_.BeginTransaction(
        "Create Path Network");

    try
    {
        const scene::ObjectId object =
            commands_.CreateObject(
                kPathNetworkType,
                name,
                parent);

        commands_.SetProperty(
            object,
            kNetworkProfile,
            profileAsset);

        commands_.CommitTransaction();

        return {
            .id = ToNetworkId(object),
            .object = object,
            .name = std::move(name),
            .profileAsset =
                std::move(profileAsset)
        };
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}

PathNodeRecord PathNetworkService::CreateNode(
    const NetworkId network,
    std::string name,
    PathAnchor anchor)
{
    const scene::ObjectId networkObject =
        NetworkObject(network);

    if (name.empty())
    {
        throw std::invalid_argument(
            "Path node name must not be empty.");
    }

    commands_.BeginTransaction(
        "Create Path Node");

    try
    {
        const scene::ObjectId node =
            commands_.CreateObject(
                kPathNodeType,
                name,
                networkObject);

        WriteAnchor(
            node,
            anchor);

        commands_.CommitTransaction();

        return {
            .id = node,
            .network = network,
            .name = std::move(name),
            .anchor = std::move(anchor)
        };
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}

PathEdgeRecord PathNetworkService::ConnectDirect(
    const scene::ObjectId startNode,
    const scene::ObjectId endNode,
    std::string name)
{
    if (startNode == endNode)
    {
        throw std::invalid_argument(
            "A path edge requires two different nodes.");
    }

    const NetworkId network =
        RequireNodeNetwork(startNode);

    if (RequireNodeNetwork(endNode) != network)
    {
        throw std::invalid_argument(
            "Path edge endpoints must belong to the same network.");
    }

    commands_.BeginTransaction(
        "Connect Path Nodes");

    try
    {
        const scene::ObjectId edge =
            commands_.CreateObject(
                kPathEdgeType,
                std::move(name),
                NetworkObject(network));

        commands_.SetProperty(
            edge,
            kEdgeStartNode,
            ToReference(startNode));
        commands_.SetProperty(
            edge,
            kEdgeEndNode,
            ToReference(endNode));
        commands_.SetProperty(
            edge,
            kEdgeMode,
            std::string("direct"));
        commands_.SetProperty(
            edge,
            kBezierStartHandle,
            math::Double3{});
        commands_.SetProperty(
            edge,
            kBezierEndHandle,
            math::Double3{});
        commands_.SetProperty(
            edge,
            kEdgeProfileOverride,
            std::string{});

        commands_.CommitTransaction();

        return *FindEdge(edge);
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}

PathEdgeRecord PathNetworkService::ConnectBezier(
    const scene::ObjectId startNode,
    const scene::ObjectId endNode,
    const math::Double3 startHandleMeters,
    const math::Double3 endHandleMeters,
    std::string name)
{
    if (startNode == endNode)
    {
        throw std::invalid_argument(
            "A path edge requires two different nodes.");
    }

    const NetworkId network =
        RequireNodeNetwork(startNode);

    if (RequireNodeNetwork(endNode) != network)
    {
        throw std::invalid_argument(
            "Path edge endpoints must belong to the same network.");
    }

    commands_.BeginTransaction(
        "Connect Path Nodes Bezier");

    try
    {
        const scene::ObjectId edge =
            commands_.CreateObject(
                kPathEdgeType,
                std::move(name),
                NetworkObject(network));

        commands_.SetProperty(
            edge,
            kEdgeStartNode,
            ToReference(startNode));
        commands_.SetProperty(
            edge,
            kEdgeEndNode,
            ToReference(endNode));
        commands_.SetProperty(
            edge,
            kEdgeMode,
            std::string("bezier"));
        commands_.SetProperty(
            edge,
            kBezierStartHandle,
            startHandleMeters);
        commands_.SetProperty(
            edge,
            kBezierEndHandle,
            endHandleMeters);
        commands_.SetProperty(
            edge,
            kEdgeProfileOverride,
            std::string{});

        commands_.CommitTransaction();

        return *FindEdge(edge);
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}

void PathNetworkService::SetNodeAnchor(
    const scene::ObjectId node,
    PathAnchor anchor)
{
    static_cast<void>(
        RequireNodeNetwork(node));

    commands_.BeginTransaction(
        "Set Path Anchor");

    try
    {
        WriteAnchor(
            node,
            anchor);
        commands_.CommitTransaction();
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}

void PathNetworkService::SetBezierHandles(
    const scene::ObjectId edge,
    const math::Double3 startHandleMeters,
    const math::Double3 endHandleMeters)
{
    const auto record =
        FindEdge(edge);

    if (!record.has_value() ||
        record->mode != EdgeMode::Bezier)
    {
        throw std::invalid_argument(
            "Bezier handles require a Bezier path edge.");
    }

    commands_.BeginTransaction(
        "Edit Bezier Handles");

    try
    {
        commands_.SetProperty(
            edge,
            kBezierStartHandle,
            startHandleMeters);
        commands_.SetProperty(
            edge,
            kBezierEndHandle,
            endHandleMeters);
        commands_.CommitTransaction();
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}

std::optional<PathNetworkRecord>
PathNetworkService::FindNetwork(
    const NetworkId network) const
{
    const scene::ObjectId object =
        ToObjectId(network);
    const auto stored =
        objects_.Find(object);

    if (!stored.has_value() ||
        stored->type != kPathNetworkType)
    {
        return std::nullopt;
    }

    return PathNetworkRecord{
        .id = network,
        .object = object,
        .name = stored->name,
        .profileAsset =
            PropertyOr<std::string>(
                objects_,
                object,
                kNetworkProfile,
                {})
    };
}

std::optional<PathNodeRecord>
PathNetworkService::FindNode(
    const scene::ObjectId node) const
{
    const auto stored =
        objects_.Find(node);

    if (!stored.has_value() ||
        stored->type != kPathNodeType ||
        !stored->parent.has_value())
    {
        return std::nullopt;
    }

    const NetworkId network =
        ToNetworkId(*stored->parent);

    if (!FindNetwork(network).has_value())
    {
        return std::nullopt;
    }

    const std::string kind =
        PropertyOr<std::string>(
            objects_,
            node,
            kAnchorKind,
            "frame");

    PathAnchor anchor;

    if (kind == "frame")
    {
        const auto frame =
            frames::FrameId::Parse(
                PropertyOr<std::string>(
                    objects_, node,
                    kAnchorFrame, {}));

        if (!frame.has_value())
        {
            throw std::runtime_error(
                "Path node contains an invalid frame anchor.");
        }

        anchor = FramePointAnchor{
            .frame = *frame,
            .localMeters =
                PropertyOr<math::Double3>(
                    objects_, node,
                    kAnchorPosition, {})
        };
    }
    else if (kind == "surface")
    {
        const auto body =
            universe::BodyId::Parse(
                PropertyOr<std::string>(
                    objects_, node,
                    kAnchorBody, {}));

        if (!body.has_value())
        {
            throw std::runtime_error(
                "Path node contains an invalid surface anchor.");
        }

        anchor = SurfaceAnchor{
            .body = *body,
            .coordinate =
                PropertyOr<math::Double3>(
                    objects_, node,
                    kAnchorSurface, {})
        };
    }
    else if (kind == "entity_socket")
    {
        const auto reference =
            PropertyOr<schema::ObjectReferenceValue>(
                objects_, node,
                kAnchorEntity, {});
        const scene::ObjectId entity =
            FromReference(reference);

        if (!entity)
        {
            throw std::runtime_error(
                "Path node contains an invalid entity anchor.");
        }

        anchor = EntitySocketAnchor{
            .entity = entity,
            .socket =
                PropertyOr<std::string>(
                    objects_, node,
                    kAnchorSocket, {}),
            .localMeters =
                PropertyOr<math::Double3>(
                    objects_, node,
                    kAnchorPosition, {})
        };
    }
    else
    {
        throw std::runtime_error(
            "Path node contains an unknown anchor kind.");
    }

    return PathNodeRecord{
        .id = node,
        .network = network,
        .name = stored->name,
        .anchor = std::move(anchor)
    };
}

std::optional<PathEdgeRecord>
PathNetworkService::FindEdge(
    const scene::ObjectId edge) const
{
    const auto stored =
        objects_.Find(edge);

    if (!stored.has_value() ||
        stored->type != kPathEdgeType ||
        !stored->parent.has_value())
    {
        return std::nullopt;
    }

    const NetworkId network =
        ToNetworkId(*stored->parent);

    if (!FindNetwork(network).has_value())
    {
        return std::nullopt;
    }

    const scene::ObjectId start =
        FromReference(
            PropertyOr<schema::ObjectReferenceValue>(
                objects_, edge,
                kEdgeStartNode, {}));
    const scene::ObjectId end =
        FromReference(
            PropertyOr<schema::ObjectReferenceValue>(
                objects_, edge,
                kEdgeEndNode, {}));

    if (!start || !end)
    {
        throw std::runtime_error(
            "Path edge contains invalid endpoint references.");
    }

    return PathEdgeRecord{
        .id = edge,
        .network = network,
        .startNode = start,
        .endNode = end,
        .mode = ParseMode(
            PropertyOr<std::string>(
                objects_, edge,
                kEdgeMode,
                "direct")),
        .startHandleMeters =
            PropertyOr<math::Double3>(
                objects_, edge,
                kBezierStartHandle, {}),
        .endHandleMeters =
            PropertyOr<math::Double3>(
                objects_, edge,
                kBezierEndHandle, {}),
        .profileOverride =
            PropertyOr<std::string>(
                objects_, edge,
                kEdgeProfileOverride, {})
    };
}

scene::ObjectId PathNetworkService::NetworkObject(
    const NetworkId network) const
{
    const scene::ObjectId object =
        ToObjectId(network);
    const auto stored =
        objects_.Find(object);

    if (!stored.has_value() ||
        stored->type != kPathNetworkType)
    {
        throw std::invalid_argument(
            "Unknown path network.");
    }

    return object;
}

NetworkId PathNetworkService::RequireNodeNetwork(
    const scene::ObjectId node) const
{
    const auto stored =
        objects_.Find(node);

    if (!stored.has_value() ||
        stored->type != kPathNodeType ||
        !stored->parent.has_value())
    {
        throw std::invalid_argument(
            "Path edge endpoint is not a path node.");
    }

    const NetworkId network =
        ToNetworkId(*stored->parent);

    static_cast<void>(
        NetworkObject(network));
    return network;
}

void PathNetworkService::WriteAnchor(
    const scene::ObjectId node,
    const PathAnchor& anchor)
{
    // Write a canonical complete property set each time. Inactive anchor
    // fields are reset so changing anchor kind never leaves stale semantic
    // state behind in the authoritative document.
    commands_.SetProperty(node, kAnchorFrame, std::string{});
    commands_.SetProperty(node, kAnchorPosition, math::Double3{});
    commands_.SetProperty(node, kAnchorBody, std::string{});
    commands_.SetProperty(node, kAnchorSurface, math::Double3{});
    commands_.SetProperty(node, kAnchorEntity, schema::ObjectReferenceValue{});
    commands_.SetProperty(node, kAnchorSocket, std::string{});

    std::visit(
        [this, node](const auto& value)
        {
            using Anchor = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<Anchor, FramePointAnchor>)
            {
                if (!value.frame)
                {
                    throw std::invalid_argument(
                        "Frame path anchor requires a valid FrameId.");
                }

                commands_.SetProperty(node, kAnchorKind, std::string("frame"));
                commands_.SetProperty(node, kAnchorFrame, value.frame.ToString());
                commands_.SetProperty(node, kAnchorPosition, value.localMeters);
            }
            else if constexpr (std::is_same_v<Anchor, SurfaceAnchor>)
            {
                if (!value.body)
                {
                    throw std::invalid_argument(
                        "Surface path anchor requires a valid BodyId.");
                }

                commands_.SetProperty(node, kAnchorKind, std::string("surface"));
                commands_.SetProperty(node, kAnchorBody, value.body.ToString());
                commands_.SetProperty(node, kAnchorSurface, value.coordinate);
            }
            else
            {
                if (!value.entity)
                {
                    throw std::invalid_argument(
                        "Entity path anchor requires a valid ObjectId.");
                }

                commands_.SetProperty(node, kAnchorKind, std::string("entity_socket"));
                commands_.SetProperty(node, kAnchorEntity, ToReference(value.entity));
                commands_.SetProperty(node, kAnchorSocket, value.socket);
                commands_.SetProperty(node, kAnchorPosition, value.localMeters);
            }
        },
        anchor);
}
} // namespace orbit::paths
