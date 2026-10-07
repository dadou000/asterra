#include "StudioPathNetworkHost.hpp"

#include "EditorAppSupport.hpp"

#include <orbit/content/ContentService.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_rpc/EditorSessionRpcHost.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_routing/RouteDomains.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/universe/ReferenceSurface.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::editor_app
{
using namespace support;

StudioPathNetworkHost::StudioPathNetworkHost(
    editor_session::EditorWorldSession& worldSession,
    studio_session::StudioSession& studioSession,
    documents::ProjectDocument& project,
    content::ContentService& content,
    editor_rpc::EditorSessionRpcHost& rpcHost) noexcept
    : worldSession_(worldSession),
      studioSession_(studioSession),
      project_(project),
      content_(content),
      rpcHost_(rpcHost)
{
}

scene::ObjectStore& StudioPathNetworkHost::objects() const
{
    return worldSession_.Objects();
}

frames::FrameGraph& StudioPathNetworkHost::frames() const
{
    return worldSession_.Universe().Frames();
}

universe::BodyRegistry& StudioPathNetworkHost::bodies() const
{
    return worldSession_.Universe().Bodies();
}

path_routing::RoutePlanner& StudioPathNetworkHost::routePlanner() const
{
    return studioSession_.PathRouting().Planner();
}

void StudioPathNetworkHost::OnRoutingRebound() noexcept
{
    routedObjectRevision_ = ~u64{0};
    publishedRouteGeneration_.clear();
}

void StudioPathNetworkHost::OnPathProductsInvalidated() noexcept
{
    derivedObjectRevision_ = ~u64{0};
    derivedPaths_.clear();
    derivedRouteGeneration_.clear();
}

void StudioPathNetworkHost::Clear() noexcept
{
    derivedPaths_.clear();
    knownRoutedEdges_.clear();
}

void StudioPathNetworkHost::RequestRoutes()
{
    auto& pathService =
        studioSession_.PathNetwork().Service();

    const auto routedEdges =
        FindRoutedPathEdges(
            objects(),
            pathService);

    std::unordered_set<
        orbit::scene::ObjectId>
        liveEdges(
            routedEdges.begin(),
            routedEdges.end());

    for (const auto edge :
         knownRoutedEdges_)
    {
        if (!liveEdges.
                contains(edge))
        {
            routePlanner().Erase(edge);
            publishedRouteGeneration_.
                erase(edge);
        }
    }

    for (const auto edgeId :
         routedEdges)
    {
        try
        {
            const auto edge =
                pathService.FindEdge(
                    edgeId);

            if (!edge.has_value())
            {
                continue;
            }

            const auto start =
                pathService.FindNode(
                    edge->startNode);
            const auto end =
                pathService.FindNode(
                    edge->endNode);

            if (!start.has_value() ||
                !end.has_value())
            {
                throw std::runtime_error(
                    "Routed edge has an invalid endpoint.");
            }

            const auto resolvedProfile =
                ResolveRoutingProfile(
                    *edge,
                    pathService,
                    content_,
                    project_.
                        RootDirectory());

            orbit::path_routing::
                RouteEnvironment
                    environment;

            environment.search =
                RouteSearchForProfile(
                    resolvedProfile.
                        profile);

            const auto* startSurface =
                std::get_if<
                    orbit::paths::
                        SurfaceAnchor>(
                            &start->
                                anchor);
            const auto* endSurface =
                std::get_if<
                    orbit::paths::
                        SurfaceAnchor>(
                            &end->
                                anchor);

            if (startSurface !=
                    nullptr &&
                endSurface !=
                    nullptr &&
                startSurface->body ==
                    endSurface->body)
            {
                const auto* routeBody =
                    bodies().FindBody(
                        startSurface->
                            body);

                if (routeBody ==
                    nullptr)
                {
                    throw std::runtime_error(
                        "Routed surface edge references an unknown runtime body.");
                }

                environment =
                    orbit::path_routing::
                        MakeReferenceSurfaceEnvironment(
                            startSurface->
                                body,
                            routeBody->
                                frame,
                            {},
                            frames(),
                            bodies(),
                            RouteSearchForProfile(
                                resolvedProfile.
                                    profile));
            }

            static_cast<void>(
                routePlanner().Request({
                    .edge = *edge,
                    .startNode =
                        *start,
                    .endNode =
                        *end,
                    .profile =
                        resolvedProfile.
                            profile,
                    .profileRevision =
                        resolvedProfile.
                            revision,
                    .environment =
                        std::move(
                            environment)
                }));
        }
        catch (const std::exception&
                   exception)
        {
            routePlanner().Erase(
                edgeId);

            orbit::log::Warning(
                std::format(
                    "Route '{}': {}",
                    edgeId.ToString(),
                    exception.what()));
        }
    }

    knownRoutedEdges_ =
        std::move(liveEdges);
    routedObjectRevision_ =
        objects().Revision();
    routedContentRevision_ =
        content_.Revision();
}

void StudioPathNetworkHost::PollRoutes()
{
    if (objects().Revision() !=
            routedObjectRevision_ ||
        content_.Revision() !=
            routedContentRevision_)
    {
        RequestRoutes();
    }

    routePlanner().Poll();

    for (const auto edge :
         knownRoutedEdges_)
    {
        const auto status =
            routePlanner().Status(edge);

        if (!status.has_value())
        {
            continue;
        }

        const auto published =
            publishedRouteGeneration_.
                find(edge);

        if (published !=
                publishedRouteGeneration_.
                    end() &&
            published->second ==
                status->generation)
        {
            continue;
        }

        if (status->state ==
            orbit::path_routing::
                RouteState::Ready)
        {
            const auto* result =
                routePlanner().Result(edge);

            if (result == nullptr)
            {
                continue;
            }

            publishedRouteGeneration_[
                edge] =
                    status->
                        generation;

            rpcHost_.PublishEvent(
                "path.route_ready",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {
                            "generation",
                            static_cast<
                                orbit::i64>(
                                    status->
                                        generation)
                        },
                        {
                            "revision",
                            static_cast<
                                orbit::i64>(
                                    status->
                                        committedRevision)
                        },
                        {
                            "points",
                            static_cast<
                                orbit::i64>(
                                    result->
                                        points.
                                        size())
                        },
                        {
                            "total_cost",
                            result->
                                totalCost
                        },
                        {
                            "cross_frame",
                            result->
                                crossFrameEndpoints
                        }
                    }));
        }
        else if (
            status->state ==
                orbit::path_routing::
                    RouteState::Failed)
        {
            publishedRouteGeneration_[
                edge] =
                    status->
                        generation;

            orbit::log::Warning(
                std::format(
                    "Route '{}' failed: {}",
                    edge.ToString(),
                    status->error));

            rpcHost_.PublishEvent(
                "path.route_failed",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {
                            "generation",
                            static_cast<
                                orbit::i64>(
                                    status->
                                        generation)
                        },
                        {
                            "error",
                            status->error
                        }
                    }));
        }
    }
}

