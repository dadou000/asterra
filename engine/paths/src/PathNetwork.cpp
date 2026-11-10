#include <orbit/paths/PathNetwork.hpp>

#include <cmath>
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
    switch (mode)
    {
    case EdgeMode::Direct: return "direct";
    case EdgeMode::Bezier: return "bezier";
    case EdgeMode::Routed: return "routed";
    }

    return "direct";
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

    if (value == "routed")
    {
        return EdgeMode::Routed;
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
                .id = kInfrastructureCatalogId,
                .name = "Infrastructure Catalog",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            },
            {
                .id = kInfrastructureCatalogVersion,
                .name = "Catalog Version",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{1}
            },
            {
                .id = kInfrastructureConstruction,
                .name = "Construction Mode",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string("terrain")
            },
            {
                .id = kInfrastructureDesignSpeed,
                .name = "Design Speed",
                .kind = schema::PropertyKind::Float,
                .unit = "km/h",
                .defaultValue = f64{0.0},
                .range = {.minimum = 0.0}
            },
            {
                .id = kInfrastructureEaseTurns,
                .name = "Ease Turns",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = true
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

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Create Path Network");
    }

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

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

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
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
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

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Create Path Node");
    }

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

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

        return {
            .id = node,
            .network = network,
            .name = std::move(name),
            .anchor = std::move(anchor)
        };
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
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

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Connect Path Nodes");
    }

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

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

        return *FindEdge(edge);
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
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

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Connect Path Nodes Bezier");
    }

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

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

        return *FindEdge(edge);
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

PathEdgeRecord PathNetworkService::ConnectRouted(
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

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Connect Path Nodes Routed");
    }

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
            std::string("routed"));
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

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

        return *FindEdge(edge);
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

