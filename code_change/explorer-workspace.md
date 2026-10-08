# Explorer workspace treatment

- Existing owners: `StudioExplorerPanel` owns the World hierarchy, selection
  feedback, object context menu, and project asset catalog; `StudioViewportPanels`
  composes its internal source into the single visible Explorer panel.
- Primary insertion points: the Explorer panel draw callback in
  `apps/editor/src/StudioExplorerPanel.cpp` and its composition in
  `engine/studio_ui/src/StudioViewportPanels.cpp`; they use shared `PanelContext`
  controls.
- Canonical state: `ExplorerModel`, `SelectionService`, and `ContentService`;
  the unified tree filter is view state only. World objects and asset categories
  appear beneath expandable roots in one Explorer panel with no separate mode
  switch.
- Preserve drag/drop reparenting, existing context commands, asset categories,
  and the selection driven viewport actions. Do not create another explorer
  store or duplicate command handlers.
- Visual controls use shared EditorUi vector icons and icon-decorated tree/selectable rows with an explicit text gutter;
- Explorer rows and search use opt-in shared-control vertical padding for larger pointer targets without changing other panels;
  ordinary implementation edits use the central Studio generation refresh.

- Context menu extension: keep the existing row menu in `StudioExplorerPanel`; add actions call `CommandService`, remain reachable through `object.create` / `object.delete` RPC and MCP, and select newly-created rows. Protect the actual world root and expose Delete only where the leaf-only delete command succeeds.
