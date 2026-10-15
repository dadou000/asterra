#include <orbit/path_geometry/RoadJunctionPublication.hpp>
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace orbit::path_geometry {
namespace {
bool CollisionMatches(const PathMesh& mesh,
                      const std::vector<CollisionTriangle>& triangles)
{
    if(mesh.indices.size()%3!=0 || mesh.indices.size()/3!=triangles.size())
        return false;
    for(std::size_t k=0;k<triangles.size();++k)
    {
        const auto a=mesh.indices[k*3],b=mesh.indices[k*3+1],
                   c=mesh.indices[k*3+2];
        if(a>=mesh.vertices.size() || b>=mesh.vertices.size() ||
           c>=mesh.vertices.size())return false;
        if(triangles[k].a!=mesh.vertices[a].position ||
           triangles[k].b!=mesh.vertices[b].position ||
           triangles[k].c!=mesh.vertices[c].position)return false;
    }
    return true;
}
u64 Mix(u64 previous,u64 value) {
    return (previous^value)*0x100000001b3ULL;
}
void TrimReferences(PathDerivedProduct& derived,
                    std::size_t begin,std::size_t end)
{
    const auto n=derived.stations.size();
    if(begin>=end || end>n || end-begin<2)
        throw std::invalid_argument("Overlapping junction cut boundaries.");
    const auto crop=[&](auto& items) {
        if(items.size()!=n) return false;
        items=std::decay_t<decltype(items)>(
            items.begin()+static_cast<std::ptrdiff_t>(begin),
            items.begin()+static_cast<std::ptrdiff_t>(end));
        return true;
    };
    if(!crop(derived.stations)||!crop(derived.navigation))
        throw std::invalid_argument("Station/navigation sizes differ.");
    derived.referenceGraph={};
    derived.debugLines.clear();
    u32 nodeId=0;
    for(auto& lane:derived.lanes)
    {
        if(!crop(lane.points))
            throw std::invalid_argument("Lane sample count differs.");
        for(std::size_t j=0;j<lane.points.size();++j) {
            const auto& point=lane.points[j];
            derived.referenceGraph.nodes.push_back({
                .index=nodeId++,.laneIndex=lane.laneIndex,
                .stationMeters=point.stationMeters,
                .position=point.position,.tangent=point.tangent
            });
            if(j>0) {
                const auto from=nodeId-2U,to=nodeId-1U;
                derived.referenceGraph.edges.push_back({
                    .from=from,.to=to,
                    .lengthMeters=math::Length(
                        lane.points[j].position-lane.points[j-1].position),
                    .bidirectional=true
                });
            }
        }
    }
}
PathMesh CropRoadMesh(const PathMesh& raw, std::size_t stations,
                      std::size_t start,std::size_t end)
{
    if(stations<3 || raw.vertices.size()!=stations*7U ||
       start>=end || end>stations || end-start<2)
        throw std::invalid_argument("Invalid source road section range.");
    PathMesh output;
    std::vector<u32> oldToNew(raw.vertices.size(),~u32{0});
    struct BandSpan {u32 columns;std::size_t offset;};
    const BandSpan bands[]{
        {3U,0U},{2U,stations*3U},{2U,stations*5U}
    };
    for(const auto band:bands)
        for(std::size_t i=start;i<end;++i)
            for(u32 c=0;c<band.columns;++c)
            {
                const auto old=band.offset+i*band.columns+c;
                oldToNew[old]=static_cast<u32>(output.vertices.size());
                output.vertices.push_back(raw.vertices[old]);
            }
    for(std::size_t k=0;k+2<raw.indices.size();k+=3)
    {
        const auto a=raw.indices[k],b=raw.indices[k+1],c=raw.indices[k+2];
        if(a>=oldToNew.size()||b>=oldToNew.size()||
           c>=oldToNew.size())
            throw std::invalid_argument("Invalid road source index.");
        if(oldToNew[a]!=~u32{0} && oldToNew[b]!=~u32{0} &&
           oldToNew[c]!=~u32{0})
            output.indices.insert(output.indices.end(),
                {oldToNew[a],oldToNew[b],oldToNew[c]});
    }
    return output;
}
std::vector<CollisionTriangle> MeshCollision(const PathMesh& mesh)
{
    std::vector<CollisionTriangle> collision;
    collision.reserve(mesh.indices.size()/3);
    for(std::size_t i=0;i+2<mesh.indices.size();i+=3)
        collision.push_back({
            mesh.vertices[mesh.indices[i]].position,
            mesh.vertices[mesh.indices[i+1]].position,
            mesh.vertices[mesh.indices[i+2]].position
        });
    return collision;
}
} // namespace

RoadJunctionPublicationResult AssembleRoadJunctionScene(
    const frames::FrameId frame,
    const u64 worldGeneration,
    const std::span<const RoadJunctionConstruction> junctions,
    const std::span<const PathDerivedProduct* const> rawEdges,
    const std::span<const scene::ObjectId> dirtyEdges,
    const std::size_t maximumVertices)
{
    RoadJunctionPublicationResult result;
    const auto fail=[&](RoadJunctionPublicationIssue issue,
                        scene::ObjectId id,const char* message) {
        result.packet={};
        result.findings.push_back({issue,id,message});
    };
    if(!frame || !worldGeneration || junctions.empty() ||
       junctions.size()>64 || rawEdges.empty() || rawEdges.size()>512 ||
       maximumVertices<64U || maximumVertices>1000000U) {
        fail(RoadJunctionPublicationIssue::InvalidInput,{},
             "Invalid world, frame, input size or build budget.");
        return result;
    }
    std::map<scene::ObjectId,const PathDerivedProduct*> sources;
    for(const auto* product:rawEdges)
        if(!product || !product->edge ||
           !sources.emplace(product->edge,product).second) {
            fail(RoadJunctionPublicationIssue::InvalidInput,{},
                 "Null or duplicated source corridor.");
            return result;
        }
    const std::set<scene::ObjectId> dirty(dirtyEdges.begin(),dirtyEdges.end());
    struct Cuts { const RoadJunctionTrimmedEdge* front{};
                  const RoadJunctionTrimmedEdge* back{}; };
    std::map<scene::ObjectId,Cuts> edgeCuts;
    std::set<scene::ObjectId> consumedNodes;
    result.packet.frame=frame;
    result.packet.worldGeneration=worldGeneration;
    u64 signature=0xcbf29ce484222325ULL;
    signature=Mix(signature,frame.high);
    signature=Mix(signature,frame.low);
    signature=Mix(signature,worldGeneration);
    std::vector<const RoadJunctionConstruction*> sorted;
    for(const auto& j:junctions)sorted.push_back(&j);
    std::sort(sorted.begin(),sorted.end(),[](const auto* a,const auto* b) {
        return a->node<b->node;
    });
    std::size_t vertexCount=0;
    for(const auto* junction:sorted)
    {
        if(!junction->Ready() || !junction->dependencySignature ||
           junction->frame!=frame) {
            fail(RoadJunctionPublicationIssue::IncompleteConstruction,
                 junction->node,"Unready junction or mismatched frame.");
            return result;
        }
        if(!consumedNodes.insert(junction->node).second) {
            fail(RoadJunctionPublicationIssue::InvalidInput,junction->node,
                "Same semantic junction submitted more than once.");
            return result;
        }
        if(!CollisionMatches(junction->pavement,junction->pavementCollision)) {
            fail(RoadJunctionPublicationIssue::InvalidCollision,junction->node,
                 "Patch render and collision data disagree.");
            return result;
        }
        if(junction->pavement.vertices.size()>maximumVertices-vertexCount) {
            fail(RoadJunctionPublicationIssue::BudgetExceeded,junction->node,
                 "Patch geometry exceeds publication budget.");
            return result;
        }
        vertexCount+=junction->pavement.vertices.size();
        signature=Mix(signature,junction->node.high);
        signature=Mix(signature,junction->node.low);
        signature=Mix(signature,junction->dependencySignature);
        result.packet.patches.push_back({
            junction->node,junction->frame,junction->dependencySignature,
            junction->pavement,junction->pavementCollision
        });
        for(const auto& trimmed:junction->edges)
        {
            auto& cuts=edgeCuts[trimmed.edge];
            auto*& slot=trimmed.atStart?cuts.front:cuts.back;
            if(slot) {
                fail(RoadJunctionPublicationIssue::SharedEdge,trimmed.edge,
                     "More than one junction claims the same end of a road.");
                return result;
            }
            if(dirty.contains(trimmed.edge)) {
                fail(RoadJunctionPublicationIssue::DirtySource,trimmed.edge,
                     "An authored edge has pending construction.");
                return result;
            }
            const auto it=sources.find(trimmed.edge);
            if(it==sources.end()) {
                fail(RoadJunctionPublicationIssue::MissingSource,trimmed.edge,
                     "Missing source corridor.");
                return result;
            }
            const auto& raw=*it->second;
            if(raw.frame!=frame) {
                fail(RoadJunctionPublicationIssue::MixedFrames,trimmed.edge,
                     "Cross-frame road cannot be published.");
                return result;
            }
            if(raw.buildSignature!=trimmed.sourceBuildSignature ||
               raw.sourceRevision!=trimmed.sourceRevision) {
                fail(RoadJunctionPublicationIssue::ChangedSource,trimmed.edge,
                     "Junction cut was built from an obsolete corridor.");
                return result;
            }
            if(!CollisionMatches(trimmed.retainedMesh,trimmed.collision)) {
                fail(RoadJunctionPublicationIssue::InvalidCollision,trimmed.edge,
                     "Cut mesh and collider disagree.");
                return result;
            }
            slot=&trimmed;
        }
        result.packet.connections.insert(result.packet.connections.end(),
            junction->laneConnectors.begin(),junction->laneConnectors.end());
    }
    // Fold cuts at both ends of an edge into one retained strip, rather
    // than publishing duplicate overlapping road fragments.
    for(const auto& [id,cuts]:edgeCuts)
    {
        const auto source=sources.find(id);
        if(source==sources.end()) {
            fail(RoadJunctionPublicationIssue::MissingSource,id,
                 "Source disappeared before final publication.");
            return result;
        }
        const auto& raw=*source->second;
        const auto n=raw.stations.size();
        const auto start=cuts.front?
            static_cast<std::size_t>(cuts.front->sourceCutStation):0U;
        const auto end=cuts.back?
            static_cast<std::size_t>(cuts.back->sourceCutStation)+1U:n;
        if(start>=end || end-start<2U) {
            fail(RoadJunctionPublicationIssue::InvalidNavigation,id,
                 "Opposite junction cuts overlap or leave no driving surface.");
            return result;
        }
        PathDerivedProduct derived=raw;
        try {
            derived.visualMesh=CropRoadMesh(raw.visualMesh,n,start,end);
            derived.collision=MeshCollision(derived.visualMesh);
            TrimReferences(derived,start,end);
        }
        catch(const std::invalid_argument&) {
            fail(RoadJunctionPublicationIssue::InvalidNavigation,id,
                 "Invalid source geometry or station crop.");
            return result;
        }
        if(derived.visualMesh.vertices.size()>maximumVertices-vertexCount) {
            fail(RoadJunctionPublicationIssue::BudgetExceeded,id,
                 "Retained road mesh exceeds geometry budget.");
            return result;
        }
        vertexCount+=derived.visualMesh.vertices.size();
        u64 edgeSignature=Mix(raw.buildSignature,start);
        edgeSignature=Mix(edgeSignature,end);
        if(cuts.front)edgeSignature=Mix(edgeSignature,
            cuts.front->sourceBuildSignature);
        if(cuts.back)edgeSignature=Mix(edgeSignature,
            cuts.back->sourceBuildSignature);
        derived.buildSignature=edgeSignature;
        signature=Mix(signature,edgeSignature);
        result.packet.edges.push_back({
            id,raw.buildSignature,std::move(derived)
        });
    }
    std::sort(result.packet.edges.begin(),result.packet.edges.end(),
        [](const auto& a,const auto& b){return a.edge<b.edge;});
    result.packet.buildSignature=signature?signature:1U;
    return result;
}

bool RoadJunctionScenePublisher::Publish(RoadJunctionPublicationResult candidate)
{
    if(!candidate.Ready()) {
        findings_=std::move(candidate.findings);
        return false;
    }
    // One move is the atomic CPU generation swap. No partial mutable outputs
    // escape to downstream consumers.
    snapshot_=std::move(candidate.packet);
    findings_.clear();
    return true;
}
const RoadJunctionScenePacket* RoadJunctionScenePublisher::Snapshot() const noexcept
{
    return snapshot_.Valid()?&snapshot_:nullptr;
}
const std::vector<RoadJunctionPublicationFinding>&
RoadJunctionScenePublisher::Findings() const noexcept {return findings_;}
void RoadJunctionScenePublisher::Clear() noexcept
{
    snapshot_={};
    findings_.clear();
}
} // namespace orbit::path_geometry
