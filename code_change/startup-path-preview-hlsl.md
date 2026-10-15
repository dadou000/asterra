# Startup path-preview HLSL fix

- **Existing owner:** `PathPreviewRenderer` owns the embedded road/path preview shaders.
- **Insertion point:** the fragment shader's lane-divider local in
  `engine/editor_ui/src/PathPreviewRenderer.cpp`.
- **Canonical state:** the shader continues to derive divider intensity from
  `lateral`, `halfWidth`, and `lanes`; no CPU-side or duplicate shader path is
  introduced.
- **Scope:** rename the DXC-reserved local `line` so the existing preview
  pipeline compiles during Studio startup.
