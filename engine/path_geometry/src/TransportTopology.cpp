#include <orbit/path_geometry/TransportTopology.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace orbit::path_geometry
{
TransportTopology CompileTransportTopology(
    std::span<const TransportNode> nodes,
    std::span<const TransportLink> links)
{
    using Id = scene::ObjectId;
    using Key = std::pair<TransportMode,Id>;
    TransportTopology out;
    std::map<Id,TransportNode> nodeIndex;
    std::set<Id> linkIds;
    for (const auto& n:nodes)
    {
        if (!n.id || !n.network)
            out.findings.push_back({TransportTopologyIssue::InvalidNode,n.id});
        else if (!nodeIndex.emplace(n.id,n).second)
            out.findings.push_back({TransportTopologyIssue::DuplicateNode,n.id});
    }
    std::map<Id,std::vector<TransportLink>> incidence;
    std::map<Key,std::vector<std::pair<Id,Id>>> adjacency;
    for (const auto& edge:links)
    {
        if (!edge.id || !edge.network || !edge.start || !edge.end)
        {
            out.findings.push_back({TransportTopologyIssue::InvalidLink,edge.id});
            continue;
        }
        if (!linkIds.insert(edge.id).second)
        {
            out.findings.push_back({TransportTopologyIssue::DuplicateLink,edge.id});
            continue;
        }
        if (edge.start==edge.end)
        {
            out.findings.push_back({TransportTopologyIssue::SelfLoop,edge.id});
            continue;
        }
        const auto a=nodeIndex.find(edge.start), b=nodeIndex.find(edge.end);
        if (a==nodeIndex.end() || b==nodeIndex.end())
        {
            out.findings.push_back({TransportTopologyIssue::MissingEndpoint,edge.id});
            continue;
        }
        if (a->second.network!=edge.network || b->second.network!=edge.network)
        {
            out.findings.push_back({TransportTopologyIssue::NetworkMismatch,edge.id});
            continue;
        }
        incidence[edge.start].push_back(edge);
        incidence[edge.end].push_back(edge);
        adjacency[{edge.mode,edge.start}].push_back({edge.end,edge.id});
        adjacency[{edge.mode,edge.end}].push_back({edge.start,edge.id});
        out.arcs.push_back({edge.id,edge.start,edge.end,edge.mode});
        if (edge.bidirectional)
            out.arcs.push_back({edge.id,edge.end,edge.start,edge.mode});
    }
    for (const auto& [id,edges]:incidence)
    {
        if (edges.size()<2) continue;
        TransportJunction node{.node=id};
        for (const auto& edge:edges)
        {
            node.incidentLinks.push_back(edge.id);
            node.roadLinks += edge.mode==TransportMode::Road ? 1U : 0U;
            node.railLinks += edge.mode==TransportMode::Rail ? 1U : 0U;
        }
        std::sort(node.incidentLinks.begin(),node.incidentLinks.end());
        node.explicitLevelCrossing=node.roadLinks>0 && node.railLinks>0;
        out.junctions.push_back(std::move(node));
    }
    std::set<Key> visited;
    for (const auto& [root,neighbors]:adjacency)
    {
        static_cast<void>(neighbors);
        if (!visited.insert(root).second) continue;
        TransportComponent component{.mode=root.first};
        std::vector<Key> pending{root};
        std::set<Id> componentLinks;
        while (!pending.empty())
        {
            const Key current=pending.back();
            pending.pop_back();
            component.nodes.push_back(current.second);
            for (const auto& [neighbor,link]:adjacency.at(current))
            {
                componentLinks.insert(link);
                const Key next{current.first,neighbor};
                if (visited.insert(next).second) pending.push_back(next);
            }
        }
        std::sort(component.nodes.begin(),component.nodes.end());
        component.links.assign(componentLinks.begin(),componentLinks.end());
        out.components.push_back(std::move(component));
    }
    std::sort(out.arcs.begin(),out.arcs.end(),[](const auto& a,const auto& b) {
        if (a.mode!=b.mode) return a.mode<b.mode;
        if (a.from!=b.from) return a.from<b.from;
        if (a.to!=b.to) return a.to<b.to;
        return a.link<b.link;
    });
    std::sort(out.components.begin(),out.components.end(),[](const auto& a,const auto& b){
        if (a.mode!=b.mode) return a.mode<b.mode;
        return a.nodes.front()<b.nodes.front();
    });
    std::sort(out.findings.begin(),out.findings.end(),[](const auto& a,const auto& b){
        if (a.object!=b.object) return a.object<b.object;
        return a.issue<b.issue;
    });
    return out;
}
} // namespace orbit::path_geometry
