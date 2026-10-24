#include <orbit/studio_session/StudioRoadJunctionPipeline.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <chrono>
#include <memory>
#include <orbit/path_geometry/TransportJunctions.hpp>
#include <orbit/path_geometry/RoadJunctionConstruction.hpp>
#include <orbit/path_geometry/RoadCatalog.hpp>
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
    pendingBuild_.reset();
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
const path_geometry::RoadContactSceneOwner*
StudioRoadJunctionPipeline::ActiveContactOwner(
    const scene::ObjectId junction,
    const UniverseBoundPathCache& raw) const noexcept
{
    const auto* snapshot=CurrentContact(junction,raw);
    if(!snapshot)return nullptr;
    const auto it=contactScenes_.find(snapshot->frame);
    if(it==contactScenes_.end())return nullptr;
    return &it->second;
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
std::optional<path_geometry::RoadVehicleFixtureReport>
StudioRoadJunctionPipeline::RunVehicleFixture(
    const scene::ObjectId junction,
    const UniverseBoundPathCache& raw,
    path_geometry::RoadVehicleFixtureRequest request) const
{
    const auto* current=CurrentContact(junction,raw);
    if(!current)return std::nullopt;
    const auto found=contactScenes_.find(current->frame);
    if(found==contactScenes_.end())return std::nullopt;
    request.frame=current->frame;
    return path_geometry::RunRoadVehicleFixture(found->second,request);
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
    if (!world.HasWorld())
    {
        if (!frames_.empty() || observedFingerprint_ || pendingBuild_)
            Clear();
        return;
    }

    const auto products = cache.Products();
    const u64 generation = world.UniverseGeneration();
    u64 fingerprint = Mix(
        Mix(0xcbf29ce484222325ULL, world.Objects().Revision()), generation);
    fingerprint = Mix(fingerprint, cache.BindingGeneration());
    fingerprint = Mix(fingerprint, cache.DirtyCount());
    for (const auto* product : products)
    {
        fingerprint = Mix(fingerprint, product->edge.high);
        fingerprint = Mix(fingerprint, product->edge.low);
        fingerprint = Mix(fingerprint, product->buildSignature);
        fingerprint = Mix(fingerprint, cache.IsDirty(product->edge) ? 1U : 0U);
    }

    // Publish a fully cooked packet + contact scene only at the owner-thread
    // boundary. A newer semantic fingerprint invalidates an obsolete result.
    if (pendingBuild_ && pendingBuild_->future.wait_for(
            std::chrono::seconds(0)) == std::future_status::ready)
    {
        const bool current = pendingBuild_->fingerprint == fingerprint &&
            pendingBuild_->worldGeneration == generation;
        try
        {
            auto completed = pendingBuild_->future.get();
            if (current)
            {
                lastError_ = completed.error;
                for (auto& prepared : completed.junctions)
                {
                    path_geometry::RoadContactSceneOwner contact;
                    if (!contact.Commit(std::move(prepared.cooked)))
                    {
                        lastError_ = contact.LastError();
                        continue;
                    }
                    path_geometry::RoadJunctionScenePublisher publisher;
                    if (!publisher.Publish(std::move(prepared.candidate)))
                    {
                        lastError_ = "Junction packet publication failed.";
                        continue;
                    }
                    const auto frame = prepared.frame;
                    contactScenes_.emplace(frame, std::move(contact));
                    frames_.emplace(frame, std::move(publisher));
                    // Mesh construction/copying already happened on the
                    // worker. Publication only moves complete products.
                    for (auto& product : prepared.renderPatches)
                        renderPatches_.emplace(product.edge, std::move(product));
                }
                ++revision_;
            }
        }
        catch (const std::exception& error)
        {
            if (current) lastError_ = error.what();
        }
        pendingBuild_.reset();
    }

    if (fingerprint == observedFingerprint_)
        return;
    // A previous full junction build is still running. Deferring its
    // replacement prevents a flood of obsolete workers under rapid edits.
    // Consumers already reject packets whose raw corridors are dirty.
    if (pendingBuild_ && pendingBuild_->future.wait_for(
            std::chrono::seconds(0)) != std::future_status::ready)
        return;

    // A stale packet is not suitable for collision or navigation. Invalidating
    // does not block on the old job, and no derived geometry is recalculated
    // inside the session tick.
    frames_.clear();
    contactScenes_.clear();
    renderPatches_.clear();
    lastError_.clear();
    ++revision_;

    // Capture ONLY semantic endpoints and immutable published corridor handles
    // on the owner thread. Workers never touch the mutable scene or cache.
    struct CapturedEdge
    {
        scene::ObjectId id{}, start{}, end{};
        bool rail{false};
    };
    struct BuildInput
    {
        u64 worldGeneration{0U};
        std::vector<CapturedEdge> edges;
        std::map<scene::ObjectId,
            std::shared_ptr<const path_geometry::PathDerivedProduct>> products;
        std::set<scene::ObjectId> dirty;
    };
    BuildInput input;
    input.worldGeneration = generation;
    for (const auto* product : products)
    {
        auto snapshot = cache.Snapshot(product->edge);
        if (snapshot)
            input.products.emplace(product->edge, std::move(snapshot));
        if (cache.IsDirty(product->edge))
            input.dirty.insert(product->edge);
    }

    const auto& objects = world.Objects();
    std::vector<scene::ObjectId> pending;
    for (const auto& root : objects.Roots()) pending.push_back(root.id);
    std::size_t scanned = 0U;
    while (!pending.empty())
    {
        if (++scanned > 8192U)
        {
            lastError_ = "Road junction discovery exceeded 8192 scene objects.";
            observedFingerprint_ = fingerprint;
            pendingBuild_.reset();
            return;
        }
        const auto parent = pending.back();
        pending.pop_back();
        for (const auto& child : objects.Children(parent))
        {
            if (child.type != paths::kPathEdgeType)
            {
                if (child.type != paths::kPathNodeType)
                    pending.push_back(child.id);
                continue;
            }
            const auto edge = network.FindEdge(child.id);
            if (!edge || edge->infrastructureCatalogVersion != 1U ||
                edge->constructionMode != "terrain") continue;
            const bool road = path_geometry::FindRoadCatalog(
                edge->infrastructureCatalogId) != nullptr;
            const bool rail = edge->infrastructureCatalogId == "ballasted-rail";
            if (!road && !rail) continue;
            input.edges.push_back({child.id, edge->startNode, edge->endNode, rail});
        }
    }

    // Do not launch a worker when the world has no road junction sources.
    observedFingerprint_ = fingerprint;
    if (input.edges.empty())
    {
        pendingBuild_.reset();
        return;
    }

    auto future = jobs::SubmitBackgroundCpu(
        [snapshot = std::move(input)]() mutable -> PreparedBatch
        {
            PreparedBatch result;
            std::map<scene::ObjectId,
                std::vector<path_geometry::JunctionApproach>> nodes;
            std::set<scene::ObjectId> railNodes;
            for (const auto& edge : snapshot.edges)
            {
                if (edge.rail)
                {
                    railNodes.insert(edge.start);
                    railNodes.insert(edge.end);
                    continue;
                }
                const auto found = snapshot.products.find(edge.id);
                const auto* geometry = found == snapshot.products.end()
                    ? nullptr : found->second.get();
                nodes[edge.start].push_back({
                    .edge = edge.id,
                    .frame = geometry ? geometry->frame : frames::FrameId{},
                    .mode = path_geometry::TransportMode::Road,
                    .atStart = true, .bidirectional = true,
                    .geometry = geometry});
                nodes[edge.end].push_back({
                    .edge = edge.id,
                    .frame = geometry ? geometry->frame : frames::FrameId{},
                    .mode = path_geometry::TransportMode::Road,
                    .atStart = false, .bidirectional = true,
                    .geometry = geometry});
            }
            std::map<frames::FrameId, FrameBatch> batches;
            std::size_t candidates = 0U;
            for (const auto& [node, approaches] : nodes)
            {
                if (approaches.size() < 2U || railNodes.contains(node))
                    continue;
                if (++candidates > 32U)
                {
                    result.error = "More than 32 junctions; spatial scheduling required.";
                    return result;
                }
                bool waiting = false;
                for (const auto& approach : approaches)
                    if (!approach.geometry || snapshot.dirty.contains(approach.edge))
                        waiting = true;
                if (waiting)
                {
                    result.error = "Road junction waiting for source corridors.";
                    continue;
                }
                const auto plan = path_geometry::BuildTransportJunction(node, approaches);
                if (!plan.Valid())
                {
                    result.error = "Road junction topology validation failed.";
                    continue;
                }
                auto junction = path_geometry::BuildRoadJunctionConstruction(plan, approaches);
                if (!junction.Ready())
                {
                    result.error = junction.findings.empty()
                        ? "Road junction geometry failed."
                        : junction.findings.front().message;
                    continue;
                }
                batches[junction.frame].junctions.push_back(std::move(junction));
            }
            for (auto& [frame, batch] : batches)
            {
                for (const auto& [edge, product] : snapshot.products)
                    if (product->frame == frame)
                    {
                        batch.products.push_back(product.get());
                        if (snapshot.dirty.contains(edge))
                            batch.dirty.push_back(edge);
                    }
                auto candidate = path_geometry::AssembleRoadJunctionScene(
                    frame, snapshot.worldGeneration, batch.junctions,
                    batch.products, batch.dirty);
                if (!candidate.Ready())
                {
                    if (!candidate.findings.empty())
                        result.error = candidate.findings.front().message;
                    continue;
                }
                auto cooked = path_geometry::PrepareRoadContactScene(candidate.packet);
                if (!cooked.Ready())
                {
                    result.error = cooked.error;
                    continue;
                }
                std::vector<path_geometry::PathDerivedProduct> patches;
                patches.reserve(candidate.packet.patches.size());
                for (const auto& patch : candidate.packet.patches)
                {
                    path_geometry::PathDerivedProduct product;
                    product.edge = patch.node;
                    product.frame = patch.frame;
                    product.sourceRevision = candidate.packet.worldGeneration;
                    product.buildSignature = candidate.packet.buildSignature;
                    product.visualMesh = patch.mesh;
                    patches.push_back(std::move(product));
                }
                result.junctions.push_back({frame, std::move(candidate),
                    std::move(cooked), std::move(patches)});
            }
            return result;
        }, jobs::JobPriority::Normal);
    pendingBuild_ = PendingBuild{std::move(future), fingerprint, generation};
}
} // namespace orbit::studio_session
