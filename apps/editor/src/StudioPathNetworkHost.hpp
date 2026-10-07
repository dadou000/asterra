#pragma once

// The Studio path network's route planning and derived-geometry products: it asks the StudioSession route
// planner for every routed edge, publishes path.route_ready / path.route_failed / path.derived_ready events as the
// results arrive, rebuilds the derived centreline/mesh products when edges, content or routes change, and serves the
// path.route and path.geometry RPC queries. Moved out of Main.cpp's frame setup, bodies unchanged.

#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <unordered_map>
#include <unordered_set>

namespace orbit::content
{
class ContentService;
}

namespace orbit::documents
{
class ProjectDocument;
}

namespace orbit::editor_rpc
{
class EditorSessionRpcHost;
}

namespace orbit::editor_session
{
class EditorWorldSession;
}

namespace orbit::frames
{
class FrameGraph;
}

namespace orbit::path_routing
{
class RoutePlanner;
}

namespace orbit::studio_session
{
class StudioSession;
}

namespace orbit::universe
{
class BodyRegistry;
}

namespace orbit::editor_app
{
class StudioPathNetworkHost
{
public:
    StudioPathNetworkHost(
        editor_session::EditorWorldSession& worldSession,
        studio_session::StudioSession& studioSession,
        documents::ProjectDocument& project,
        content::ContentService& content,
        editor_rpc::EditorSessionRpcHost& rpcHost) noexcept;

    // Registers the path.route and path.geometry RPC queries on the session host.
    void AttachRpc();

    // Queues a route request for every routed edge (and drops planner state for removed edges).
    void RequestRoutes();

    // Re-requests when the objects or content changed, polls the planner and publishes finished routes.
    void PollRoutes();

    // Rebuilds derived path products when an edge, the content or a route generation changed.
    void RefreshDerivedPaths();

    // StudioSession rebound its planner: every route must be requested and published again.
    void OnRoutingRebound() noexcept;

    // Path products were invalidated upstream: drop every derived product.
    void OnPathProductsInvalidated() noexcept;

    // No world is open: forget the derived products and known edges.
    void Clear() noexcept;

private:
    [[nodiscard]] scene::ObjectStore& objects() const;
    [[nodiscard]] frames::FrameGraph& frames() const;
    [[nodiscard]] universe::BodyRegistry& bodies() const;

    // Route planning is owned by StudioSession and rebound whenever the composed universe generation changes.
    // Resolve it on use so no reference survives a generation swap.
    [[nodiscard]] path_routing::RoutePlanner& routePlanner() const;

    editor_session::EditorWorldSession& worldSession_;
    studio_session::StudioSession& studioSession_;
    documents::ProjectDocument& project_;
    content::ContentService& content_;
    editor_rpc::EditorSessionRpcHost& rpcHost_;

    std::unordered_set<scene::ObjectId> knownRoutedEdges_;
    std::unordered_map<scene::ObjectId, u64> publishedRouteGeneration_;
    u64 routedObjectRevision_{~u64{0}};
    u64 routedContentRevision_{~u64{0}};

    std::unordered_map<scene::ObjectId, path_geometry::PathDerivedProduct> derivedPaths_;
    std::unordered_map<scene::ObjectId, u64> derivedRouteGeneration_;
    u64 derivedObjectRevision_{~u64{0}};
    u64 derivedContentRevision_{~u64{0}};
};
} // namespace orbit::editor_app
