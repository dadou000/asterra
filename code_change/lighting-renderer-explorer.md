# Protected Lighting / Renderer Explorer element

- Existing owners: `StudioRenderViewSet` owns per-view lighting contribution,
  GI and anti-aliasing layer
  state; `StudioViewportRenderer` owns display and post-process renderer state;
  `DisplayDiagnosticsUi` presents the existing renderer controls.
- Primary insertion points: add a protected virtual Lighting tree
  node and focused settings children in `StudioExplorerPanel`; route selections
  through the shared inspector target in `StudioPropertiesPanel`.
- Canonical state: per-view `StudioRenderViewSet` layer options and
  `StudioViewportRenderer` display configuration. Explorer nodes are
  presentation only and are never world objects.
- Reuse `DisplayDiagnosticsUi` for the complete existing renderer settings
  surface. Do not duplicate renderer state or expose a delete/reparent path.
  GI, direct-lighting, reflection, atmosphere, cloud and surface controls use
  existing `view.terrain_layers_*` RPC/MCP. Lighting children expose these
  groups in the Properties panel; Renderer Diagnostics exposes display and
  lighting runtime controls.
  Eye Adaptation is a Camera child, not part of Lighting. Native code changes use central Studio
  generation refresh.