void StudioPathNetworkHost::RefreshDerivedPaths()
{
    bool requiresRefresh =
        objects().Revision() !=
            derivedObjectRevision_ ||
        content_.Revision() !=
            derivedContentRevision_;

    for (const auto edge :
         knownRoutedEdges_)
    {
        const auto status =
            routePlanner().Status(edge);

        if (!status.has_value() ||
            status->state !=
                orbit::path_routing::
                    RouteState::Ready)
        {
            continue;
        }

        const auto built =
            derivedRouteGeneration_.
                find(edge);

        if (built ==
                derivedRouteGeneration_.
                    end() ||
            built->second !=
                status->generation)
        {
            requiresRefresh = true;
            break;
        }
    }

    if (!requiresRefresh)
    {
        return;
    }

    auto& pathService =
        studioSession_.PathNetwork().Service();

    const auto edgeIds =
        FindPathEdges(
            objects(),
            pathService);

    std::unordered_set<
        orbit::scene::ObjectId>
        liveEdges(
            edgeIds.begin(),
            edgeIds.end());

    for (auto item =
             derivedPaths_.begin();
         item !=
             derivedPaths_.end();)
    {
        if (!liveEdges.contains(
                item->first))
        {
            derivedRouteGeneration_.
                erase(item->first);
            item =
                derivedPaths_.erase(
                    item);
        }
        else
        {
            ++item;
        }
    }

    for (const auto edgeId :
         edgeIds)
    {
        try
        {
            const auto edge =
                pathService.FindEdge(
                    edgeId);

            if (!edge.has_value())
            {
                continue;
            }

            const auto start =
                pathService.FindNode(
                    edge->startNode);
            const auto end =
                pathService.FindNode(
                    edge->endNode);

            if (!start.has_value() ||
                !end.has_value())
            {
                throw std::runtime_error(
                    "Path geometry edge has an invalid endpoint.");
            }

            const auto resolvedProfile =
                ResolveRoutingProfile(
                    *edge,
                    pathService,
                    content_,
                    project_.
                        RootDirectory());

            orbit::frames::FrameId
                targetFrame{};
            const orbit::path_routing::
                RouteResult*
                    routeResult =
                        nullptr;
            orbit::u64 routeGeneration =
                0;

            if (edge->mode ==
                orbit::paths::
                    EdgeMode::Routed)
            {
                const auto status =
                    routePlanner().Status(
                        edgeId);

                if (!status.has_value() ||
                    status->state !=
                        orbit::path_routing::
                            RouteState::Ready)
                {
                    continue;
                }

                routeResult =
                    routePlanner().Result(
                        edgeId);

                if (routeResult ==
                    nullptr)
                {
                    continue;
                }

                targetFrame =
                    routeResult->frame;
                routeGeneration =
                    status->generation;
            }
            else
            {
                const auto startFrame =
                    PathAnchorNativeFrame(
                        start->anchor,
                        bodies());
                const auto endFrame =
                    PathAnchorNativeFrame(
                        end->anchor,
                        bodies());

                targetFrame =
                    startFrame.
                        value_or(
                            endFrame.
                                value_or(
                                    orbit::frames::
                                        FrameId{}));

                if (!targetFrame)
                {
                    continue;
                }
            }

            std::string failure;

            const orbit::f64
                sampleSpacing =
                    std::clamp(
                        resolvedProfile.
                            profile.
                            widthMeters *
                            0.5,
                        1.0,
                        4.0);

            const auto centerline =
                orbit::path_geometry::
                    BuildPathCenterline({
                        .edge = *edge,
                        .startNode = *start,
                        .endNode = *end,
                        .targetFrame =
                            targetFrame,
                        .frames = &frames(),
                        .bodies = &bodies(),
                        .routed =
                            routeResult,
                        .curveSampleSpacingMeters =
                            sampleSpacing
                    },
                    &failure);

            if (!centerline.has_value())
            {
                throw std::runtime_error(
                    failure.empty()
                        ? "Path centerline derivation failed."
                        : failure);
            }

            auto product =
                orbit::path_geometry::
                    BuildPathDerived(
                        *centerline,
                        resolvedProfile.
                            profile,
                        {
                            .sampleSpacingMeters =
                                sampleSpacing
                        });

            const auto existing =
                derivedPaths_.find(
                    edgeId);

            const bool changed =
                existing ==
                    derivedPaths_.end() ||
                existing->second.
                    buildSignature !=
                    product.
                        buildSignature;

            if (changed)
            {
                derivedPaths_[
                    edgeId] =
                        std::move(
                            product);

                const auto& committed =
                    derivedPaths_.at(
                        edgeId);

                rpcHost_.PublishEvent(
                    "path.derived_ready",
                    orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "edge",
                                edgeId.
                                    ToString()
                            },
                            {
                                "signature",
                                static_cast<
                                    orbit::i64>(
                                        committed.
                                            buildSignature &
                                        0x7fffffffffffffffULL)
                            },
                            {
                                "vertices",
                                static_cast<
                                    orbit::i64>(
                                        committed.
                                            visualMesh.
                                            vertices.
                                            size())
                            },
                            {
                                "lanes",
                                static_cast<
                                    orbit::i64>(
                                        committed.
                                            lanes.
                                            size())
                            },
                            {
                                "nav_samples",
                                static_cast<
                                    orbit::i64>(
                                        committed.
                                            navigation.
                                            size())
                            }
                        }));
            }

            if (edge->mode ==
                orbit::paths::
                    EdgeMode::Routed)
            {
                derivedRouteGeneration_[
                    edgeId] =
                        routeGeneration;
            }
        }
        catch (const std::exception&
                   exception)
        {
            orbit::log::Warning(
                std::format(
                    "Path geometry '{}': {}",
                    edgeId.ToString(),
                    exception.what()));
        }
    }

    derivedObjectRevision_ =
        objects().Revision();
    derivedContentRevision_ =
        content_.Revision();
}

