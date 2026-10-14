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
        report_=rpc::Value{};
        error_.clear();
        if (!session_->World().HasWorld()) return;
        const auto& store=session_->World().Objects();
        auto pending=store.Roots();
        std::size_t visited=0;
        while (!pending.empty() && visited++<20000 && edges_.size()<512)
        {
            const auto record=pending.back();
            pending.pop_back();
            if (record.type==paths::kPathEdgeType) edges_.push_back(record);
            const auto children=store.Children(record.id);
            pending.insert(pending.end(),children.begin(),children.end());
        }
        std::sort(edges_.begin(),edges_.end(),
            [](const auto& a,const auto& b) {
                if(a.name!=b.name) return a.name<b.name;
                return a.id<b.id;
            });
    }
    void SelectEdge(const scene::ObjectId id)
    {
        edgeId_ = id.ToString();
        report_ = rpc::Value{};
        selectedFinding_ = 0;
        error_.clear();
        const auto edge = session_->PathNetwork().Service().FindEdge(id);
        if (!edge) { error_ = "The selected edge no longer exists."; return; }
        catalogId_ = edge->infrastructureCatalogId;
        catalogVersion_ = edge->infrastructureCatalogVersion;
        constructionMode_ = edge->constructionMode;
        speed_ = edge->designSpeedKmh > 0.0 ? edge->designSpeedKmh/3.6 : 20.0;
        easeTurns_ = edge->easeTurns;
        startHandle_ = edge->startHandleMeters;
        endHandle_ = edge->endHandleMeters;
        mode_ = edge->mode;
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
        }
        catch(const std::exception& e){error_=e.what();}
    }
    void Draw(editor_ui::PanelContext& context)
    {
        context.Heading("Build / Procedurals");
        context.MutedText("Infrastructure builder · Roads / Railways");
        static constexpr std::array<std::string_view,2> categories{"Roads","Railways"};
        static_cast<void>(context.SegmentedControl("Infrastructure##proc-category",
            categories, category_));
        context.MutedText(category_ == 0
            ? "Road layouts, grades, surface construction and junction-ready splines."
            : "Track alignments, design speed, rail curves and cant diagnostics.");

        if (context.Button("Refresh infrastructure##proc-refresh")) Refresh();
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

        if (edgeId_.empty())
        {
            context.MutedText("Select an authored segment to configure it.");
            return;
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
        if (const auto* banks=report_.Find("suggested_bank_radians");
            banks && banks->IsArray())
            context.KeyValue("Bank / cant suggestions",
                std::to_string(banks->AsArray().size()));
        context.MutedText("Analysis suggestions are read-only; no automatic banking is applied.");
    }
    studio_session::StudioSession* session_;
    rpc::Dispatcher* rpc_;
    std::vector<scene::ObjectRecord> edges_;
    std::string edgeId_;
    double speed_{20.0};
    std::string error_;
    rpc::Value report_;
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
