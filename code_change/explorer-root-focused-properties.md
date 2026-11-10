# Explorer root and focused Properties

- Existing owner: `StudioExplorerPanel` renders the authored world hierarchy, while `StudioViewportPanels::InstallQol` contributes workflow controls to the shared Properties provider registry.
- Primary insertion points: render the actual `kWorldType` root directly in the Explorer and nest the protected viewport Camera and Lighting virtual items under it; stop registering the selection workflow provider as a Properties extension.
- Canonical state: authored parentage remains in `ExplorerModel`; virtual Camera/Lighting selection remains in `StudioInspectorTarget`; schema editing remains in `InspectorModel` and selected-type providers.
- Do not add another root model or duplicate schema editing. The Explorer is presentation over existing world records and protected view-owned controls.
