# Geological authoring usability

- Existing owner: `studio_ui::SurfaceAuthoringUi` presents geology authoring; `editor_model::SurfaceAuthoringModel` remains the authority for saved project edits and undo.
- Improve the current panel in place: make the Plate Recipe location accurate, add planet-aware starter drafts and a reload action for long TOML histories, retain unsaved drafts per surface across selection changes, and keep examples separate from the explicit Save operation. Stratigraphy examples use the selected body's existing geological materials.
- Do not add a second geology UI model or save path. Template buttons only prepare drafts; the existing model/RPC operations continue to validate, commit and invalidate.
- Verify the Studio build, authoring model/UI tests and docs index after the edit.
