# Eco mode placement

- Existing owners: `apps/editor/src/Main.cpp` owns frame pacing and `EditorSessionRpcHost` owns RPC request dispatch; `StudioViewportPanels::DrawActivityBand` owns the status strip beside FPS.
- Primary insertion points: add the Eco toggle beside the FPS readout, keep the selected mode in the Studio application, and apply its frame deadlines in the existing main loop.
- Canonical state: one application-owned Eco mode flag, exposed through the same RPC operation used by the status-strip toggle.
- Do not duplicate the pacing policy or make RPC clients rely on mouse/keyboard input. RPC dispatch must wake the platform wait so the next iteration services commands immediately.
