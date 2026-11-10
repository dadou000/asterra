# Eco mode placement

- Existing owners: `apps/editor/src/Main.cpp` owns frame pacing and `EditorSessionRpcHost` owns RPC request dispatch; `StudioViewportPanels::DrawActivityBand` owns the status strip beside FPS.
- Primary insertion points: add the Eco toggle beside the FPS readout, keep the selected mode in the Studio application, and apply its frame deadlines in the existing main loop.
- Canonical state: one application-owned Eco mode flag, exposed through the same RPC operation used by the status-strip toggle.
- Do not duplicate the pacing policy or make RPC clients rely on mouse/keyboard input. RPC dispatch must wake the platform wait so the next iteration services commands immediately.

## Harmony correction: continuous Eco cap

Eco mode must not infer viewport visibility or user presence from mouse activity. Previously its idle branch forced **1 FPS** after input stopped, while the active branch capped at **15 FPS**. Moving the pointer outside the viewport therefore appeared to stop rendering. The frame loop now applies an approximately **15 FPS** cap continuously whenever Eco is enabled, independent of cursor hover and input. RPC activity bypasses that cap for responsive automation. When Eco is disabled, the separately configured ordinary inactivity limiter still applies.

Verification: enable Eco, leave the mouse still and move it between viewport and inspector, and confirm roughly 15 FPS for both; disable Eco and verify the ordinary idle cap remains unchanged. Check RPC responsiveness and performance trace recording while Eco is enabled. Build/runtime validation has not been performed for this correction.
