#include <orbit/studio_session/StudioRoadJunctionPipeline.hpp>
#include <orbit/path_geometry/TransportJunctions.hpp>
#include <orbit/path_geometry/RoadJunctionConstruction.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace orbit::studio_session {
namespace {
u64 Mix(u64 h,u64 v) {
    return (h^v)*0x100000001b3ULL;
}
struct FrameBatch {
    std::vector<path_geometry::RoadJunctionConstruction> junctions;
    std::vector<const path_geometry::PathDerivedProduct*> products;
    std::vector<scene::ObjectId> dirty;
};
}
void StudioRoadJunctionPipeline::Clear() noexcept {
    frames_.clear();
    contactScenes_.clear();
    renderPatches_.clear();
    observedFingerprint_=0;
    lastError_.clear();
    ++revision_;
}
std::size_t StudioRoadJunctionPipeline::ActiveJunctions() const noexcept {
    std::size_t count=0;
    for(const auto& [id,publisher]:frames_)
        if(const auto* p=publisher.Snapshot())count+=p->patches.size();
    return count;
}
std::vector<const path_geometry::RoadJunctionScenePacket*>
StudioRoadJunctionPipeline::Packets() const {
    std::vector<const path_geometry::RoadJunctionScenePacket*> result;
    for(const auto& [frame,publisher]:frames_)
        if(const auto* packet=publisher.Snapshot())result.push_back(packet);
    return result;
}
const path_geometry::RoadJunctionPublishedEdge*
StudioRoadJunctionPipeline::FindEdge(scene::ObjectId edge) const noexcept {
    for(const auto& [frame,publisher]:frames_)
        if(const auto* packet=publisher.Snapshot())
            for(const auto& product:packet->edges)
                if(product.edge==edge)return &product;
    return nullptr;
}
const path_geometry::RoadJunctionPatchProduct*
StudioRoadJunctionPipeline::FindPatch(scene::ObjectId node) const noexcept {
    for(const auto& [frame,publisher]:frames_)
        if(const auto* packet=publisher.Snapshot())
            for(const auto& patch:packet->patches)
                if(patch.node==node)return &patch;
    return nullptr;
}
bool StudioRoadJunctionPipeline::PacketCurrent(
    const path_geometry::RoadJunctionScenePacket& packet,
    const UniverseBoundPathCache& raw) const noexcept
{
    if(packet.worldGeneration!=raw.ObservedUniverseGeneration())return false;
    const auto contact=contactScenes_.find(packet.frame);
    if(contact==contactScenes_.end())return false;
    const auto* scene=contact->second.Snapshot();
    if(!scene || scene->worldGeneration!=packet.worldGeneration ||
       scene->packetSignature!=packet.buildSignature ||
       scene->frame!=packet.frame)return false;
    for(const auto& edge:packet.edges)
    {
        const auto* source=raw.Find(edge.edge);
        if(!source || raw.IsDirty(edge.edge) ||
           source->frame!=packet.frame ||
           source->buildSignature!=edge.rawBuildSignature)
            return false;
    }
    return true;
}
std::size_t StudioRoadJunctionPipeline::ActiveContactScenes() const noexcept
{
    return contactScenes_.size();
}
const path_geometry::RoadContactSceneGeneration*
StudioRoadJunctionPipeline::CurrentContact(
    const scene::ObjectId junction,
    const UniverseBoundPathCache& raw) const noexcept
{
    for(const auto& [frame,publisher]:frames_)
    {
        const auto* packet=publisher.Snapshot();
        if(!packet || !PacketCurrent(*packet,raw))continue;
        const bool found=std::any_of(
            packet->patches.begin(),packet->patches.end(),
            [&](const auto& patch){return patch.node==junction;});
        if(!found)continue;
        const auto contact=contactScenes_.find(frame);
        return contact==contactScenes_.end()?nullptr:
            contact->second.Snapshot();
    }
    return nullptr;
}
std::optional<path_geometry::RoadContactHit>
StudioRoadJunctionPipeline::ProbeContact(
    const scene::ObjectId junction,
    const UniverseBoundPathCache& raw,
    path_geometry::RoadContactProbe probe) const
{
    const auto* current=CurrentContact(junction,raw);
    if(!current)return std::nullopt;
    const auto found=contactScenes_.find(current->frame);
    if(found==contactScenes_.end())return std::nullopt;
    probe.frame=current->frame;
    return found->second.Probe(probe);
}
std::vector<const path_geometry::PathDerivedProduct*>
StudioRoadJunctionPipeline::RenderProducts(
    const UniverseBoundPathCache& raw) const
{
    const auto sources=raw.Products();
    std::map<scene::ObjectId,const path_geometry::PathDerivedProduct*> replaced;
    std::vector<const path_geometry::PathDerivedProduct*> patches;
    for(const auto& [frame,publisher]:frames_)
    {
        const auto* packet=publisher.Snapshot();
        if(!packet || !PacketCurrent(*packet,raw))
            continue; // No visual replacement without coherent contact data.
        for(const auto& edge:packet->edges)
            replaced.emplace(edge.edge,&edge.product);
        for(const auto& patch:packet->patches)
        {
            const auto it=renderPatches_.find(patch.node);
            if(it!=renderPatches_.end() && it->second.frame==frame &&
               it->second.buildSignature==packet->buildSignature)
                patches.push_back(&it->second);
        }
    }
    std::vector<const path_geometry::PathDerivedProduct*> render;
    render.reserve(sources.size()+patches.size());
    for(const auto* product:sources)
    {
        const auto it=replaced.find(product->edge);
        render.push_back(it==replaced.end()?product:it->second);
    }
    render.insert(render.end(),patches.begin(),patches.end());
    return render;
}