void PathNetworkService::SetNodeAnchor(
    const scene::ObjectId node,
    PathAnchor anchor)
{
    static_cast<void>(
        RequireNodeNetwork(node));

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Set Path Anchor");
    }

    try
    {
        WriteAnchor(
            node,
            anchor);
        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

PathSplitResult PathNetworkService::SplitEdge(
    const scene::ObjectId edgeId, const f64 fraction,
    std::string nodeName)
{
    if (!std::isfinite(fraction) || fraction <= 0.001 ||
        fraction >= 0.999 || nodeName.empty())
        throw std::invalid_argument("Split fraction must lie strictly inside the edge and node name must be set.");
    const auto old=FindEdge(edgeId);
    if(!old || old->mode==EdgeMode::Routed)
        throw std::invalid_argument("Only existing direct or Bezier edges can be split.");
    const auto start=FindNode(old->startNode),end=FindNode(old->endNode);
    if(!start || !end || start->network!=old->network ||
       end->network!=old->network)
        throw std::invalid_argument("Split edge endpoints must resolve in the same network.");
    const auto* a=std::get_if<FramePointAnchor>(&start->anchor);
    const auto* b=std::get_if<FramePointAnchor>(&end->anchor);
    if(!a || !b || !a->frame || a->frame!=b->frame)
        throw std::invalid_argument("Split requires endpoint FramePoint anchors in one reference frame.");
    const auto finite=[](const math::Double3 v) {
        return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
    };
    if(!finite(a->localMeters) || !finite(b->localMeters) ||
       !finite(old->startHandleMeters)||!finite(old->endHandleMeters) ||
       math::LengthSquared(b->localMeters-a->localMeters)<1.e-12)
        throw std::invalid_argument("Split requires finite nondegenerate geometry.");

    const auto mix=[&](const math::Double3 x,const math::Double3 y) {
        return x*(1.0-fraction)+y*fraction;
    };
    const auto p0=a->localMeters,p3=b->localMeters;
    const auto p1=old->mode==EdgeMode::Bezier?p0+old->startHandleMeters:p0+(p3-p0)*(1.0/3.0);
    const auto p2=old->mode==EdgeMode::Bezier?p3+old->endHandleMeters:p0+(p3-p0)*(2.0/3.0);
    const auto q0=mix(p0,p1),q1=mix(p1,p2),q2=mix(p2,p3);
    const auto r0=mix(q0,q1),r1=mix(q1,q2);
    const auto joined=mix(r0,r1);
    // Preserve *exact* direct edges without unnecessary Bezier sampling.
    const auto record=objects_.Find(edgeId);
    const std::string originalName=record?record->name:std::string("Path");
    const bool ownsTransaction=!commands_.HasActiveTransaction();
    if(ownsTransaction)commands_.BeginTransaction("Split Infrastructure Edge");
    try
    {
        const auto node=CreateNode(old->network,std::move(nodeName),
            FramePointAnchor{.frame=a->frame,.localMeters=joined});
        // Retain the original edge ID to preserve references to its entry.
        commands_.SetProperty(edgeId,kEdgeEndNode,ToReference(node.id));
        PathEdgeRecord second{};
        if(old->mode==EdgeMode::Bezier)
        {
            commands_.SetProperty(edgeId,kBezierStartHandle,q0-p0);
            commands_.SetProperty(edgeId,kBezierEndHandle,r0-joined);
            second=ConnectBezier(node.id,old->endNode,r1-joined,
                                 q2-p3,originalName+" (2)");
        }
        else
            second=ConnectDirect(node.id,old->endNode,originalName+" (2)");
        if(!old->profileOverride.empty())
            SetProfile(second.id,old->profileOverride);
        if(!old->infrastructureCatalogId.empty())
            SetInfrastructureIntent(second.id,old->infrastructureCatalogId,
                old->infrastructureCatalogVersion,old->constructionMode,
                old->designSpeedKmh,old->easeTurns);
        if(ownsTransaction)commands_.CommitTransaction();
        return {node,*FindEdge(edgeId),*FindEdge(second.id)};
    }
    catch(...)
    {
        if(ownsTransaction && commands_.HasActiveTransaction())
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

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Edit Bezier Handles");
    }

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
        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

void PathNetworkService::SetInfrastructureIntent(
    const scene::ObjectId edge, std::string catalogId,
    const i64 catalogVersion, std::string constructionMode,
    const f64 designSpeedKmh, const bool easeTurns)
{
    if (!FindEdge(edge).has_value())
        throw std::invalid_argument("Infrastructure intent requires a valid path edge.");
    if (catalogId.empty() || catalogVersion < 1 ||
        (constructionMode != "terrain" && constructionMode != "supported" &&
         constructionMode != "bridge" && constructionMode != "deck" &&
         constructionMode != "tunnel") ||
        !std::isfinite(designSpeedKmh) || designSpeedKmh < 0.0)
        throw std::invalid_argument("Invalid infrastructure catalog, version, construction or design speed.");
    const bool ownsTransaction = !commands_.HasActiveTransaction();
    if (ownsTransaction) commands_.BeginTransaction("Set Infrastructure Intent");
    try
    {
        commands_.SetProperty(edge, kInfrastructureCatalogId, std::move(catalogId));
        commands_.SetProperty(edge, kInfrastructureCatalogVersion, catalogVersion);
        commands_.SetProperty(edge, kInfrastructureConstruction, std::move(constructionMode));
        commands_.SetProperty(edge, kInfrastructureDesignSpeed, designSpeedKmh);
        commands_.SetProperty(edge, kInfrastructureEaseTurns, easeTurns);
        if (ownsTransaction) commands_.CommitTransaction();
    }
    catch (...)
    {
        if (ownsTransaction && commands_.HasActiveTransaction()) commands_.RollbackTransaction();
        throw;
    }
}

void PathNetworkService::SetProfile(
    const scene::ObjectId pathObject,
    std::string profileAsset)
{
    const auto stored =
        objects_.Find(pathObject);

    if (!stored.has_value())
    {
        throw std::invalid_argument(
            "Cannot assign a profile to an unknown path object.");
    }

    schema::PropertyId property{};

    if (stored->type == kPathNetworkType)
    {
        property = kNetworkProfile;
    }
    else if (stored->type == kPathEdgeType)
    {
        property = kEdgeProfileOverride;
    }
    else
    {
        throw std::invalid_argument(
            "Path profiles may be assigned only to networks or edges.");
    }

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Assign Path Profile");
    }

    try
    {
        commands_.SetProperty(
            pathObject,
            property,
            std::move(profileAsset));

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }

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
                kEdgeProfileOverride, {}),
        .infrastructureCatalogId = PropertyOr<std::string>(objects_, edge, kInfrastructureCatalogId, {}),
        .infrastructureCatalogVersion = PropertyOr<i64>(objects_, edge, kInfrastructureCatalogVersion, 1),
        .constructionMode = PropertyOr<std::string>(objects_, edge, kInfrastructureConstruction, "terrain"),
        .designSpeedKmh = PropertyOr<f64>(objects_, edge, kInfrastructureDesignSpeed, 0.0),
        .easeTurns = PropertyOr<bool>(objects_, edge, kInfrastructureEaseTurns, true)
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
