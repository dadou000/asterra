# Protected viewport camera element

- Existing owners: `StudioRenderViewSet` owns per-view camera zoom/lens output;
  `StudioViewportRenderer` owns per-view eye-adaptation configuration;
  `StudioExplorerPanel` and `StudioPropertiesPanel` own presentation only.
- Primary insertion points: add the protected virtual camera in the Explorer;
  use a shared app-level inspector target so Properties can present the
  selected virtual element; route camera lens changes through
  `StudioRenderViewSet`; keep Eye Adaptation as an artistic child of Camera.
  Renderer settings such as GI and anti-aliasing are grouped under the
  separate protected Lighting node (`lighting-renderer-explorer.md`).
- Canonical state: the primary view (`studio.primary`) camera and renderer
  state. The Explorer row is a view-owned virtual element, not a world object.
- Preserve the world selection model, render view refresh authority, eye
  adaptation renderer, existing view zoom and display eye RPCs. The virtual
  camera cannot be deleted, duplicated, reparented, or serialized as a world
  object. Its lens controls remain available through RPC and dedicated MCP
  tools; Eye Adaptation is exposed as a Camera child and via `display.eye_*`.
- Native UI changes follow the central Studio generation refresh; there is no
  new renderer or GPU-resource boundary.
