#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_ui/ProfilerModel.hpp>

#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace orbit::studio_ui
{
// The Profiler panel: a live timeline of every thread (or CPU core) fed by the
// always-on micro-profiler (docs/ORBIT_PROFILER.md), which can be paused and
// inspected: zoom and pan, pick a slice for its self time and core, see the
// heaviest scopes in view, the stack samples taken during a stall, and open a
// saved hitch trace. Every control has a profiler.panel_* RPC / MCP equivalent
// (RegisterProfilerPanelRpc), driving the same ProfilerModel.
class ProfilerUi
{
public:
    ProfilerUi();

    void Register(editor_ui::EditorUi& ui);
    void SetViewportOnlyCaptureAction(
        std::function<bool(f64, std::string_view, std::string_view)> action)
    {
        viewportOnlyCaptureAction_ = std::move(action);
    }

    [[nodiscard]] ProfilerModel& Model() noexcept { return model_; }

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f5242495450524fULL,
        .low = 0x46494c4552504e4cULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void DrawToolbar(editor_ui::PanelContext& context, f64 now);
    void DrawOptions(editor_ui::PanelContext& context);
    void DrawFrameStrip(editor_ui::PanelContext& context, f64 now);
    void DrawTimeline(editor_ui::PanelContext& context, f64 now);
    void DrawDetails(editor_ui::PanelContext& context, f64 now);
    void DrawScopes(editor_ui::PanelContext& context);
    void DrawStalls(editor_ui::PanelContext& context);
    void DrawHitches(editor_ui::PanelContext& context);

    ProfilerModel model_;
    // Pointer travel since the button went down; a press that barely moves is a
    // click, anything else is a pan.
    f32 pressTravelPixels_{0.0F};
    std::string lastCapturePath_;
    std::string captureError_;
    std::function<bool(f64, std::string_view, std::string_view)> viewportOnlyCaptureAction_;
};

// profiler.panel_get / profiler.panel_set / profiler.snapshot: the RPC face of
// the panel (pause, options, view, selection, loading a hitch trace) and an
// analysis of the current snapshot: lanes, top scopes, slowest slices, stalls.
void RegisterProfilerPanelRpc(rpc::Dispatcher& dispatcher, ProfilerModel& model);
} // namespace orbit::studio_ui
