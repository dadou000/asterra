# Profiler viewport-only capture placement

- Existing owners: `StudioApplication::Run` owns frame-loop and RPC lifecycle; `EditorUi` owns panel drawing; `ProfilerUi` owns profiler presentation and history settings.
- Add a viewport-only draw path to `EditorUi` that invokes only the primary viewport panel, preserving the viewport's own scene/tool UI while skipping all other panels and shell chrome.
- The app frame-loop owns the temporary capture deadline and trace file lifecycle. A Profiler button and `profiler.viewport_capture` RPC call the same start/cancel action; status is exposed over RPC/MCP.
- Continue composing/rendering the primary viewport during the interval, then write the trace and restore full Studio UI.
- Do not introduce a second renderer, capture thread, alternate launcher, or UI-only capture state.

## Capture controls follow-up

- ProfilerUi owns transient capture controls: duration in milliseconds and a named resolution preset, defaulting to 4 seconds and 1440p.
- StudioApplication remains the capture lifecycle owner and translates the selected preset to a render-target size. The Profiler button and profiler.viewport_capture RPC call the same start operation.
- StudioViewportPanels owns the temporary target-size override for `studio.primary`; it fits the rendered texture into the fullscreen panel while retaining the render target's aspect ratio. Clear the override on completion/cancel so the normal viewport size is restored.
- Presets are 720p, 1080p, 1440p and 2160p. Do not resize the native Studio window or change persistent view state.
- Capture duration and resolution are ProfilerModel options (not ProfilerUi-local state), so panel_get/panel_set, the button, and MCP observe and change the same settings. The capture start RPC defaults to those model options; explicit duration_ms/resolution override them for one request.
- Profiler capture workload is a shared ProfilerModel option; StudioApplication drives camera-only trajectories through StudioRenderViewSet::RestoreViewPose and restores the original pose on completion/cancel. The UI, panel RPC and MCP start action use the same named scenario. Keep this separate from validation scene preparation and never mutate world/terrain authority.
