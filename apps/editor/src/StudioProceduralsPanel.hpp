#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <algorithm>
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
        context.MutedText("Authoritative path networks, road and railway engineering.");
        if (context.Button("Refresh networks##proc-refresh")) Refresh();
        context.SameLine();
        context.MutedText(std::format("{} edges",edges_.size()));
        if (context.Section("Networks & edges##proc-edges",true))
        {
            for (const auto& edge:edges_)
                if (context.Button((edge.name.empty()?"Unnamed edge":edge.name)+
                        "##proc-edge-"+edge.id.ToString()))
                {
                    edgeId_=edge.id.ToString();
                    report_=rpc::Value{};
                    error_.clear();
                }
        }
        context.Heading("Selected corridor");
        static_cast<void>(context.InputText("Edge ID##proc-id",edgeId_));
        static_cast<void>(context.InputDouble("Design speed (m/s)##proc-speed",speed_));
        if (context.Button("Analyze geometry##proc-analyze")) Analyze();
        if (!error_.empty()) context.ErrorText(error_);
        if (!report_.IsObject()) return;
        const auto* ready=report_.Find("ready");
        if (ready && ready->IsBool() && !ready->AsBool())
        {
            context.MutedText("Derived path not ready; refresh after routing completes.");
            return;
        }
        if (const auto* n=report_.Find("station_count");n && n->IsInteger())
            context.KeyValue("Stations",std::to_string(n->AsInteger()));
        if (const auto* sig=report_.Find("signature");sig && sig->IsInteger())
            context.KeyValue("Geometry revision",std::to_string(sig->AsInteger()));
        const auto* warnings=report_.Find("warnings");
        if (warnings && warnings->IsArray())
        {
            context.KeyValue("Warnings",std::to_string(warnings->AsArray().size()));
            if (context.Section("Engineering findings##proc-findings",true))
                for (std::size_t i=0;i<warnings->AsArray().size();++i)
                {
                    const auto& finding=warnings->AsArray()[i];
                    if (!finding.IsObject())continue;
                    const auto* text=finding.Find("message");
                    const auto* at=finding.Find("station_meters");
                    if (!text || !text->IsString())continue;
                    const double station=at && at->IsNumber()?at->AsNumber():0.0;
                    if (context.Button(std::format(
                        "{:.1f} m | {}##proc-finding-{}",station,text->AsString(),i)))
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
            context.KeyValue("Bank / cant stations",std::to_string(banks->AsArray().size()));
        context.MutedText("Read-only preview. Editing, auto-banking and earthworks visualization are forthcoming.");
    }
    studio_session::StudioSession* session_;
    rpc::Dispatcher* rpc_;
    std::vector<scene::ObjectRecord> edges_;
    std::string edgeId_;
    double speed_{20.0};
    std::string error_;
    rpc::Value report_;
    std::size_t selectedFinding_{0};
};
} // namespace orbit::editor_app
