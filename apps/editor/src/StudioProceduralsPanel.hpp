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
        context.Heading("Procedural Paths & Roads");
        context.MutedText("Place road or railway sections directly in the world, using one connected construction tool.");
        if(context.Button("Back to Procedurals##roads-back"))
        {
            page_=0;
            return;
        }
        context.SameLine();
        if(context.Button("Detailed network editor##roads-advanced"))
        {
            page_=2;
            return;
        }
        static constexpr std::array<std::string_view,3> types{
            "Roads","Railways","Paths"};
        if(context.SegmentedControl("Build category##roads-type",types,category_))
        {
            catalogId_=category_==0?"rural-road":
                category_==1?"ballasted-rail":"";
            designSpeedKmh_=category_==1?85.0:category_==0?60.0:20.0;
            speed_=designSpeedKmh_/3.6;
            newNetworkName_=category_==0?"Road Network":
                category_==1?"Rail Network":"Path Network";
            if(viewportTool_ && viewportTool_->infrastructureDrawMode)
                viewportTool_->lastPlacedPathNode.reset();
            UpdateViewportTool();
        }
        context.Heading("1. Choose a construction type");
        // The palette is deliberately truthful: only real engine catalogs
        // are selectable; future cross sections are labelled unavailable.
        if(category_==0)
        {
            for(const auto& preset:path_geometry::kRoadCatalog)
            {
                const auto label=std::format(
                    "{}  |  {:.1f} m  |  {} lane{}##roads-preset-{}",
                    preset.label,preset.pavedWidthMeters,preset.laneCount,
                    preset.laneCount==1?"":"s",preset.id);
                if(context.SelectableWithIcon(label,
                    catalogId_==preset.id,editor_ui::ToolbarIcon::Procedural))
                {
                    catalogId_=std::string(preset.id);
                    designSpeedKmh_=preset.designSpeedKmh;
                    speed_=designSpeedKmh_/3.6;
                    UpdateViewportTool();
                }
            }
            context.MutedText("Native geometry uses each listed width and lane count. Markings, sidewalks and complex intersections remain future work.");
        }
        else if(category_==1)
        {
            if(context.SelectableWithIcon(
                "Single ballasted track  |  1.435 m gauge##roads-rail",
                catalogId_=="ballasted-rail",editor_ui::ToolbarIcon::Procedural))
            {
                catalogId_="ballasted-rail";
                speed_=85.0/3.6;
                UpdateViewportTool();
            }
            context.MutedText("Planned: double tracks, stations, turnout construction, ballast variations and electrification.");
        }
        else
        {
            if(context.SelectableWithIcon(
                "Reference spline / path  |  no constructed road##roads-reference",
                catalogId_.empty(),editor_ui::ToolbarIcon::Procedural))
            {
                catalogId_.clear();
                UpdateViewportTool();
            }
            context.MutedText("A reference path is an authored network edge, not a road or a railway.");
        }
        // A graphic cross-section gives the palette the immediate, legible
        // road-type preview of a city builder without an extra render target.
        if(context.BeginChild("roads-section-preview",
            {.width=0,.height=68},true))
        {
            const auto canvas=context.Canvas("roads-cross-section",
                {.width=0,.height=55});
            const math::Float4 ground{0.20F,0.26F,0.24F,1.0F};
            const math::Float4 road{0.32F,0.36F,0.41F,1.0F};
            const math::Float4 stripe{0.94F,0.83F,0.38F,1.0F};
            const math::Float4 rail{0.66F,0.71F,0.73F,1.0F};
            context.CanvasRect({0.01F,0.62F},{0.99F,0.93F},ground,true);
            if(category_==0)
            {
                context.CanvasRect({0.16F,0.31F},{0.84F,0.66F},road,true);
                context.CanvasLine({0.50F,0.33F},{0.50F,0.64F},stripe,2.0F);
                context.CanvasText({0.02F,0.03F},{0.90F,0.93F,0.96F,1.0F},
                    path_geometry::FindRoadCatalog(catalogId_)?
                    std::format("PAVED ROAD | {} lane(s) | {:.1f} m",
                        path_geometry::FindRoadCatalog(catalogId_)->laneCount,
                        path_geometry::FindRoadCatalog(catalogId_)->pavedWidthMeters):
                    "PAVED ROAD");
            }
            else if(category_==1)
            {
                context.CanvasRect({0.21F,0.45F},{0.79F,0.70F},
                    {0.39F,0.34F,0.30F,1.0F},true);
                context.CanvasLine({0.43F,0.38F},{0.43F,0.66F},rail,3.0F);
                context.CanvasLine({0.57F,0.38F},{0.57F,0.66F},rail,3.0F);
                context.CanvasText({0.02F,0.03F},{0.90F,0.93F,0.96F,1.0F},
                    "BALLASTED TRACK   |   1.435 m");
            }
            else
                context.CanvasText({0.02F,0.03F},{0.90F,0.93F,0.96F,1.0F},
                    "REFERENCE ALIGNMENT");
            static_cast<void>(canvas);
        }
        context.EndChild();
        context.Heading("2. Draw mode");
        static constexpr std::array<std::string_view,3> modes{
            "Straight","Curve","Terrain route"};
        if(context.SegmentedControl("Draw alignment##roads-alignment",modes,newEdgeMode_))
        {
            if(viewportTool_ && viewportTool_->infrastructureDrawMode)
                viewportTool_->lastPlacedPathNode.reset();
            UpdateViewportTool();
        }
        context.MutedText(newEdgeMode_==0?
            "Click points to draw straight road segments.":
            newEdgeMode_==1?
            "Click points for connected cubic Bezier segments. Curve strength is adjustable.":
            "Click two points to request a terrain-cost route. Routing must be available for construction.");
        if(newEdgeMode_==1)
        {
            if(context.SliderDouble("Curve strength##roads-curve",
                curveBias_,0.0,0.70))
                UpdateViewportTool();
        }
        if(context.Checkbox("Snap to existing path nodes##roads-snap",
            snapToNodes_))
            UpdateViewportTool();
        if(snapToNodes_ &&
           context.SliderDouble("Node snap radius (m)##roads-snap-radius",
               nodeSnapMeters_,1.0,50.0))
            UpdateViewportTool();
        context.MutedText("Snapping joins existing endpoints in the same network. Connecting to the middle of a road requires an explicit split.");
        context.Heading("3. Construction settings");
        static constexpr std::array<std::string_view,4> levels{
            "Ground","Bridge","Tunnel","Deck"};
        i32 level=constructionMode_=="bridge"?1:
            constructionMode_=="tunnel"?2:
            constructionMode_=="deck"?3:0;
        if(context.SegmentedControl("Road level##roads-level",levels,level))
        {
            constructionMode_=level==1?"bridge":
                level==2?"tunnel":level==3?"deck":"terrain";
            UpdateViewportTool();
        }
        if(level!=0)
            context.MutedText("Bridge/tunnel/deck stores construction intent; physical structures and excavation are not built yet.");
        if(category_!=2)
        {
            if(context.SliderDouble("Design speed (km/h)##roads-design-speed",
                designSpeedKmh_,10.0,180.0))
            {
                speed_=designSpeedKmh_/3.6;
                UpdateViewportTool();
            }
            if(context.Checkbox("Ease curves##roads-ease",easeTurns_))
                UpdateViewportTool();
        }
        context.Heading("4. Network and placement");
        if(!session_->World().HasWorld())
            context.ErrorText("Open a world to create infrastructure.");
        if(context.Section("Network selection##roads-network",false))
        {
            static_cast<void>(context.InputText(
                "New network##roads-network-name",newNetworkName_));
            if(context.Button("Create network##roads-create-network"))
            {
                CreateNetwork();
                UpdateViewportTool();
            }
            for(const auto& network:networks_)
            {
                const auto found=session_->PathNetwork().Service().FindNetwork(
                    paths::NetworkId{network.id.high,network.id.low});
                if(!found)continue;
                if(context.Selectable(network.name+
                    "##roads-network-"+network.id.ToString(),
                    newNetworkId_==found->id))
                {
                    newNetworkId_=found->id;
                    if(viewportTool_)viewportTool_->lastPlacedPathNode.reset();
                    UpdateViewportTool();
                }
            }
            context.MutedText("Without a selected network, the viewport creates one on the first click.");
        }
        const bool drawing=viewportTool_ && viewportTool_->pathPlacementMode &&
            viewportTool_->infrastructureDrawMode;
        if(context.PrimaryButton(drawing?
            "Finish drawing##roads-stop":
            "Start drawing in viewport##roads-start"))
            ToggleDraw(!drawing);
        if(drawing)
        {
            context.KeyValue("Construction tool","Active");
            context.MutedText(viewportTool_->lastPlacedPathNode?
                "Last point set. Click in the viewport for the next segment.":
                "Click the reference surface to set the first point.");
        }
        else
            context.MutedText("Select a type and press Start, then place successive points in the viewport.");
        if(!status_.empty())context.MutedText(status_);
        if(!error_.empty())context.ErrorText(error_);
        if(context.Section("Inspect and modify existing segments##roads-existing",false))
        {
            for(const auto& edge:edges_)
                if(context.Selectable(edge.name+
                    "##roads-pick-"+edge.id.ToString(),
                    edgeId_==edge.id.ToString()))
                    SelectEdge(edge.id);
            if(!edgeId_.empty() &&
                context.Button("Open selected segment in detailed editor##roads-edit"))
                page_=2;
            context.MutedText("The detailed editor includes exact profile IDs, engineering reports, splitting and junction diagnostics.");
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
