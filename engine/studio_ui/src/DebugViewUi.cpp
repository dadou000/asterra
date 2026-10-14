#include <orbit/studio_ui/DebugViewUi.hpp>
#include <cmath>
#include <format>
#include <exception>
#include <stdexcept>
#include <orbit/paths/PathNetwork.hpp>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] const char* ViewportLabel(
    const std::string_view id) noexcept
{
    if (id == "studio.primary")
    {
        return "Perspective Viewport";
    }
    if (id == "studio.map")
    {
        return "Body Map / Debug Viewport";
    }

    return "Viewport";
}

[[nodiscard]] const char* SurfaceDebugModeName(
    const lighting::SurfaceDebugMode mode) noexcept
{
    switch (mode)
    {
    case lighting::SurfaceDebugMode::Lit:
        return "Lit";
    case lighting::SurfaceDebugMode::BaseColorRoughness:
        return "Albedo / Roughness";
    case lighting::SurfaceDebugMode::NormalMetallic:
        return "Normal / Metallic";
    case lighting::SurfaceDebugMode::EmissionMetadata:
        return "Emission / Meta";
    }

    return "Lit";
}
} // namespace

DebugViewUi::DebugViewUi(StudioRenderViewSet& views,
    studio_session::StudioSession& session) noexcept
    : views_(&views), session_(&session)
{
    RegisterBuiltInDebugTests(tests_);
}

void DebugViewUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Debug",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Right,
        .dockOrder = 60,
        .minSize = {.width = 260.0F, .height = 160.0F},
        .draw =
            [this, &ui](editor_ui::PanelContext& context)
            {
                Draw(ui, context);
            }});
}

