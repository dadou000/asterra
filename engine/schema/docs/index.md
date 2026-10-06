+++
path = "/authoring/schema"
title = "Schema and property registry"
kind = "subsystem"
status = "stable"
summary = "SchemaRegistry describes every authorable semantic type and property (TypeSchema, PropertySchema, PropertyKind, NumericRange, ObjectReferenceValue). One metadata system is shared by serialization, the property inspector, toolbar enablement, Luau bindings, MCP schemas, undo/redo and validation."
owner_module = "OrbitSchema"
keywords = ["schema", "property", "type", "registry", "reflection", "numeric range", "default value"]
sources = [
  "engine/schema/include/orbit/schema/SchemaRegistry.hpp",
  "engine/schema/CMakeLists.txt"]
symbols = ["ObjectReferenceValue"]
invariants = [
  "Schema owns its own opaque 128-bit object-reference value to avoid a schema -> scene dependency; scene ObjectId has the same representation and command adapters convert explicitly.",
  "The editor does not hard-code a custom property panel for each new engine type: ordinary authoring uses the shared metadata, with a schema editor hook for custom editors (V0.0.3 spec section 11)."]
related = [ "/authoring/scene", "/authoring/commands"]
depends_on = ["/foundation/core", "/foundation/math"]
used_by = ["/apps/studio", "/authoring/commands", "/authoring/plugins", "/authoring/scene", "/editor/model", "/editor/session", "/world/paths", "/world/surface-composition", "/world/world-model"]
verify = [
  "ctest -R Orbit.AuthoringModel"]
verified = "b0a0de7f"
+++


