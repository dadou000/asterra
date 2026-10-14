#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::editor_app
{
// The unified Build > Procedurals panel. It uses existing authored paths and
// engineering RPC; it never creates a second network or directly edits objects.
class StudioProceduralsPanel
{
public:
    static constexpr editor_ui::PanelId kPanelId{
        .high=0x4f5242495450524fULL,.low=0x4345445552414c53ULL};
    StudioProceduralsPanel(studio_session::StudioSession& session,
                           rpc::Dispatcher& rpc) noexcept
        : session_(&session), rpc_(&rpc) {}

    void Register(editor_ui::EditorUi& ui)
    {
        ui.RegisterPanel({
            .id=kPanelId,.title="Procedurals",.defaultOpen=false,
            .defaultDock=editor_ui::DockRegion::Right,.dockOrder=50,
            .minSize={.width=350.0F,.height=280.0F},
            .draw=[this](editor_ui::PanelContext& context){Draw(context);}
        });
    }
private:
    void Refresh()
    {
        edges_.clear();
        nodes_.clear();
        networks_.clear();
        if (!session_->World().HasWorld()) { report_=rpc::Value{}; return; }
        const auto& store=session_->World().Objects();
        auto pending=store.Roots();
        std::size_t visited=0;
        while (!pending.empty() && visited++<20000 && edges_.size()<512)
        {
            const auto record=pending.back();
            pending.pop_back();
            if (record.type==paths::kPathEdgeType) edges_.push_back(record);
            if (record.type==paths::kPathNodeType) nodes_.push_back(record);
            if (record.type==paths::kPathNetworkType) networks_.push_back(record);
            const auto children=store.Children(record.id);
            pending.insert(pending.end(),children.begin(),children.end());
        }
        std::sort(nodes_.begin(),nodes_.end(),
            [](const auto& a,const auto& b){return a.name<b.name;});
        std::sort(networks_.begin(),networks_.end(),
            [](const auto& a,const auto& b){return a.name<b.name;});
        knownWorldGeneration_=session_->World().Objects().Revision();
        std::sort(edges_.begin(),edges_.end(),
            [](const auto& a,const auto& b) {
                if(a.name!=b.name) return a.name<b.name;
                return a.id<b.id;
            });
    }
    void SelectEdge(const scene::ObjectId id)
    {
        edgeId_ = id.ToString();
        selectedFinding_ = 0;
        report_ = rpc::Value{};
        selectedFinding_ = 0;
        error_.clear();
        const auto edge = session_->PathNetwork().Service().FindEdge(id);
        if (!edge) { error_ = "The selected edge no longer exists."; return; }
        catalogId_ = edge->infrastructureCatalogId;
        if (catalogId_ == "ballasted-rail") category_ = 1;
        else if (catalogId_ == "rural-road") category_ = 0;
        catalogVersion_ = edge->infrastructureCatalogVersion;
        constructionMode_ = edge->constructionMode;
        speed_ = edge->designSpeedKmh > 0.0 ? edge->designSpeedKmh/3.6 : 20.0;
        easeTurns_ = edge->easeTurns;
        startHandle_ = edge->startHandleMeters;
        endHandle_ = edge->endHandleMeters;
        mode_ = edge->mode;
    }
    void CreateNetwork()
    {
        try
        {
            if (!session_->World().HasWorld())
                throw std::runtime_error("Open a world first.");
            if (newNetworkName_.empty())
                throw std::invalid_argument("Network name cannot be empty.");
            const auto created=session_->PathNetwork().Service().CreateNetwork(
                newNetworkName_);
            newNetworkId_=created.id;
            status_="Network created. Add anchor nodes through the existing node tools or connect nodes below.";
            error_.clear();
            Refresh();
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void CreateAnchorNode()
    {
        try
        {
            if (!session_->World().HasWorld() || !newNetworkId_)
                throw std::invalid_argument("Create a network in this panel first.");
            const auto frame=frames::FrameId::Parse(newNodeFrame_);
            if (!frame)
                throw std::invalid_argument("Enter an existing reference-frame UUID.");
            if (!std::isfinite(newNodePoint_.x) || !std::isfinite(newNodePoint_.y) ||
                !std::isfinite(newNodePoint_.z))
                throw std::invalid_argument("Node coordinates must be finite.");
            const auto node=session_->PathNetwork().Service().CreateNode(
                newNetworkId_,newNodeName_,
                paths::FramePointAnchor{.frame=*frame,.localMeters=newNodePoint_});
            Refresh();
            if (!startNodeId_) startNodeId_=node.id;
            else endNodeId_=node.id;
            status_="Anchor node created using the selected frame.";
            error_.clear();
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void ConnectNodes()
    {
        try
        {
            if (!session_->World().HasWorld())
                throw std::runtime_error("Open a world first.");
            if (startNodeId_==endNodeId_ || !startNodeId_ || !endNodeId_)
                throw std::invalid_argument("Choose two different path nodes.");
            auto& service=session_->PathNetwork().Service();
            const auto a=service.FindNode(startNodeId_);
            const auto b=service.FindNode(endNodeId_);
            if (!a || !b || a->network != b->network)
                throw std::invalid_argument("Both nodes must exist in the same network.");
            paths::PathEdgeRecord edge{};
            if (newEdgeMode_==1)
                edge=service.ConnectBezier(startNodeId_,endNodeId_,
                    newStartHandle_,newEndHandle_,newEdgeName_);
            else if (newEdgeMode_==2)
                edge=service.ConnectRouted(startNodeId_,endNodeId_,newEdgeName_);
            else
                edge=service.ConnectDirect(startNodeId_,endNodeId_,newEdgeName_);
            // Creation commits supported catalog intent through the normal
            // undoable path service, instead of requiring Advanced editing.
            service.SetInfrastructureIntent(
                edge.id,category_==1 ? "ballasted-rail" : "rural-road",
                1,"terrain",speed_*3.6,easeTurns_);
            Refresh();
            SelectEdge(edge.id);
            status_="Infrastructure edge created with construction intent.";
            error_.clear();
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void Apply()
    {
        try
        {
            const auto id = scene::ObjectId::Parse(edgeId_);
            if (!id || !session_->World().HasWorld())
                throw std::invalid_argument("Select a valid edge in an open world.");
            if (!std::isfinite(speed_) || speed_ < 0.0 || speed_ > 200.0)
                throw std::invalid_argument("Design speed must be between 0 and 200 m/s.");
            if (constructionMode_.empty() || catalogVersion_ < 1)
                throw std::invalid_argument("Construction mode and catalog version are required.");
            auto& service = session_->PathNetwork().Service();
            const auto edge = service.FindEdge(*id);
            if (!edge) throw std::runtime_error("Edge no longer exists.");
            if (catalogId_.empty())
                catalogId_ = category_ == 1 ? "ballasted-rail" : "rural-road";
            service.SetInfrastructureIntent(*id, catalogId_, catalogVersion_,
                constructionMode_, speed_*3.6, easeTurns_);
            if (edge->mode == paths::EdgeMode::Bezier)
                service.SetBezierHandles(*id, startHandle_, endHandle_);
            error_.clear();
            status_ = "Construction and geometry settings applied through undoable authoring commands.";
            report_ = rpc::Value{};
        }
        catch (const std::exception& e) { error_ = e.what(); }
    }
    void SplitSelectedEdge()
    {
        error_.clear();
        if(!scene::ObjectId::Parse(edgeId_))
        {
            error_="Select an existing infrastructure segment first.";
            return;
        }
        try
        {
            rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id",static_cast<i64>(1)},
                {"method","path.edge_split"},
                {"params",rpc::Value(rpc::Value::Object{
                    {"edge",edgeId_},
                    {"fraction",splitFraction_},
                    {"node_name",splitNodeName_}})}
            });
            const auto response=rpc_->Dispatch(rpc::Serialize(request));
            if(!response)throw std::runtime_error("Split RPC returned no response.");
            const auto parsed=rpc::ParseValue(*response);
            if(const auto* err=parsed.Find("error"))
                throw std::runtime_error(rpc::Serialize(*err));
            const auto* payload=parsed.Find("result");
            if(!payload || !payload->IsObject())
                throw std::runtime_error("Malformed split result.");
            const auto* junction=payload->Find("junction_node");
            const auto* second=payload->Find("second_edge");
            if(!junction || !junction->IsString() ||
               !second || !second->IsString())
                throw std::runtime_error("Split returned incomplete identities.");
            status_="Split created junction "+junction->AsString()+
                " and second edge "+second->AsString()+
                ". The original edge ID was preserved. Undo reverts both.";
            junctionNodeId_=junction->AsString();
            junctionReport_=rpc::Value{};
            report_=rpc::Value{};
            Refresh();
            if(const auto original=scene::ObjectId::Parse(edgeId_))
                SelectEdge(*original);
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void Analyze()
    {
        error_.clear();
        report_=rpc::Value{};
        try
        {
            if (!scene::ObjectId::Parse(edgeId_))
                throw std::invalid_argument("Select a valid authored edge.");
            if (!std::isfinite(speed_) || speed_<0.0)
                throw std::invalid_argument("Design speed must be finite and nonnegative.");
            rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id",static_cast<i64>(1)},
                {"method","path.engineering"},
                {"params",rpc::Value(rpc::Value::Object{
                    {"edge",edgeId_},{"design_speed_mps",speed_}})}
            });
            const auto response=rpc_->Dispatch(rpc::Serialize(request));
            if (!response) throw std::runtime_error("No RPC response");
            const auto parsed=rpc::ParseValue(*response);
            if (const auto* e=parsed.Find("error");e)
                throw std::runtime_error(rpc::Serialize(*e));
            const auto* value=parsed.Find("result");
            if (!value || !value->IsObject())
                throw std::runtime_error("Invalid engineering result");
            report_=*value;
            selectedFinding_ = 0U;
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void AnalyzeJunction()
    {
        junctionError_.clear();
        junctionReport_=rpc::Value{};
        try
        {
            const auto node=scene::ObjectId::Parse(junctionNodeId_);
            if(!node)throw std::invalid_argument("Select a valid path node.");
            const rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id",static_cast<i64>(1)},
                {"method","path.junction_preview"},
                {"params",rpc::Value(rpc::Value::Object{
                    {"node",junctionNodeId_},{"include_samples",false},
                    {"include_construction",true}})}
            });
            const auto response=rpc_->Dispatch(rpc::Serialize(request));
            if(!response)throw std::runtime_error("No junction RPC response.");
            const auto parsed=rpc::ParseValue(*response);
            if(const auto* e=parsed.Find("error"))
                throw std::runtime_error(rpc::Serialize(*e));
            const auto* payload=parsed.Find("result");
            if(!payload || !payload->IsObject())
                throw std::runtime_error("Malformed junction result.");
            junctionReport_=*payload;
        }
        catch(const std::exception& ex){junctionError_=ex.what();}
    }
    void Draw(editor_ui::PanelContext& context)
    {
        if (session_->World().HasWorld())
        {
            const auto revision = session_->World().Objects().Revision();
            if (revision != knownWorldGeneration_)
                Refresh();
        }
        else if (!edges_.empty() || !networks_.empty() || !nodes_.empty())
        {
            edges_.clear(); nodes_.clear(); networks_.clear();
            edgeId_.clear(); newNetworkId_ = {};
            report_ = rpc::Value{};
            junctionReport_ = rpc::Value{};
            junctionNodeId_.clear();
            knownWorldGeneration_ = ~u64{0};
        }
        context.Heading("Build / Procedurals");
        context.MutedText("Infrastructure builder · Roads / Railways");
        static constexpr std::array<std::string_view,2> categories{"Roads","Railways"};
        if (context.SegmentedControl("Infrastructure##proc-category",
            categories, category_))
        {
            if (catalogId_.empty() || catalogId_ == "rural-road" ||
                catalogId_ == "ballasted-rail")
                catalogId_ = category_ == 1 ? "ballasted-rail" : "rural-road";
        }
        context.MutedText(category_ == 0
            ? "Road layouts, grades, surface construction and junction-ready splines."
            : "Track alignments, design speed, rail curves and cant diagnostics.");

        if (context.Button("Refresh infrastructure##proc-refresh")) Refresh();
        context.MutedText("Inventory follows authored scene changes automatically.");
        if (context.Section("Create networks and segments##proc-create",false))
        {
            static_cast<void>(context.InputText("Network name##proc-new-network",newNetworkName_));
            if (context.Button("Create network##proc-create-network")) CreateNetwork();
            if (!networks_.empty() && context.Section("Use existing network##proc-networks",true))
            {
                for (const auto& network : networks_)
                {
                    const auto found = session_->PathNetwork().Service().FindNetwork(
                        paths::NetworkId{network.id.high,network.id.low});
                    if (!found) continue;
                    if (context.Selectable(network.name+"##proc-network-"+network.id.ToString(),
                        newNetworkId_ == found->id))
                    {
                        newNetworkId_ = found->id;
                        startNodeId_ = {};
                        endNodeId_ = {};
                    }
                }
            }
            if (newNetworkId_ && context.Section("Add frame-anchored node##proc-node-create",true))
            {
                context.MutedText("Positions are local metres in an existing frame, not global world coordinates.");
                static_cast<void>(context.InputText("Node name##proc-node-name",newNodeName_));
                if (context.Button("Use active body frame##proc-active-frame"))
                {
                    const auto& target = session_->ActiveBody().Active();
                    if (target) { newNodeFrame_ = target->frame.ToString(); error_.clear(); }
                    else error_ = "No active celestial body frame is available.";
                }
                static_cast<void>(context.InputText("Frame UUID##proc-node-frame",newNodeFrame_));
                static_cast<void>(context.InputDouble3("Local position (m)##proc-node-position",newNodePoint_));
                if (context.Button("Create anchor node##proc-node-action"))
                    CreateAnchorNode();
            }
            context.MutedText(std::format("{} networks / {} anchor nodes",
                networks_.size(),nodes_.size()));
            if (context.Section("Connect existing nodes##proc-connect",true))
            {
                for (const auto& node:nodes_)
                {
                    const auto record = session_->PathNetwork().Service().FindNode(node.id);
                    if (!record || (newNetworkId_ && record->network != newNetworkId_))
                        continue;
                    const std::string id=node.id.ToString();
                    if (context.Selectable("Start: "+node.name+"##proc-start-"+id,
                        startNodeId_==node.id))
                        startNodeId_=node.id;
                }
                context.Separator();
                for (const auto& node:nodes_)
                {
                    const auto record = session_->PathNetwork().Service().FindNode(node.id);
                    if (!record || (newNetworkId_ && record->network != newNetworkId_))
                        continue;
                    const std::string id=node.id.ToString();
                    if (context.Selectable("End: "+node.name+"##proc-end-"+id,
                        endNodeId_==node.id))
                        endNodeId_=node.id;
                }
                static constexpr std::array<std::string_view,3> modes{
                    "Direct","Bezier","Terrain routed"};
                static_cast<void>(context.SegmentedControl(
                    "Connection##proc-new-mode",modes,newEdgeMode_));
                static_cast<void>(context.InputText("Segment name##proc-new-edge-name",newEdgeName_));
                if (newEdgeMode_==1)
                {
                    static_cast<void>(context.InputDouble3("Initial handle##proc-new-handle-a",newStartHandle_));
                    static_cast<void>(context.InputDouble3("Final handle##proc-new-handle-b",newEndHandle_));
                }
                if (context.PrimaryButton("Connect selected nodes##proc-connect-action"))
                    ConnectNodes();
            }
        }
        context.SameLine();
        context.MutedText(std::format("{} authored edges", edges_.size()));

        if (context.Section("1. Select a segment##proc-select",true))
        {
            if (edges_.empty())
                context.MutedText("No segments loaded. Use Refresh infrastructure to browse the active world.");
            for (const auto& edge : edges_)
            {
                const auto label = edge.name.empty() ? "Unnamed segment" : edge.name;
                if (context.Selectable(label + "##proc-edge-" + edge.id.ToString(),
                    edgeId_ == edge.id.ToString()))
                    SelectEdge(edge.id);
            }
            if (context.Section("Advanced edge lookup##proc-lookup",false))
                static_cast<void>(context.InputText("Edge UUID##proc-id",edgeId_));
        }

        // Junction inspection stays available before selecting an individual
        // edge and shares the same RPC used by external agents.
        if (context.Section("Junction and turnout preview##proc-junction",false))
        {
            context.MutedText("Exact authored node identity; no proximity guessing or persistent changes.");
            if (context.Section("Choose junction node##proc-junction-nodes",false))
            {
                for (const auto& node:nodes_)
                    if (context.Selectable(node.name+"##proc-junction-"+node.id.ToString(),
                        junctionNodeId_==node.id.ToString()))
                    {
                        junctionNodeId_=node.id.ToString();
                        junctionReport_=rpc::Value{};
                    }
            }
            if (context.Section("Advanced node UUID##proc-junction-id",false))
                static_cast<void>(context.InputText("Node UUID##proc-junction-manual",junctionNodeId_));
            if (context.Button("Analyze junction##proc-junction-run"))
                AnalyzeJunction();
            if(!junctionError_.empty())context.ErrorText(junctionError_);
            if(junctionReport_.IsObject())
            {
                if(const auto* ready=junctionReport_.Find("ready");
                   ready && ready->IsBool() && !ready->AsBool())
                {
                    context.MutedText("Junction unavailable or world closed.");
                }
                else
                {
                    for(const auto* field:{"approaches","movement_count",
                        "road_footprint_triangles"})
                        if(const auto* v=junctionReport_.Find(field);
                           v && v->IsInteger())
                            context.KeyValue(field,std::to_string(v->AsInteger()));
                    if(const auto* built=junctionReport_.Find("construction_ready");
                       built && built->IsBool())
                    {
                        context.KeyValue("Stitched construction preview",
                            built->AsBool()?"Valid CPU product":"Not available");
                        if(built->AsBool())
                        {
                            for(const auto* field:{"construction_patch_vertices",
                                "construction_patch_collision_triangles",
                                "construction_lane_connectors"})
                                if(const auto* n=junctionReport_.Find(field);
                                   n && n->IsInteger())
                                    context.KeyValue(field,
                                        std::to_string(n->AsInteger()));
                            if(const auto* edges=junctionReport_.Find(
                                "construction_trimmed_edges");
                               edges && edges->IsArray() &&
                               context.Section("Cut stations##proc-junction-cuts",false))
                                for(const auto& cut:edges->AsArray())
                                {
                                    if(!cut.IsObject())continue;
                                    const auto* edge=cut.Find("edge");
                                    const auto* dist=cut.Find("cut_station_meters");
                                    const auto* triangles=cut.Find(
                                        "retained_collision_triangles");
                                    if(!edge || !dist || !triangles ||
                                       !edge->IsString() || !dist->IsNumber() ||
                                       !triangles->IsInteger())continue;
                                    context.MutedText(std::format(
                                        "{}: cut {:.2f} m, {} retained collision triangles",
                                        edge->AsString(),dist->AsNumber(),
                                        triangles->AsInteger()));
                                }
                            const auto* cpu=junctionReport_.Find(
                                "construction_published_cpu");
                            context.KeyValue("Production CPU publication",
                                cpu && cpu->IsBool() && cpu->AsBool()?
                                "Published to StudioSession packet":
                                "Preview only");
                            if(const auto* contact=junctionReport_.Find(
                                "construction_contact_scene_active");
                               contact && contact->IsBool())
                                context.KeyValue("Static contact generation",
                                    contact->AsBool()?"Current / CPU active":
                                    "Pending, missing or stale");
                            context.MutedText(
                                "The Vulkan Paths pass consumes the same coherent CPU packet. Static contact is queryable; rigid-body world / vehicle physics is not yet connected.");
                        }
                        if(const auto* errors=junctionReport_.Find(
                            "construction_findings");
                           errors && errors->IsArray() &&
                           context.Section("Construction findings##proc-junction-build-errors",true))
                            for(const auto& item:errors->AsArray())
                            {
                                if(!item.IsObject())continue;
                                const auto* msg=item.Find("message");
                                if(msg && msg->IsString())
                                    context.ErrorText(msg->AsString());
                            }
                    }
                    if(const auto* conflicts=junctionReport_.Find("conflict_count");
                       conflicts && conflicts->IsInteger())
                        context.KeyValue("Conflicting movement pairs",
                            std::to_string(conflicts->AsInteger()));
                    if(const auto* valid=junctionReport_.Find(
                           "advisory_road_phase_valid");
                       valid && valid->IsBool() && valid->AsBool())
                    {
                        if(const auto* phases=junctionReport_.Find(
                               "advisory_road_phase_count");
                           phases && phases->IsInteger())
                            context.KeyValue("Advisory road phases",
                                std::to_string(phases->AsInteger()));
                        context.MutedText(
                            "Conflict-free grouping only; signal timing, priorities and control are not applied.");
                    }
                    if(const auto* truncated=junctionReport_.Find(
                           "conflict_analysis_truncated");
                       truncated && truncated->IsBool() && truncated->AsBool())
                        context.ErrorText(
                            "Conflict scan reached its evaluation budget. Results are incomplete.");
                    if(const auto* f=junctionReport_.Find("frame");
                       f && f->IsString())
                        context.KeyValue("Station frame",f->AsString());
                    if(const auto* r=junctionReport_.Find("rail_requires_explicit_turnout");
                       r && r->IsBool() && r->AsBool())
                        context.MutedText(
                            "Rail branches are candidate connections only. An authored turnout and interlocking are required.");
                    if(const auto* findings=junctionReport_.Find("findings");
                       findings && findings->IsArray())
                    {
                        context.KeyValue("Topology/geometry findings",
                            std::to_string(findings->AsArray().size()));
                        if(context.Section("Junction findings##proc-junction-errors",true))
                            for(const auto& finding:findings->AsArray())
                            {
                                if(!finding.IsObject())continue;
                                const auto* msg=finding.Find("message");
                                if(msg && msg->IsString())context.ErrorText(msg->AsString());
                            }
                    }
                    if(const auto* moves=junctionReport_.Find("movements");
                       moves && moves->IsArray() &&
                       context.Section("Route candidates##proc-junction-moves",false))
                    {
                        for(const auto& move:moves->AsArray())
                        {
                            if(!move.IsObject())continue;
                            const auto* from=move.Find("from_edge");
                            const auto* to=move.Find("to_edge");
                            const auto* distance=move.Find("length_meters");
                            if(from && to && distance && from->IsString() &&
                               to->IsString() && distance->IsNumber())
                                context.MutedText(std::format("{} -> {}: {:.1f} m",
                                    from->AsString(),to->AsString(),
                                    distance->AsNumber()));
                        }
                    }
                }
            }
        }

        if (edgeId_.empty())
        {
            context.MutedText("Select an authored segment to configure it.");
            return;
        }
        const auto selectedObject = scene::ObjectId::Parse(edgeId_);
        if (selectedObject)
        {
            const auto& cache = session_->PathProducts();
            const auto* built = cache.Find(*selectedObject);
            if (built)
            {
                context.KeyValue("Constructed vertices",
                    std::to_string(built->visualMesh.vertices.size()));
                context.KeyValue("Constructed collision triangles",
                    std::to_string(built->collision.size()));
                context.KeyValue("Construction revision",
                    std::to_string(built->buildSignature));
            }
            if (cache.IsDirty(*selectedObject))
                context.MutedText("Construction update pending.");
            const auto failure = cache.RebuildError(*selectedObject);
            if (!failure.empty()) context.ErrorText(
                "Construction blocked: " + failure);
        }
        context.Heading("2. Construction");
        static constexpr std::array<std::string_view,4> methods{
            "Terrain", "Bridge / elevated", "Tunnel", "Supported deck"};
        i32 method = constructionMode_ == "bridge" ? 1 :
            constructionMode_ == "tunnel" ? 2 :
            constructionMode_ == "deck" ? 3 : 0;
        if (context.SegmentedControl("Construction method##proc-method",methods,method))
            constructionMode_ = method == 1 ? "bridge" :
                method == 2 ? "tunnel" : method == 3 ? "deck" : "terrain";
        context.MutedText("Construction intent is stored on the edge; structures require the construction runtime.");
        if (context.Section("Advanced construction##proc-advanced",false))
        {
            static_cast<void>(context.InputText("Catalog ID##proc-catalog",catalogId_));
            static_cast<void>(context.InputInteger("Catalog version##proc-catalog-ver",catalogVersion_));
            static_cast<void>(context.InputText("Construction mode ID##proc-mode-raw",constructionMode_));
        }

        context.Heading("3. Alignment");
        static_cast<void>(context.InputDouble("Design speed (m/s)##proc-speed",speed_));
        static_cast<void>(context.Checkbox("Ease turns##proc-ease",easeTurns_));
        if (mode_ == paths::EdgeMode::Bezier &&
            context.Section("Bezier handles##proc-handles",true))
        {
            static_cast<void>(context.InputDouble3("Start tangent (m)##proc-start", startHandle_));
            static_cast<void>(context.InputDouble3("End tangent (m)##proc-end", endHandle_));
        }
        if (context.PrimaryButton("Apply to authored segment##proc-apply")) Apply();
        if (context.Section("Split infrastructure segment##proc-split",false))
        {
            context.MutedText(
                "Preserves the original curve and first edge ID. Only direct/Bezier endpoints in one reference frame are supported.");
            static_cast<void>(context.InputDouble("Split fraction (0-1)##proc-split-frac",splitFraction_));
            static_cast<void>(context.InputText("New node label##proc-split-name",splitNodeName_));
            if(context.PrimaryButton("Split edge (undoable)##proc-split-run"))
                SplitSelectedEdge();
        }
        if (!status_.empty()) context.MutedText(status_);
        if (!error_.empty()) context.ErrorText(error_);

        context.Heading("4. Engineering preview");
        context.MutedText("Analyze without altering the authored road or railway.");
        if (context.Button("Analyze segment##proc-analyze")) Analyze();
        if (!report_.IsObject()) return;
        const auto* ready = report_.Find("ready");
        if (ready && ready->IsBool() && !ready->AsBool())
        {
            context.MutedText("Derived geometry is not ready. Analyze after routing completes.");
            return;
        }
        if (const auto* n=report_.Find("station_count");n && n->IsInteger())
            context.KeyValue("Sample stations",std::to_string(n->AsInteger()));
        if (const auto* sig=report_.Find("signature");sig && sig->IsInteger())
            context.KeyValue("Geometry revision",std::to_string(sig->AsInteger()));
        const auto* warnings=report_.Find("warnings");
        if (warnings && warnings->IsArray())
        {
            context.KeyValue("Engineering warnings",std::to_string(warnings->AsArray().size()));
            if (context.Section("Warnings by station##proc-findings",true))
                for (std::size_t i=0;i<warnings->AsArray().size();++i)
                {
                    const auto& finding=warnings->AsArray()[i];
                    if (!finding.IsObject())continue;
                    const auto* text=finding.Find("message");
                    const auto* at=finding.Find("station_meters");
                    if (!text || !text->IsString())continue;
                    const double station=at && at->IsNumber()?at->AsNumber():0.0;
                    if (context.Selectable(std::format(
                        "{:.1f} m | {}##proc-finding-{}",station,text->AsString(),i),
                        selectedFinding_ == i))
                        selectedFinding_=i;
                }
            if (selectedFinding_<warnings->AsArray().size())
            {
                const auto& finding=warnings->AsArray()[selectedFinding_];
                if (const auto* measured=finding.Find("measured");
                    measured && measured->IsNumber())
                    context.KeyValue("Measured",std::format("{:.4f}",measured->AsNumber()));
                if (const auto* permitted=finding.Find("permitted");
                    permitted && permitted->IsNumber())
                    context.KeyValue("Limit",std::format("{:.4f}",permitted->AsNumber()));
                if (const auto* frame=finding.Find("position_frame");
                    frame && frame->IsString())
                    context.KeyValue("Reference frame",frame->AsString());
            }
        }
        if (const auto* built=report_.Find("construction_ready");
            built && built->IsBool() && !built->AsBool())
            context.MutedText("Engineering is using a preview; physical construction is not ready.");
        if (const auto* banks=report_.Find("suggested_bank_radians");
            banks && banks->IsArray())
            context.KeyValue("Generic bank suggestions",
                std::to_string(banks->AsArray().size()));
        if (const auto* cant=report_.Find("rail_cant_meters");
            cant && cant->IsArray())
            context.KeyValue("Rail cant samples (metres)",
                std::to_string(cant->AsArray().size()));
        if (const auto* issues=report_.Find("rail_cant_warnings");
            issues && issues->IsArray())
        {
            context.KeyValue("Rail cant warnings",
                std::to_string(issues->AsArray().size()));
            if (context.Section("Rail cant issues##proc-cant-issues",false))
            {
                for (const auto& issue:issues->AsArray())
                {
                    if (!issue.IsObject()) continue;
                    const auto* station=issue.Find("station_meters");
                    const auto* measured=issue.Find("measured_meters");
                    const auto* permitted=issue.Find("permitted_meters");
                    if (!station || !measured || !permitted ||
                        !station->IsNumber() || !measured->IsNumber() ||
                        !permitted->IsNumber()) continue;
                    context.MutedText(std::format(
                        "{:.1f} m: {:.3f} m vs {:.3f} m permitted",
                        station->AsNumber(),measured->AsNumber(),
                        permitted->AsNumber()));
                }
            }
        }
        context.MutedText("Engineering is advisory; rail cant is generated only when turn easing is enabled.");
    }
    studio_session::StudioSession* session_;
    rpc::Dispatcher* rpc_;
    u64 knownWorldGeneration_{~u64{0}};
    std::vector<scene::ObjectRecord> edges_;
    std::vector<scene::ObjectRecord> nodes_;
    std::vector<scene::ObjectRecord> networks_;
    paths::NetworkId newNetworkId_{};
    std::string newNodeName_{"Control Point"};
    std::string newNodeFrame_;
    math::Double3 newNodePoint_{};
    std::string newNetworkName_{"New Infrastructure"};
    scene::ObjectId startNodeId_{};
    scene::ObjectId endNodeId_{};
    i32 newEdgeMode_{1};
    std::string newEdgeName_{"New Segment"};
    math::Double3 newStartHandle_{5.0,0.0,0.0};
    math::Double3 newEndHandle_{-5.0,0.0,0.0};
    std::string edgeId_;
    double speed_{20.0};
    double splitFraction_{0.5};
    std::string splitNodeName_{"Split Junction"};
    std::string error_;
    rpc::Value report_;
    rpc::Value junctionReport_;
    std::string junctionNodeId_;
    std::string junctionError_;
    std::size_t selectedFinding_{0};
    i32 category_{0};
    std::string catalogId_;
    i64 catalogVersion_{1};
    std::string constructionMode_{"terrain"};
    std::string status_;
    bool easeTurns_{true};
    paths::EdgeMode mode_{paths::EdgeMode::Direct};
    math::Double3 startHandle_{};
    math::Double3 endHandle_{};
};
} // namespace orbit::editor_app