void DebugViewUi::Draw(
    editor_ui::EditorUi& ui,
    editor_ui::PanelContext& context)
{
    context.Heading("Debug / Test Center");
    context.MutedText("One entry point for rendering, terrain, lighting, performance and validation tools.");
    const auto open = [&](std::string_view title, std::string_view button) {
        if (context.Button(button))
        {
            if (!ui.FocusPanelByTitle(title))
                context.MutedText("This tool is not registered in the current workspace.");
        }
    };

    context.Separator();
    context.Heading("Terrain / Infrastructure");
    open("Surface Authoring", "Terrain authoring##dbg-terrain");
    context.SameLine();
    open("Body Map / Debug View", "Terrain map##dbg-map");
    context.MutedText("Registered terrain tests appear below. Road grading, rendered displacement and collision end-to-end tests are not yet available.");

    context.Heading("Road / Railway engineering");
    context.MutedText("Inspect an authored path edge using the existing path.engineering RPC. Read-only.");
    if (context.Button("Refresh path edges##dbg-road-refresh") && session_)
    {
        engineeringEdges_.clear();
        if (session_->World().HasWorld())
        {
            const auto& objects = session_->World().Objects();
            auto pending = objects.Roots();
            std::size_t visited = 0U;
            while (!pending.empty() && visited++ < 8192U &&
                   engineeringEdges_.size() < 128U)
            {
                const auto object = pending.back();
                pending.pop_back();
                if (object.type == paths::kPathEdgeType)
                    engineeringEdges_.push_back(object);
                const auto children = objects.Children(object.id);
                pending.insert(pending.end(), children.begin(), children.end());
            }
        }
    }
    context.MutedText(std::format("{} path edges discovered", engineeringEdges_.size()));
    if (context.Section("Select authored edge##dbg-road-edges", false))
    {
        for (const auto& edge : engineeringEdges_)
        {
            if (context.Button(edge.name + "##dbg-edge-" + edge.id.ToString()))
            {
                engineeringEdgeId_ = edge.id.ToString();
                engineeringResult_.clear();
            }
        }
    }
    static_cast<void>(context.InputText("Edge UUID##dbg-road-edge", engineeringEdgeId_));
    static_cast<void>(context.InputDouble("Design speed (m/s)##dbg-road-speed", engineeringSpeedMps_));
    if (context.Button("Analyze edge##dbg-road-analyze"))
    {
        engineeringError_ = false;
        try
        {
            if (!engineeringRpc_)
                throw std::runtime_error("Path engineering RPC is unavailable.");
            if (engineeringEdgeId_.empty() ||
                !std::isfinite(engineeringSpeedMps_) || engineeringSpeedMps_ < 0.0)
                throw std::invalid_argument("Enter an edge UUID and a finite, non-negative design speed.");
            rpc::Value request(rpc::Value::Object{
                {"jsonrpc", "2.0"},
                {"id", static_cast<i64>(1)},
                {"method", "path.engineering"},
                {"params", rpc::Value(rpc::Value::Object{
                    {"edge", engineeringEdgeId_},
                    {"design_speed_mps", engineeringSpeedMps_}})}
            });
            const auto response = engineeringRpc_->Dispatch(rpc::Serialize(request));
            if (!response)
                throw std::runtime_error("No engineering RPC response.");
            const auto parsed = rpc::ParseValue(*response);
            if (const auto* error = parsed.Find("error"); error)
                throw std::runtime_error(rpc::Serialize(*error));
            const auto* result = parsed.Find("result");
            if (!result)
                throw std::runtime_error("Engineering response has no result.");
            engineeringData_ = *result;
            engineeringResult_ = rpc::Serialize(*result);
        }
        catch (const std::exception& error)
        {
            engineeringResult_ = error.what();
            engineeringError_ = true;
        }
    }
    if (!engineeringResult_.empty())
    {
        if (engineeringError_)
            context.ErrorText(engineeringResult_);
        else if (engineeringData_.IsObject())
        {
            const auto* ready = engineeringData_.Find("ready");
            if (ready && ready->IsBool() && !ready->AsBool())
                context.MutedText("Path geometry is not ready. Try again after the route builds.");
            else
            {
                if (const auto* count = engineeringData_.Find("station_count");
                    count && count->IsInteger())
                    context.KeyValue("Stations", std::to_string(count->AsInteger()));
                if (const auto* signature = engineeringData_.Find("signature");
                    signature && signature->IsInteger())
                    context.KeyValue("Geometry signature", std::to_string(signature->AsInteger()));
                if (const auto* warnings = engineeringData_.Find("warnings");
                    warnings && warnings->IsArray())
                {
                    context.KeyValue("Engineering findings",
                        std::to_string(warnings->AsArray().size()));
                    for (const auto& finding : warnings->AsArray())
                    {
                        if (!finding.IsObject()) continue;
                        const auto* message = finding.Find("message");
                        const auto* station = finding.Find("station_meters");
                        if (message && message->IsString())
                            context.Text(std::format("At {:.1f} m: {}",
                                station && station->IsNumber() ? station->AsNumber() : 0.0,
                                message->AsString()));
                    }
                }
                if (const auto* banks = engineeringData_.Find("suggested_bank_radians");
                    banks && banks->IsArray())
                    context.KeyValue("Cant/bank suggestions",
                        std::to_string(banks->AsArray().size()));
            }
        }
    }

    context.Heading("Registered experiments");
    for (const auto& test : tests_.Catalog())
    {
        context.Text(std::string(DebugTestCategoryName(test.category)) +
            " / " + test.label);
        if (context.Button("Run##debug-test:" + test.id) && session_)
            static_cast<void>(tests_.Run(test.id, *session_));
        const auto result = tests_.LastResult(test.id);
        const char* state = "NOT RUN";
        switch (result.outcome)
        {
        case DebugTestOutcome::NotRun: state = "NOT RUN"; break;
        case DebugTestOutcome::Pass: state = "PASS"; break;
        case DebugTestOutcome::Fail: state = "FAIL"; break;
        case DebugTestOutcome::NotReady: state = "NOT READY"; break;
        }
        context.KeyValue(state, result.detail);
        const auto history = tests_.History(test.id);
        if (!history.empty())
        {
            const auto& latest = history.back();
            context.KeyValue("Last run",
                std::to_string(latest.sequence) + " / " +
                std::to_string(latest.elapsedMilliseconds) +
                " ms / world " + std::to_string(latest.worldGeneration));
            context.KeyValue("Recorded runs", std::to_string(history.size()));
        }
    }

    context.Separator();
    context.Heading("Lighting / Atmosphere");
    open("Display Diagnostics", "Display and eye adaptation##dbg-display");
    context.SameLine();
    open("Shading", "Shading##dbg-shading");
    open("Celestial", "Atmosphere and celestial##dbg-celestial");

    context.Separator();
    context.Heading("Performance / Reports");
    open("Profiler", "Profiler##dbg-profiler");
    context.SameLine();
    open("Reports", "Reports##dbg-reports");

    context.Separator();
    context.Heading("Viewport GBuffer");
    context.MutedText(
        "Chooses which GBuffer channel each viewport renders. "
        "The same switch is available per-viewport in its own toolbar, "
        "and over RPC/MCP as view.surface_debug_set.");

    if (views_ == nullptr)
    {
        context.ErrorText("No render-view set is bound.");
        return;
    }

    bool first = true;
    for (const auto& info : views_->Catalog())
    {
        if (!first)
        {
            context.Separator();
        }
        first = false;

        DrawViewportRow(context, info);
    }
}

void DebugViewUi::DrawViewportRow(
    editor_ui::PanelContext& context,
    const StudioRenderViewInfo& info)
{
    context.Text(ViewportLabel(info.id));

    const std::string lit = "Lit##debug-view-lit:" + info.id;
    const std::string base =
        "Albedo / Roughness##debug-view-base:" + info.id;
    const std::string normal =
        "Normal / Metallic##debug-view-normal:" + info.id;
    const std::string emission =
        "Emission / Meta##debug-view-emission:" + info.id;

    if (context.Button(lit))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::Lit);
    }
    context.SameLine();
    if (context.Button(base))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::BaseColorRoughness);
    }
    context.SameLine();
    if (context.Button(normal))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::NormalMetallic);
    }
    context.SameLine();
    if (context.Button(emission))
    {
        views_->SetSurfaceDebugMode(
            info.id, lighting::SurfaceDebugMode::EmissionMetadata);
    }

    context.KeyValue(
        "Current",
        SurfaceDebugModeName(info.surfaceDebugMode));
}
} // namespace orbit::studio_ui
