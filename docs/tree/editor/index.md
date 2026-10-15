+++
path = "/editor"
title = "Editor (Orbit Studio)"
kind = "section"
status = "stable"
summary = """
Orbit Studio is the single unified editor application (Orbit.exe): project browser, scene and \
world authoring, viewports, diagnostics, profiler, build/export. Every capability is one \
command/service reachable from the UI, plugins, RPC and MCP. Start with the viewport for \
camera/navigation/diagnostic-overlay work and with mcp-rpc for automation."""
keywords = ["editor", "studio", "ui", "viewport", "panels", "automation", "rpc", "mcp"]
related = ["/rules/ui", "/rules/placement"]

[routes]
"which Studio UI file/panel owns X, view continuity, high-res capture, flat map, inspector extension" = "studio-ui"
"viewport, navigation, camera, diagnostic overlays, text HUD" = "viewport"
"add an RPC method or an MCP tool, drive Studio from an agent" = "mcp-rpc"
"explorer, inspector, command surfaces, recipes, shortcuts" = "model"
"switching worlds, active body, world list" = "session"
"terrain runtime bridge, rebuild scheduler, project browser, bezier handle drag" = "studio-session"
"ImGui shell, panel extensions, DPI scale, previews" = "ui-toolkit"
"issue reports, capturing a reproducible problem" = "reports"
"debug tab, validation runs, test experiments, diagnostic panels, debugging location" = "/editor/studio-ui/debug-center"
"button/shortcut/toolbar change" = "/rules/placement"
"profiler, hitch capture, performance trace" = "/editor/profiler"
"shading assets, shader contract" = "/rendering/shading"
+++

Studio modules (all under `engine/`): `studio_ui` (panels, viewport renderer, render-view set, RPC for views; `/editor/viewport`),
`studio_session` (`/editor/studio-session`), `editor_session` (`/editor/session`), `editor_model` (`/editor/model`), `editor_ui` (`/editor/ui-toolkit`),
`editor_rpc` (`/editor/mcp-rpc`), `studio_reports` (`/editor/reports`) and the authoring layer in `/authoring`. The executable is `apps/editor` (`/apps/studio`).

The authoring rule is: a UI element **requests**, a domain owner **executes**, and the same
operation is reachable over RPC and MCP (`/editor/mcp-rpc`).


## Explorer selection and hierarchy deletion (Harmony)

The canonical scene Explorer lives in `apps/editor/src/StudioExplorerPanel.cpp`. Scene IDs use the shared `SelectionService`; a click selects one object, Ctrl+click toggles an object, and Shift+click selects a contiguous range in the currently visible tree/search order. Ctrl+Shift+click adds the range to the existing selection. Ctrl+A selects visible scene rows when Explorer has keyboard focus; Delete removes the selected scene subtrees, but never while a text field is active. The toolbar supports Select visible, Clear selection, Select parent, Select children and Select subtree; right-click on an already selected row preserves the existing multi-selection. The world root is protected.

**Deletion is an atomic undoable domain operation, not a UI-specific tree hack.** `CommandService::DeleteObject` calls `DeleteObjects` and recursively deletes all descendants, even when they are nested arbitrarily deep. The command snapshots object records and explicit properties, collapses overlapping selected ancestors/descendants, erases children before parents, and restores parents before children on Undo using original stable IDs and property values. A failed validation leaves every object intact. A multi-selection uses exactly one undo entry. The low-level `ObjectStore::Erase` remains leaf-only; do not bypass the command service.

**Validation:** `tests/ExplorerInspectorTests.cpp` checks overlapping selections, deletion/Undo/Redo, multiple siblings, property restoration and invalid-target atomicity. Source changes are committed; the Windows executable and native CTest remain to be verified.


## Orbit updater status bubble (Harmony)

The **single Studio top-bar updater chip** opens a non-modal detail popover; clicking the chip itself never downloads source or restarts the editor. It displays the tracked `origin/0.1.1-Harmony` branch, verified local and remote short commit IDs, remote-behind commit count, the last successful check time, and the current worker stage. A new-commit state is visually emphasized; active work shows an animated refresh glyph. The popup has separate actions:

- **Check now:** `hot_reload::RequestGitHubCheck()`, exposed also as `studio.github_update_check`. This runs on the existing hot-iteration worker; no project checkpoint, checkout, compilation, or restart. The automatic check runs at startup and every five minutes.
- **Update and restart:** The existing `RequestGitHubUpdate()` command, also used by **Shift+F5**, the Build menu, and `studio.github_update_start`. It checkpoints the project/world, fetches GitHub, checks fast-forward eligibility and dirty working files, skips rebuild/restart when zero new commits exist, and otherwise updates, builds, and stages the replacement Studio. The popover disables the action unless newer commits were verified.
- **Retry build:** When Git has fast-forwarded successfully but building or activating the new executable failed, the worker retains `rebuildPending`. With zero new commits it may retry building without another merge; it must *not* incorrectly report that the currently running binary was updated.
- **Open update log:** Navigates to the existing worker log via the platform file browser. Excessively long Git error output is capped in the status popover but retained in the log.

The status RPC `studio.github_update_status` includes `pending_commits`, `rebuild_pending`, `local_commit`, `remote_commit`, and `last_checked_unix` in addition to the previous status fields. Verified check times reflect a successful Git comparison, not a failed or queued attempt. Git commands are non-interactive and time-bounded; failures leave the editor running. Check requests and updates never create competing Git/build workers.

### Manual verification gate

1. Launch Studio online: confirm automatic status check and recorded local/remote commit hashes. Open and close the chip popover; it must not queue an update.
2. Press **Check now**: stage updates, then one of Up to date / Update available / Blocked / Failed. Verify the project is not saved or Studio restarted by this action.
3. With no new commits, press **Shift+F5**: confirm an informational *Up to date* notification, and no rebuild/restart. The popup install action should be disabled.
4. With newer commits, confirm count, update action, successful fast-forward, rebuild and handoff. Check no duplicate watcher-triggered build is launched.
5. Simulate offline GitHub, wrong branch, divergent history, and dirty tracked worktree; verify clear status/log and unchanged source where applicable.
6. Simulate build and handoff failure **after** a successful fetch/merge; verify the source-updated state exposes **Retry build**, rather than masquerading as an up-to-date running executable.
7. With a delayed Git command, ensure the status worker times out instead of stalling indefinitely; the editor viewport and UI must remain responsive.

**Verification status:** Source changes and manual gate documented; Windows compilation, native execution, and automated UI checks have not been run in this session.