void StudioPathNetworkHost::AttachRpc()
{
    rpcHost_.AttachPathRouting({
        .status =
            [this](
                const orbit::scene::ObjectId edge)
            {
                const auto status =
                    routePlanner().Status(edge);

                if (!status.has_value())
                {
                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {"state", "missing"},
                            {
                                "edge",
                                edge.ToString()
                            }
                        });
                }

                const char* stateName =
                    "missing";

                switch (status->state)
                {
                case orbit::path_routing::
                        RouteState::Missing:
                    stateName = "missing";
                    break;
                case orbit::path_routing::
                        RouteState::Dirty:
                    stateName = "dirty";
                    break;
                case orbit::path_routing::
                        RouteState::Building:
                    stateName = "building";
                    break;
                case orbit::path_routing::
                        RouteState::Ready:
                    stateName = "ready";
                    break;
                case orbit::path_routing::
                        RouteState::Failed:
                    stateName = "failed";
                    break;
                }

                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {"state", stateName},
                        {
                            "generation",
                            static_cast<
                                orbit::i64>(
                                    status->
                                        generation)
                        },
                        {
                            "committed_revision",
                            static_cast<
                                orbit::i64>(
                                    status->
                                        committedRevision)
                        },
                        {
                            "dependency_signature",
                            static_cast<
                                orbit::i64>(
                                    status->
                                        dependencySignature &
                                    0x7fffffffffffffffULL)
                        },
                        {
                            "cross_frame",
                            status->
                                crossFrameEndpoints
                        },
                        {
                            "error",
                            status->error
                        }
                    });
            },
        .result =
            [this](
                const orbit::scene::ObjectId edge)
            {
                const auto* result =
                    routePlanner().Result(edge);

                if (result == nullptr)
                {
                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "edge",
                                edge.ToString()
                            },
                            {"ready", false}
                        });
                }

                orbit::rpc::Value::Array
                    points;

                points.reserve(
                    result->points.size());

                for (const auto& point :
                     result->points)
                {
                    points.emplace_back(
                        orbit::rpc::Value::Object{
                            {
                                "position",
                                orbit::rpc::Value::Array{
                                    point.localMeters.x,
                                    point.localMeters.y,
                                    point.localMeters.z
                                }
                            },
                            {
                                "elevation_meters",
                                point.
                                    elevationMeters
                            },
                            {
                                "water_depth_meters",
                                point.
                                    waterDepthMeters
                            }
                        });
                }

                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {"ready", true},
                        {
                            "frame",
                            result->frame.
                                ToString()
                        },
                        {
                            "generation",
                            static_cast<
                                orbit::i64>(
                                    result->
                                        generation)
                        },
                        {
                            "dependency_signature",
                            static_cast<
                                orbit::i64>(
                                    result->
                                        dependencySignature &
                                    0x7fffffffffffffffULL)
                        },
                        {
                            "total_cost",
                            result->totalCost
                        },
                        {
                            "cross_frame",
                            result->
                                crossFrameEndpoints
                        },
                        {
                            "points",
                            std::move(points)
                        }
                    });
            },
        .invalidate =
            [this](
                const orbit::scene::ObjectId edge)
            {
                routePlanner().Invalidate(edge);
            }
    });

    rpcHost_.AttachPathGeometry({
        .result =
            [this](
                const orbit::scene::ObjectId edge)
            {
                const auto found =
                    derivedPaths_.find(edge);

                if (found ==
                    derivedPaths_.end())
                {
                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "edge",
                                edge.ToString()
                            },
                            {"ready", false}
                        });
                }

                const auto& product =
                    found->second;

                orbit::rpc::Value::Array
                    lanes;
                lanes.reserve(
                    product.lanes.size());

                for (const auto& lane :
                     product.lanes)
                {
                    orbit::rpc::Value::Array
                        points;
                    points.reserve(
                        lane.points.size());

                    for (const auto& point :
                         lane.points)
                    {
                        points.emplace_back(
                            orbit::rpc::Value::Object{
                                {
                                    "station_meters",
                                    point.
                                        stationMeters
                                },
                                {
                                    "position",
                                    orbit::rpc::Value::Array{
                                        point.position.x,
                                        point.position.y,
                                        point.position.z
                                    }
                                },
                                {
                                    "tangent",
                                    orbit::rpc::Value::Array{
                                        point.tangent.x,
                                        point.tangent.y,
                                        point.tangent.z
                                    }
                                }
                            });
                    }

                    lanes.emplace_back(
                        orbit::rpc::Value::Object{
                            {
                                "lane",
                                static_cast<
                                    orbit::i64>(
                                        lane.
                                            laneIndex)
                            },
                            {
                                "lateral_offset_meters",
                                lane.
                                    lateralOffsetMeters
                            },
                            {
                                "points",
                                std::move(points)
                            }
                        });
                }

                orbit::rpc::Value::Array
                    navigation;
                navigation.reserve(
                    product.navigation.
                        size());

                for (const auto& sample :
                     product.navigation)
                {
                    navigation.emplace_back(
                        orbit::rpc::Value::Object{
                            {
                                "station_meters",
                                sample.
                                    stationMeters
                            },
                            {
                                "position",
                                orbit::rpc::Value::Array{
                                    sample.position.x,
                                    sample.position.y,
                                    sample.position.z
                                }
                            },
                            {
                                "tangent",
                                orbit::rpc::Value::Array{
                                    sample.tangent.x,
                                    sample.tangent.y,
                                    sample.tangent.z
                                }
                            },
                            {
                                "half_width_meters",
                                sample.
                                    halfWidthMeters
                            },
                            {
                                "lanes",
                                static_cast<
                                    orbit::i64>(
                                        sample.lanes)
                            }
                        });
                }

                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {"ready", true},
                        {
                            "frame",
                            product.frame.
                                ToString()
                        },
                        {
                            "build_signature",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        buildSignature &
                                    0x7fffffffffffffffULL)
                        },
                        {
                            "width_meters",
                            product.widthMeters
                        },
                        {
                            "lane_count",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        laneCount)
                        },
                        {
                            "stations",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        stations.
                                        size())
                        },
                        {
                            "visual_vertices",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        visualMesh.
                                        vertices.
                                        size())
                        },
                        {
                            "visual_indices",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        visualMesh.
                                        indices.
                                        size())
                        },
                        {
                            "collision_triangles",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        collision.
                                        size())
                        },
                        {
                            "reference_nodes",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        referenceGraph.
                                        nodes.
                                        size())
                        },
                        {
                            "reference_edges",
                            static_cast<
                                orbit::i64>(
                                    product.
                                        referenceGraph.
                                        edges.
                                        size())
                        },
                        {
                            "lanes",
                            std::move(lanes)
                        },
                        {
                            "navigation",
                            std::move(
                                navigation)
                        }
                    });
            }
    });
}
} // namespace orbit::editor_app
