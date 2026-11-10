+++
path = "/editor/session"
title = "World session (EditorWorldSession)"
kind = "subsystem"
status = "stable"
summary = "EditorWorldSession owns every service whose lifetime is scoped to one authoritative .orbitworld document, ActiveBodyModel resolves which body the viewport follows from the semantic selection, and WorldDocumentsModel presents the project's worlds."
owner_module = "OrbitEditorSession"
keywords = ["world session", "session", "switch world", "active body", "body focus", "world documents", "service graph", "generation"]
sources = [
  "engine/editor_session/include/orbit/editor_session/ActiveBodyModel.hpp",
  "engine/editor_session/include/orbit/editor_session/EditorWorldSession.hpp",
  "engine/editor_session/include/orbit/editor_session/WorldDocumentsModel.hpp",
  "engine/editor_session/CMakeLists.txt",
]
symbols = ["ActiveBodyTarget", "EditorWorldSession", "WorldDocumentItem"]
invariants = [
  "Switching worlds constructs a complete candidate session first and then atomically replaces the old service graph; any facade that retains references into world authority (Explorer, Inspector, plugins, command surfaces) lives inside the same state and is destroyed before the services it references.",
  "A monotonic generation changes whenever the runtime celestial representation is replaced or invalidated; consumers that retain FrameGraph/BodyRegistry references must re-fetch on change.",
  "ActiveBodyModel persists no runtime body identity: targets are reconstructed whenever the world session generation or semantic revision changes; it follows the first selected object that belongs to a body, keeps the previous valid target for an unrelated selection and falls back to the first composed body in a fresh session.",
  "WorldDocumentsModel owns no parallel catalogue: every query is reconstructed from ProjectDocument, and invalid or incompatible world files appear as diagnostic entries so one damaged document cannot hide the project's other worlds.",
]
related = ["/authoring/documents", "/editor/model", "/world/universe"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/plugins", "/authoring/scene", "/authoring/schema", "/authoring/selection", "/editor/model", "/world/surface-composition", "/world/world-model"]
used_by = ["/editor/studio-session"]
verify = [
  "ctest -R Orbit.EditorWorldSession",
  "ctest -R Orbit.TransactionalComposition",
  "ctest -R Orbit.WorldDocumentsModel",
  "ctest -R Orbit.ActiveBodyModel",
  "ctest -R Orbit.WorldScopedFacades",
]
verified = "b0a0de7f"
+++


