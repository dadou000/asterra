#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include "StudioPanels.hpp"
#include <orbit/path_geometry/RoadCatalog.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <set>
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

    void BindViewportTool(StudioViewportPanel& viewport) noexcept
    {
        viewportTool_=&viewport;
        UpdateViewportTool();
    }
    void OpenPathsAndRoads() noexcept
    {
        page_=1;
        if(category_==0 && catalogId_.empty())catalogId_="rural-road";
        UpdateViewportTool();
    }
    void OpenOverview() noexcept {page_=0;}
    void OpenAdvanced() noexcept {page_=2;}
    [[nodiscard]] bool IsRoadBuilderOpen() const noexcept {return page_==1;}

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
        // Never keep deleted edge IDs selected for a future batch edit.
        for(auto it=selectedRoads_.begin();it!=selectedRoads_.end();)
            if(!session_->PathNetwork().Service().FindEdge(*it))
                it=selectedRoads_.erase(it);
            else ++it;
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
        else if (path_geometry::FindRoadCatalog(catalogId_)) category_ = 0;
        else category_=2;
        catalogVersion_ = edge->infrastructureCatalogVersion;
        constructionMode_ = edge->constructionMode;
        speed_ = edge->designSpeedKmh > 0.0 ? edge->designSpeedKmh/3.6 : 20.0;
        designSpeedKmh_=speed_*3.6;
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
            if(category_!=2)
                service.SetInfrastructureIntent(
                    edge.id,catalogId_.empty()?
                        (category_==1?"ballasted-rail":"rural-road"):
                        catalogId_,
                    1,constructionMode_,speed_*3.6,easeTurns_);
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
    void RunRoadVehicleWorldAction(std::string_view action)
    {
        runtimeVehicleError_.clear();
        try
        {
            rpc::Value::Object args{{"action",std::string(action)}};
            if(action=="spawn" || action=="step" || action=="follow")
            {
                if(!scene::ObjectId::Parse(junctionNodeId_))
                    throw std::invalid_argument("Choose a junction node.");
                args.emplace("node",junctionNodeId_);
            }
            if(action=="step")
                args.emplace("steps",static_cast<i64>(60));
            if(action=="controls")
            {
                if(runtimeVehicleId_.empty())
                    throw std::invalid_argument("Spawn a car first.");
                args.emplace("vehicle",runtimeVehicleId_);
                args.emplace("throttle",runtimeThrottle_);
                args.emplace("brake",runtimeBrake_);
                args.emplace("steer_radians",runtimeSteer_);
            }
            if(action=="remove" || action=="status")
                if(!runtimeVehicleId_.empty())
                    args.emplace("vehicle",runtimeVehicleId_);
            const rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id",static_cast<i64>(1)},
                {"method","infrastructure.vehicle_world"},
                {"params",rpc::Value(std::move(args))}
            });
            const auto reply=rpc_->Dispatch(rpc::Serialize(request));
            if(!reply)throw std::runtime_error("No runtime vehicle RPC response.");
            const auto parsed=rpc::ParseValue(*reply);
            if(const auto* err=parsed.Find("error"))
                throw std::runtime_error(rpc::Serialize(*err));
            const auto* result=parsed.Find("result");
            if(!result || !result->IsObject())
                throw std::runtime_error("Malformed runtime vehicle reply.");
            runtimeVehicleReport_=*result;
            if(action=="spawn")
            {
                if(const auto* id=result->Find("vehicle");id && id->IsString())
                    runtimeVehicleId_=id->AsString();
            }
            if(action=="remove" || action=="clear")
                runtimeVehicleId_.clear();
        }
        catch(const std::exception& ex){runtimeVehicleError_=ex.what();}
    }
    void RunJunctionDriveFixture()
    {
        driveError_.clear();
        driveReport_=rpc::Value{};
        try
        {
            if(!scene::ObjectId::Parse(junctionNodeId_))
                throw std::invalid_argument("Select a junction node first.");
            const rpc::Value request(rpc::Value::Object{
                {"jsonrpc","2.0"},{"id",static_cast<i64>(1)},
                {"method","infrastructure.road_vehicle_fixture"},
                {"params",rpc::Value::Object{
                    {"node",junctionNodeId_},
                    {"steps",static_cast<i64>(480)}
                }}
            });
            const auto reply=rpc_->Dispatch(rpc::Serialize(request));
            if(!reply)throw std::runtime_error("No vehicle fixture RPC reply.");
            const auto parsed=rpc::ParseValue(*reply);
            if(const auto* err=parsed.Find("error"))
                throw std::runtime_error(rpc::Serialize(*err));
            const auto* result=parsed.Find("result");
            if(!result || !result->IsObject())
                throw std::runtime_error("Vehicle fixture response malformed.");
            driveReport_=*result;
        }
        catch(const std::exception& e){driveError_=e.what();}
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
    [[nodiscard]] bool RoadMatchesFilter(
        const scene::ObjectRecord& record) const
    {
        const auto road=session_->PathNetwork().Service().FindEdge(record.id);
        if(!road || !path_geometry::FindRoadCatalog(
            road->infrastructureCatalogId))return false;
        if(roadInventoryFilter_.empty())return true;
        auto normalize=[](std::string value)
        {
            std::transform(value.begin(),value.end(),value.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            return value;
        };
        const std::string needle=normalize(roadInventoryFilter_);
        const auto* catalog=path_geometry::FindRoadCatalog(
            road->infrastructureCatalogId);
        return normalize(record.name).find(needle)!=std::string::npos ||
            normalize(std::string(catalog->label)).find(needle)!=
                std::string::npos ||
            normalize(std::string(catalog->id)).find(needle)!=
                std::string::npos;
    }
    void UpgradeSelectedRoads()
    {
        error_.clear();
        if(category_!=0 || !path_geometry::FindRoadCatalog(catalogId_))
        {
            error_="Choose a valid paved road profile first.";
            return;
        }
        if(!session_->World().HasWorld())
        {
            error_="Open a world before upgrading infrastructure.";
            return;
        }
        auto& commands=session_->World().Commands();
        if(commands.HasActiveTransaction())
        {
            error_="Finish the current authored operation before batch upgrading.";
            return;
        }
        auto& service=session_->PathNetwork().Service();
        std::vector<scene::ObjectId> eligible;
        eligible.reserve(selectedRoads_.size());
        for(const auto& id:selectedRoads_)
        {
            const auto road=service.FindEdge(id);
            if(!road || !path_geometry::FindRoadCatalog(
                road->infrastructureCatalogId))
                continue;
            if(newNetworkId_ && upgradeOnlyActiveNetwork_ &&
               road->network!=newNetworkId_)
                continue;
            if(road->infrastructureCatalogId!=catalogId_ ||
               road->designSpeedKmh!=designSpeedKmh_ ||
               road->easeTurns!=easeTurns_)
                eligible.push_back(id);
        }
        if(eligible.empty())
        {
            status_="No selected roads require a profile change.";
            return;
        }
        try
        {
            commands.BeginTransaction("Upgrade Road Sections");
            for(const auto& id:eligible)
            {
                const auto road=service.FindEdge(id);
                if(!road)
                    throw std::runtime_error(
                        "A selected segment vanished during batch upgrade.");
                // Preserve construction mode, alignment, incident node IDs,
                // junction authority and segment identity.
                service.SetInfrastructureIntent(id,catalogId_,1,
                    road->constructionMode,designSpeedKmh_,easeTurns_);
            }
            commands.CommitTransaction();
            status_=std::format(
                "Upgraded {} road segments as one undoable edit.",
                eligible.size());
            selectedRoads_.clear();
            if(viewportTool_)viewportTool_->lastPlacedPathNode.reset();
            Refresh();
        }
        catch(const std::exception& e)
        {
            if(commands.HasActiveTransaction())
                commands.RollbackTransaction();
            error_=e.what();
        }
    }
    void UpgradeRoadSection()
    {
        error_.clear();
        try
        {
            if(!session_->World().HasWorld())
                throw std::runtime_error("Open a world first.");
            const auto id=scene::ObjectId::Parse(upgradeTargetEdgeId_);
            if(!id)throw std::invalid_argument(
                "Select a road segment to upgrade.");
            const auto* selected=path_geometry::FindRoadCatalog(catalogId_);
            if(category_!=0 || !selected)
                throw std::invalid_argument(
                    "Choose a paved road profile for the replacement.");
            auto& service=session_->PathNetwork().Service();
            const auto current=service.FindEdge(*id);
            if(!current)
                throw std::invalid_argument(
                    "The road no longer exists; refresh the panel.");
            if(!path_geometry::FindRoadCatalog(
                current->infrastructureCatalogId))
                throw std::invalid_argument(
                    "The selected segment is not a constructed road.");
            // Authored alignment, incident junction identity and
            // construction level are preserved; only section and design
            // metadata change through CommandService.
            service.SetInfrastructureIntent(
                *id,std::string(selected->id),1,
                current->constructionMode,designSpeedKmh_,easeTurns_);
            status_=std::format("Upgraded segment to {} ({} lanes, {:.1f} m). Undo is available.",
                selected->label,selected->laneCount,
                selected->pavedWidthMeters);
            if(viewportTool_)
                viewportTool_->infrastructureLastAction=status_;
            Refresh();
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void UpdateViewportTool() noexcept
    {
        if(!viewportTool_)return;
        viewportTool_->infrastructureCatalogId=
            category_==2?"":catalogId_.empty()?
                (category_==1?"ballasted-rail":"rural-road"):catalogId_;
        viewportTool_->infrastructureAlignmentMode=newEdgeMode_;
        viewportTool_->infrastructureConstructionMode=constructionMode_;
        viewportTool_->infrastructureDesignSpeedKmh=
            std::clamp(speed_*3.6,0.0,720.0);
        viewportTool_->infrastructureEaseTurns=easeTurns_;
        viewportTool_->infrastructureCurveBias=curveBias_;
        viewportTool_->infrastructureSnapNodes=snapToNodes_;
        viewportTool_->infrastructureNodeSnapMeters=nodeSnapMeters_;
        viewportTool_->infrastructureElevationMeters=elevationMeters_;
        if(newNetworkId_)viewportTool_->activePathNetwork=newNetworkId_;
    }
    void ToggleDraw(bool enable)
    {
        if(!viewportTool_)
        {
            error_="The viewport authoring tool is not attached.";
            return;
        }
        if(enable && !session_->World().HasWorld())
        {
            error_="Open a world before drawing infrastructure.";
            return;
        }
        UpdateViewportTool();
        viewportTool_->pathPlacementMode=enable;
        viewportTool_->infrastructureDrawMode=enable;
        if(!enable)viewportTool_->lastPlacedPathNode.reset();
        error_.clear();
        status_=enable?
            "Construction tool armed. Click to place the start point, then click again to build a segment; continue clicking to extend.":
            "Construction mode finished.";
    }
    void DrawBuilder(editor_ui::PanelContext& context)
    {
        // Viewport shortcuts and numeric panel controls share one mutable
        // presentation state, never a second authored source.
        if(viewportTool_ && viewportTool_->infrastructureDrawMode)
            elevationMeters_=viewportTool_->infrastructureElevationMeters;
        context.Heading("Procedural Paths & Roads");
        context.MutedText(
            "Construct connected road, railway or reference-path networks "
            "directly in the viewport.");
        if(context.Button("Procedurals home##road-home"))
        {
            if(viewportTool_ && viewportTool_->infrastructureDrawMode)
                ToggleDraw(false);
            page_=0;
            return;
        }
        context.SameLine();
        if(context.Button("Expert editor##road-expert"))
        {
            page_=2;
            return;
        }
        const bool drawing=viewportTool_ &&
            viewportTool_->pathPlacementMode &&
            viewportTool_->infrastructureDrawMode;
        if(context.PrimaryButton(drawing?
            "Finish construction  [Esc]##roads-stop":
            "Start building in viewport##roads-start"))
            ToggleDraw(!drawing);
        // A city-builder construction palette keeps the ordinary editor
        // undo/redo stack within reach; no private geometry history.
        if(session_->World().HasWorld())
        {
            auto& commands=session_->World().Commands();
            const auto undoLabels=commands.UndoLabels(1U);
            if(context.Button("Undo last edit##road-undo"))
            {
                try
                {
                    if(!commands.CanUndo())
                        throw std::runtime_error("There is no authored edit to undo.");
                    commands.Undo();
                    if(viewportTool_)viewportTool_->lastPlacedPathNode.reset();
                    status_="Undo complete. Re-select an endpoint to resume drawing.";
                    error_.clear();
                    Refresh();
                }
                catch(const std::exception& e){error_=e.what();}
            }
            context.SameLine();
            if(context.Button("Redo##road-redo"))
            {
                try
                {
                    if(!commands.CanRedo())
                        throw std::runtime_error("There is no authored edit to redo.");
                    commands.Redo();
                    if(viewportTool_)viewportTool_->lastPlacedPathNode.reset();
                    status_="Redo complete. Select an endpoint to continue.";
                    error_.clear();
                    Refresh();
                }
                catch(const std::exception& e){error_=e.what();}
            }
            if(!undoLabels.empty())
                context.MutedText("Undo available: "+undoLabels.front());
        }
        if(drawing)
        {
            context.KeyValue("Tool","Drawing - successive clicks extend the network");
            context.MutedText(
                "Left click: add road | Right click: end chain | Escape: exit tool");
            if(context.Button("New independent road chain##road-new-chain"))
            {
                viewportTool_->lastPlacedPathNode.reset();
                viewportTool_->infrastructureLastAction=
                    "Next viewport click starts an independent road chain.";
            }
            if(viewportTool_->infrastructureHoverValid)
            {
                context.KeyValue("Ghost segment length",
                    std::format("{:.1f} m",
                        viewportTool_->infrastructurePreviewLengthMeters));
                context.KeyValue("Target",
                    viewportTool_->infrastructurePreviewSnapped ?
                        "Existing authored network node" :
                        "New reference-surface point");
            }
            if(!viewportTool_->infrastructurePlacementError.empty())
                context.ErrorText(
                    viewportTool_->infrastructurePlacementError);
            if(!viewportTool_->infrastructureLastAction.empty())
                context.MutedText(
                    viewportTool_->infrastructureLastAction);
            context.KeyValue("Created this session",
                std::to_string(viewportTool_->infrastructureBuiltEdges));
        }
        else
            context.MutedText("Choose a road type and click Start. No wizard or separate tool window.");

        context.Separator();
        static constexpr std::array<std::string_view,3> types{
            "Roads","Railways","Paths"};
        if(context.SegmentedControl("Network kind##road-category",types,category_))
        {
            catalogId_=category_==0?"rural-road":
                category_==1?"ballasted-rail":"";
            designSpeedKmh_=category_==1?85.0:category_==0?60.0:20.0;
            speed_=designSpeedKmh_/3.6;
            newNetworkName_=category_==0?"Road Network":
                category_==1?"Rail Network":"Path Network";
            if(viewportTool_)
                viewportTool_->lastPlacedPathNode.reset();
            UpdateViewportTool();
        }
        context.Heading("Choose a section");
        if(category_==0)
        {
            for(const auto& preset:path_geometry::kRoadCatalog)
            {
                const auto text=std::format(
                    "{}  -  {} lane{} / {:.1f} m##roads-preset-{}",
                    preset.label,preset.laneCount,
                    preset.laneCount==1?"":"s",preset.pavedWidthMeters,
                    preset.id);
                if(context.SelectableWithIcon(text,
                    catalogId_==preset.id,
                    editor_ui::ToolbarIcon::Procedural))
                {
                    catalogId_=std::string(preset.id);
                    designSpeedKmh_=preset.designSpeedKmh;
                    speed_=designSpeedKmh_/3.6;
                    UpdateViewportTool();
                }
            }
        }
        else if(category_==1)
        {
            if(context.SelectableWithIcon(
                "Single ballasted track - 1.435 m gauge##roads-rail",
                catalogId_=="ballasted-rail",
                editor_ui::ToolbarIcon::Procedural))
            {
                catalogId_="ballasted-rail";
                designSpeedKmh_=85.0;
                speed_=designSpeedKmh_/3.6;
                UpdateViewportTool();
            }
            context.MutedText("Multiple tracks and physical turnouts are planned.");
        }
        else
        {
            if(context.SelectableWithIcon(
                "Reference spline - no constructed surface##roads-path",
                catalogId_.empty(),
                editor_ui::ToolbarIcon::Procedural))
            {
                catalogId_.clear();
                UpdateViewportTool();
            }
            context.MutedText("Reference paths create authored nodes and edges only.");
        }

        // A single compact native section diagram reflects the ACTUAL
        // selected lane count/width instead of a hard-coded 2-lane picture.
        const auto available=context.ContentAvailable();
        const float diagramWidth=std::max(145.0F,available.width-12.0F);
        const bool childVisible=context.BeginChild(
            "roads-selected-section",
            {.width=0.0F,.height=104.0F},true);
        if(childVisible)
        {
            static_cast<void>(context.Canvas(
                "roads-native-section-preview",
                {.width=diagramWidth,.height=85.0F}));
            const math::Float4 grey{0.31F,0.36F,0.41F,1.0F};
            const math::Float4 marking{0.92F,0.83F,0.56F,1.0F};
            const math::Float4 ground{0.19F,0.27F,0.23F,1.0F};
            context.CanvasRect({0.0F,0.71F},{1.0F,0.99F},ground,true);
            const auto* profile=path_geometry::FindRoadCatalog(catalogId_);
            if(category_==0 && profile)
            {
                context.CanvasRect({0.08F,0.31F},{0.92F,0.75F},
                    grey,true);
                const float lanes=static_cast<float>(profile->laneCount);
                for(u32 i=1U;i<profile->laneCount;++i)
                {
                    const float x=0.08F+0.84F*
                        static_cast<float>(i)/lanes;
                    context.CanvasLine({x,0.33F},{x,0.72F},
                        marking,i*2U==profile->laneCount?2.8F:1.2F);
                }
                context.CanvasLine({0.08F,0.31F},{0.08F,0.75F},
                    {0.9F,0.9F,0.9F,1.0F},1.7F);
                context.CanvasLine({0.92F,0.31F},{0.92F,0.75F},
                    {0.9F,0.9F,0.9F,1.0F},1.7F);
                context.CanvasText({0.03F,0.07F},
                    {0.92F,0.94F,0.96F,1.0F},
                    std::format("{} lanes | {:.1f} m pavement | {} km/h design",
                        profile->laneCount,profile->pavedWidthMeters,
                        static_cast<int>(designSpeedKmh_)));
            }
            else if(category_==1)
            {
                context.CanvasRect({0.20F,0.42F},{0.80F,0.80F},
                    {0.41F,0.36F,0.31F,1.0F},true);
                context.CanvasLine({0.43F,0.37F},{0.43F,0.76F},
                    {0.80F,0.83F,0.84F,1.0F},3.5F);
                context.CanvasLine({0.57F,0.37F},{0.57F,0.76F},
                    {0.80F,0.83F,0.84F,1.0F},3.5F);
                context.CanvasText({0.03F,0.07F},
                    {0.92F,0.94F,0.96F,1.0F},
                    "Single track | 1.435 m gauge");
            }
            else
            {
                context.CanvasLine({0.12F,0.61F},{0.88F,0.33F},
                    {0.42F,0.76F,0.95F,1.0F},2.5F);
                context.CanvasText({0.03F,0.07F},
                    {0.92F,0.94F,0.96F,1.0F},
                    "Unconstructed reference alignment");
            }
        }
        context.EndChild();

        context.Heading("Alignment");
        static constexpr std::array<std::string_view,3> alignment{
            "Straight","Curve","Routed"};
        if(context.SegmentedControl(
            "Road alignment##road-geometry",alignment,newEdgeMode_))
            UpdateViewportTool();
        if(newEdgeMode_==1 &&
           context.SliderDouble(
               "Curve offset##roads-curve",curveBias_,0.0,0.70))
            UpdateViewportTool();
        if(newEdgeMode_==2)
            context.MutedText(
                "Terrain routing uses the existing path router; native "
                "physical corridor construction may remain pending.");
        if(context.Checkbox(
            "Snap to existing junction nodes##roads-snap",snapToNodes_))
            UpdateViewportTool();
        if(snapToNodes_ &&
           context.SliderDouble(
               "Snap radius (m)##roads-snap-radius",nodeSnapMeters_,1.0,50.0))
            UpdateViewportTool();
        context.MutedText(
            "Snapping joins real nodes in the selected network, "
            "never roads that only visually cross.");
        context.Separator();
        context.KeyValue("Placement elevation",
            std::format("{:+.1f} m above reference surface",
                elevationMeters_));
        if(context.Button("Lower 1 m##roads-elevation-down"))
        {
            elevationMeters_=std::max(-10000.0,elevationMeters_-1.0);
            UpdateViewportTool();
        }
        context.SameLine();
        if(context.Button("Raise 1 m##roads-elevation-up"))
        {
            elevationMeters_=std::min(100000.0,elevationMeters_+1.0);
            UpdateViewportTool();
        }
        context.SameLine();
        if(context.Button("Reset height##roads-elevation-reset"))
        {
            elevationMeters_=0.0;
            UpdateViewportTool();
        }
        if(context.InputDouble("Exact height (m)##roads-elevation",
            elevationMeters_))
        {
            if(!std::isfinite(elevationMeters_))
                elevationMeters_=0.0;
            elevationMeters_=std::clamp(elevationMeters_,-10000.0,100000.0);
            UpdateViewportTool();
        }
        context.MutedText(
            "Page Up / Page Down adjust height in the viewport "
            "(Shift: 5 m). Height is relative to the reference ellipsoid, "
            "not automatically sampled terrain.");

        if(context.Section("Road level and engineering##road-levels",false))
        {
            static constexpr std::array<std::string_view,4> levels{
                "Ground","Bridge","Tunnel","Deck"};
            i32 level=constructionMode_=="bridge"?1:
                constructionMode_=="tunnel"?2:
                constructionMode_=="deck"?3:0;
            if(context.SegmentedControl(
                "Construction method##roads-level",levels,level))
            {
                constructionMode_=level==1?"bridge":
                    level==2?"tunnel":level==3?"deck":"terrain";
                UpdateViewportTool();
            }
            if(level!=0)
                context.MutedText(
                    "Structures and excavation are not implemented: this "
                    "choice authors construction intent only.");
            if(category_!=2)
            {
                if(context.SliderDouble(
                    "Design speed (km/h)##roads-speed",
                    designSpeedKmh_,10.0,180.0))
                {
                    speed_=designSpeedKmh_/3.6;
                    UpdateViewportTool();
                }
                if(context.Checkbox(
                    "Ease turns##roads-easing",easeTurns_))
                    UpdateViewportTool();
            }
        }

        if(context.Section("Networks##road-networks",false))
        {
            static_cast<void>(context.InputText(
                "New network name##roads-network-name",newNetworkName_));
            if(context.Button("Create and select##roads-network-create"))
            {
                CreateNetwork();
                if(viewportTool_)viewportTool_->lastPlacedPathNode.reset();
                UpdateViewportTool();
            }
            for(const auto& entry:networks_)
            {
                const auto found=session_->PathNetwork().Service().FindNetwork(
                    paths::NetworkId{entry.id.high,entry.id.low});
                if(!found)continue;
                if(context.Selectable(entry.name+
                    "##roads-network-"+entry.id.ToString(),
                    newNetworkId_==found->id))
                {
                    newNetworkId_=found->id;
                    if(viewportTool_)viewportTool_->lastPlacedPathNode.reset();
                    UpdateViewportTool();
                }
            }
            if(context.Button("Automatically select network##roads-network-auto"))
            {
                newNetworkId_={};
                if(viewportTool_)
                {
                    viewportTool_->activePathNetwork.reset();
                    viewportTool_->lastPlacedPathNode.reset();
                }
            }
            context.MutedText(
                "Auto selection uses an Explorer path/network selection "
                "or creates a body-owned network on first placement.");
        }
        if(!session_->World().HasWorld())
            context.ErrorText("Open a world before using the road builder.");
        if(!status_.empty())context.MutedText(status_);
        if(!error_.empty())context.ErrorText(error_);

        if(context.Section("Road manager / bulk upgrade##roads-upgrade",false))
        {
            context.MutedText(
                "Select multiple authored roads and update their geometry "
                "profiles together. Alignment, construction mode and "
                "junction identity are preserved.");
            static_cast<void>(context.InputText(
                "Filter roads##roads-inventory-search",roadInventoryFilter_));
            static_cast<void>(context.Checkbox(
                "Only selected network##roads-upgrade-network",
                upgradeOnlyActiveNetwork_));
            static_cast<void>(context.Checkbox(
                "Show build errors only##roads-upgrade-errors",
                showOnlyConstructionErrors_));
            const auto& cache=session_->PathProducts();
            std::size_t matching=0;
            for(const auto& edge:edges_)
            {
                if(!RoadMatchesFilter(edge))continue;
                const auto road=session_->PathNetwork().Service().FindEdge(edge.id);
                if(!road)continue;
                if(upgradeOnlyActiveNetwork_ && newNetworkId_ &&
                   road->network!=newNetworkId_)continue;
                if(showOnlyConstructionErrors_ &&
                   cache.RebuildError(edge.id).empty())continue;
                ++matching;
            }
            context.KeyValue("Matching roads",std::to_string(matching));
            if(context.Button("Select visible##roads-manager-all"))
            {
                for(const auto& edge:edges_)
                {
                    if(!RoadMatchesFilter(edge))continue;
                    const auto road=session_->PathNetwork().Service().FindEdge(edge.id);
                    if(!road)continue;
                    if(upgradeOnlyActiveNetwork_ && newNetworkId_ &&
                       road->network!=newNetworkId_)continue;
                    if(showOnlyConstructionErrors_ &&
                       cache.RebuildError(edge.id).empty())continue;
                    selectedRoads_.insert(edge.id);
                }
            }
            context.SameLine();
            if(context.Button("Clear selection##roads-manager-clear"))
                selectedRoads_.clear();
            context.KeyValue("Selected",std::to_string(selectedRoads_.size()));
            if(context.BeginChild("roads-manager-list",
                {.width=0.0F,.height=235.0F},true))
            {
                for(const auto& edge:edges_)
                {
                    if(!RoadMatchesFilter(edge))continue;
                    const auto road=session_->PathNetwork().Service().FindEdge(edge.id);
                    if(!road)continue;
                    if(upgradeOnlyActiveNetwork_ && newNetworkId_ &&
                       road->network!=newNetworkId_)continue;
                    const auto failure=cache.RebuildError(edge.id);
                    if(showOnlyConstructionErrors_ && failure.empty())continue;
                    const auto* preset=path_geometry::FindRoadCatalog(
                        road->infrastructureCatalogId);
                    const char* state=!failure.empty()?"ERROR":
                        cache.IsDirty(edge.id)?"BUILDING":
                        cache.Find(edge.id)?"READY":"NOT BUILT";
                    const auto label=std::format(
                        "[{}] {} | {} / {} lane(s)##roads-manager-{}",
                        state,edge.name,preset->label,preset->laneCount,
                        edge.id.ToString());
                    if(context.Selectable(label,
                        selectedRoads_.contains(edge.id)))
                    {
                        if(!selectedRoads_.erase(edge.id))
                            selectedRoads_.insert(edge.id);
                        upgradeTargetEdgeId_=edge.id.ToString();
                    }
                     if(!failure.empty())
                        context.ErrorText(failure);
                }
            }
            context.EndChild();
            const auto* target=path_geometry::FindRoadCatalog(catalogId_);
            if(target)
                context.KeyValue("Target profile",
                    std::format("{} | {} lanes | {:.1f} m",
                        target->label,target->laneCount,
                        target->pavedWidthMeters));
            if(context.PrimaryButton("Upgrade selected roads (undoable)##roads-batch-upgrade"))
                UpgradeSelectedRoads();
            if(context.Button("Inspect last selected segment##roads-manager-expert"))
            {
                if(const auto id=scene::ObjectId::Parse(upgradeTargetEdgeId_))
                    SelectEdge(*id);
                page_=2;
            }
        }
        if(context.Section("Single road quick upgrade##roads-upgrade-legacy",false))
        {
            context.MutedText(
                "Change only an existing road's section profile. "
                "Its alignment, node IDs and construction level stay intact.");
            if(edges_.empty())
                context.MutedText("No authored path edges in this world.");
            else
            {
                for(const auto& entry:edges_)
                {
                    const auto road=session_->PathNetwork().Service().FindEdge(
                        entry.id);
                    if(!road || !path_geometry::FindRoadCatalog(
                        road->infrastructureCatalogId))continue;
                    const auto* previous=path_geometry::FindRoadCatalog(
                        road->infrastructureCatalogId);
                    const auto label=std::format(
                        "{} | {} lane(s)##roads-upgrade-{}",
                        entry.name,previous->laneCount,entry.id.ToString());
                    if(context.Selectable(label,
                        upgradeTargetEdgeId_==entry.id.ToString()))
                        upgradeTargetEdgeId_=entry.id.ToString();
                }
                if(!upgradeTargetEdgeId_.empty())
                {
                    const auto id=scene::ObjectId::Parse(upgradeTargetEdgeId_);
                    const auto road=id?
                        session_->PathNetwork().Service().FindEdge(*id):
                        std::nullopt;
                    const auto* from=road?
                        path_geometry::FindRoadCatalog(
                            road->infrastructureCatalogId):nullptr;
                    const auto* into=path_geometry::FindRoadCatalog(catalogId_);
                    if(from && into)
                        context.KeyValue("Section change",
                            std::format("{:.1f} m / {} lanes  ->  {:.1f} m / {} lanes",
                                from->pavedWidthMeters,from->laneCount,
                                into->pavedWidthMeters,into->laneCount));
                    if(context.PrimaryButton(
                        "Upgrade selected road##roads-upgrade-run"))
                        UpgradeRoadSection();
                    if(context.Button("Edit alignment and junctions##roads-upgrade-expert"))
                    {
                        if(id)SelectEdge(*id);
                        page_=2;
                    }
                }
            }
        }
        if(context.Section("Construction inventory and diagnostics##road-inventory",false))
        {
            context.KeyValue("Authored networks",
                std::to_string(networks_.size()));
            context.KeyValue("Network nodes",
                std::to_string(nodes_.size()));
            context.KeyValue("Authored segments",
                std::to_string(edges_.size()));
            const auto& cache=session_->PathProducts();
            std::size_t built=0,dirty=0,failures=0;
            for(const auto& edge:edges_)
            {
                if(cache.Find(edge.id))++built;
                if(cache.IsDirty(edge.id))++dirty;
                if(!cache.RebuildError(edge.id).empty())++failures;
            }
            context.KeyValue("Native corridor cache",std::to_string(built));
            context.KeyValue("Pending regeneration",std::to_string(dirty));
            context.KeyValue("Construction errors",std::to_string(failures));
            if(context.Button("Refresh authored inventory##road-refresh"))
                Refresh();
            if(context.Button("Inspect selected road in expert editor##road-expert-last"))
                page_=2;
            context.MutedText(
                "Native generation is asynchronous/budgeted; these counts "
                "never imply that bridges or traffic simulations are ready.");
        }
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
            driveReport_ = rpc::Value{};
            runtimeVehicleReport_=rpc::Value{};
            runtimeVehicleId_.clear();
            junctionNodeId_.clear();
            knownWorldGeneration_ = ~u64{0};
        }
        if(page_==0)
        {
            context.Heading("Build / Procedurals");
            context.MutedText("Select a procedural construction system.");
            if(context.PrimaryButton(
                "Procedural Paths & Roads##proc-hub-paths"))
                page_=1;
            context.MutedText("Roads, railway tracks, reference paths and junctions. Open the city-builder style construction palette.");
            if(context.Button("Advanced infrastructure editor##proc-hub-advanced"))
                page_=2;
            return;
        }
        if(page_==1)
        {
            DrawBuilder(context);
            return;
        }
        if(context.Button("Back to Procedurals##proc-advanced-back"))
        {
            page_=0;
            return;
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
                        driveReport_=rpc::Value{};
                    }
            }
            if (context.Section("Advanced node UUID##proc-junction-id",false))
                static_cast<void>(context.InputText("Node UUID##proc-junction-manual",junctionNodeId_));
            if (context.Button("Analyze junction##proc-junction-run"))
                AnalyzeJunction();
            if(context.Section("Driving seam regression##proc-junction-drive",false))
            {
                context.MutedText(
                    "Runs 480 deterministic steps at 120 Hz using four sprung wheel contacts. CPU fixture only; no gameplay vehicle is spawned.");
                if(context.Button("Run junction drive test##proc-junction-drive-run"))
                    RunJunctionDriveFixture();
                if(!driveError_.empty())context.ErrorText(driveError_);
                if(driveReport_.IsObject())
                {
                    const auto* ready=driveReport_.Find("ready");
                    if(ready && ready->IsBool() && !ready->AsBool())
                    {
                        if(const auto* reason=driveReport_.Find("reason");
                           reason && reason->IsString())
                            context.ErrorText(reason->AsString());
                    }
                    else
                    {
                        const auto* passed=driveReport_.Find(
                            "seam_continuity_passed");
                        if(passed && passed->IsBool())
                            context.KeyValue("CPU seam continuity",
                                passed->AsBool()?"Pass (fixture)":"Failed / incomplete");
                        for(const auto* key:{"steps","full_contact_steps",
                            "patch_contact_steps","wheel_owner_transitions"})
                            if(const auto* n=driveReport_.Find(key);
                               n && n->IsInteger())
                                context.KeyValue(key,
                                    std::to_string(n->AsInteger()));
                        for(const auto* key:{"distance_meters",
                            "maximum_bottom_out_meters",
                            "maximum_vertical_acceleration_mps2"})
                            if(const auto* n=driveReport_.Find(key);
                               n && n->IsNumber())
                                context.KeyValue(key,
                                    std::format("{:.4f}",n->AsNumber()));
                        if(const auto* error=driveReport_.Find("error");
                           error && error->IsString() &&
                           !error->AsString().empty())
                            context.ErrorText(error->AsString());
                        context.MutedText(
                            "Fixture runs a simplified planar car body. It does not certify physics-world colliders, tire simulation or playable vehicles.");
                    }
                }
            }
            if(context.Section("Temporary vehicle runtime##proc-vehicle-runtime",false))
            {
                context.MutedText(
                    "Transient simulation actors use the current contact generation. Each button advances only the requested fixed steps; no game vehicle is created.");
                if(context.Button("Spawn car##proc-runtime-spawn"))
                    RunRoadVehicleWorldAction("spawn");
                context.SameLine();
                if(context.Button("Step 60 ticks##proc-runtime-step"))
                    RunRoadVehicleWorldAction("step");
                context.SameLine();
                if(context.Button("Inspect car##proc-runtime-status"))
                    RunRoadVehicleWorldAction("status");
                if(!runtimeVehicleId_.empty())
                {
                    context.KeyValue("Runtime car",runtimeVehicleId_);
                    static_cast<void>(context.InputDouble(
                        "Throttle (0..1)##proc-runtime-throttle",runtimeThrottle_));
                    static_cast<void>(context.InputDouble(
                        "Brake (0..1)##proc-runtime-brake",runtimeBrake_));
                    static_cast<void>(context.InputDouble(
                        "Steering (rad)##proc-runtime-steer",runtimeSteer_));
                    if(context.Button("Apply controls##proc-runtime-control"))
                        RunRoadVehicleWorldAction("controls");
                    context.SameLine();
                    if(context.Button("Remove car##proc-runtime-remove"))
                        RunRoadVehicleWorldAction("remove");
                }
                if(!runtimeVehicleError_.empty())
                    context.ErrorText(runtimeVehicleError_);
                if(runtimeVehicleReport_.IsObject())
                {
                    if(const auto* reason=runtimeVehicleReport_.Find("reason");
                       reason && reason->IsString())
                        context.ErrorText(reason->AsString());
                    for(const auto* key:{"vehicle_count","tick",
                        "completed_steps"})
                        if(const auto* value=runtimeVehicleReport_.Find(key);
                           value && value->IsInteger())
                            context.KeyValue(key,
                                std::to_string(value->AsInteger()));
                    if(const auto* speed=runtimeVehicleReport_.Find("speed_mps");
                       speed && speed->IsNumber())
                        context.KeyValue("Speed m/s",
                            std::format("{:.2f}",speed->AsNumber()));
                }
            }
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
    StudioViewportPanel* viewportTool_{nullptr};
    i32 page_{0}; // Hub, city-builder palette, advanced editor.
    f64 curveBias_{0.25};
    f64 elevationMeters_{0.0};
    std::string upgradeTargetEdgeId_;
    std::set<scene::ObjectId> selectedRoads_;
    std::string roadInventoryFilter_;
    bool upgradeOnlyActiveNetwork_{false};
    bool showOnlyConstructionErrors_{false};


    bool snapToNodes_{true};
    f64 nodeSnapMeters_{10.0};
    f64 designSpeedKmh_{60.0};
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
    rpc::Value driveReport_;
    std::string driveError_;
    rpc::Value runtimeVehicleReport_;
    std::string runtimeVehicleId_;
    std::string runtimeVehicleError_;
    double runtimeThrottle_{0.15};
    double runtimeBrake_{0.0};
    double runtimeSteer_{0.0};
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