void StudioRoadJunctionPipeline::Refresh(
    editor_session::EditorWorldSession& world,
    paths::PathNetworkService& network,
    const UniverseBoundPathCache& cache)
{
    if(!world.HasWorld()) {
        if(!frames_.empty() || observedFingerprint_)Clear();
        return;
    }
    const auto products=cache.Products();
    const auto generation=world.UniverseGeneration();
    u64 fingerprint=Mix(
        Mix(0xcbf29ce484222325ULL,world.Objects().Revision()),generation);
    fingerprint=Mix(fingerprint,cache.BindingGeneration());
    fingerprint=Mix(fingerprint,cache.DirtyCount());
    for(const auto* p:products) {
        fingerprint=Mix(fingerprint,p->edge.high);
        fingerprint=Mix(fingerprint,p->edge.low);
        fingerprint=Mix(fingerprint,p->buildSignature);
        fingerprint=Mix(fingerprint,cache.IsDirty(p->edge)?1U:0U);
    }
    if(fingerprint==observedFingerprint_)return;
    observedFingerprint_=fingerprint;
    frames_.clear();
    renderPatches_.clear();
    lastError_.clear();
    ++revision_;

    // Discover semantic endpoints once, not O(nodes * allEdges), and never
    // connect paths by spatial proximity or frame coincidence.
    std::map<scene::ObjectId,std::vector<path_geometry::JunctionApproach>> nodes;
    std::set<scene::ObjectId> hasRail;
    const auto& objects=world.Objects();
    std::vector<scene::ObjectId> pending;
    for(const auto& root:objects.Roots())pending.push_back(root.id);
    std::size_t scanned=0U;
    while(!pending.empty()) {
        if(++scanned>8192U) {
            lastError_="Road junction discovery exceeded 8192 scene objects.";
            return;
        }
        const auto parent=pending.back();
        pending.pop_back();
        for(const auto& child:objects.Children(parent))
        {
            if(child.type!=paths::kPathEdgeType) {
                if(child.type!=paths::kPathNodeType)pending.push_back(child.id);
                continue;
            }
            const auto edge=network.FindEdge(child.id);
            if(!edge || edge->infrastructureCatalogVersion!=1U ||
               edge->constructionMode!="terrain")continue;
            const bool road=edge->infrastructureCatalogId=="rural-road";
            const bool rail=edge->infrastructureCatalogId=="ballasted-rail";
            if(!road && !rail)continue;
            if(rail) {
                hasRail.insert(edge->startNode);
                hasRail.insert(edge->endNode);
                continue;
            }
            const auto* geom=cache.Find(child.id);
            const auto add=[&](scene::ObjectId node,bool atStart) {
                nodes[node].push_back({
                    .edge=child.id,
                    .frame=geom?geom->frame:frames::FrameId{},
                    .mode=path_geometry::TransportMode::Road,
                    .atStart=atStart,.bidirectional=true,.geometry=geom
                });
            };
            add(edge->startNode,true);
            add(edge->endNode,false);
        }
    }
    std::map<frames::FrameId,FrameBatch> batches;
    std::size_t candidates=0;
    for(const auto& [node,approaches]:nodes)
    {
        if(approaches.size()<2 || hasRail.contains(node))continue;
        if(++candidates>32U) {
            frames_.clear();
            lastError_="More than 32 junctions; spatial scheduling required.";
            return;
        }
        bool pendingSource=false;
        for(const auto& a:approaches)
            if(!a.geometry || cache.IsDirty(a.edge))pendingSource=true;
        if(pendingSource) {
            lastError_="Road junction waiting for all incident source corridors.";
            continue;
        }
        const auto plan=path_geometry::BuildTransportJunction(node,approaches);
        if(!plan.Valid()) {
            lastError_="Road junction geometry failed its topology validation.";
            continue;
        }
        auto built=path_geometry::BuildRoadJunctionConstruction(plan,approaches);
        if(!built.Ready()) {
            lastError_=built.findings.empty()?
                "Unsupported road junction geometry.":
                built.findings.front().message;
            continue;
        }
        batches[built.frame].junctions.push_back(std::move(built));
    }
    for(auto& [frame,batch]:batches)
    {
        // All products of this frame are source candidates. Publishing them
        // never mutates the underlying path cache.
        for(const auto* p:products)
            if(p->frame==frame) {
                batch.products.push_back(p);
                if(cache.IsDirty(p->edge))batch.dirty.push_back(p->edge);
            }
        auto candidate=path_geometry::AssembleRoadJunctionScene(
            frame,generation,batch.junctions,batch.products,batch.dirty);
        if(candidate.Ready()) {
            // Cook/validate contact shapes in a staging generation before
            // publishing any render packet. Both become active together.
            auto cooked=path_geometry::PrepareRoadContactScene(
                candidate.packet);
            if(!cooked.Ready())
            {
                lastError_=cooked.error;
                continue;
            }
            path_geometry::RoadContactSceneOwner contact;
            if(!contact.Commit(std::move(cooked)))
            {
                lastError_=contact.LastError();
                continue;
            }
            path_geometry::RoadJunctionScenePublisher publisher;
            if(publisher.Publish(std::move(candidate)))
            {
                contactScenes_.emplace(frame,std::move(contact));
                frames_.emplace(frame,std::move(publisher));
                if(const auto* packet=frames_.at(frame).Snapshot())
                    for(const auto& patch:packet->patches)
                    {
                        path_geometry::PathDerivedProduct product;
                        product.edge=patch.node;
                        product.frame=patch.frame;
                        product.sourceRevision=packet->worldGeneration;
                        product.buildSignature=packet->buildSignature;
                        product.visualMesh=patch.mesh;
                        renderPatches_.emplace(patch.node,std::move(product));
                    }
            }
        }
        else if(!candidate.findings.empty())
            lastError_=candidate.findings.front().message;
    }
}
} // namespace orbit::studio_session
