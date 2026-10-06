+++
path = "/authoring/documents"
title = "Project and world documents"
kind = "subsystem"
status = "stable"
summary = "ProjectDocument and ProjectManifest own the project on disk (Project.orbit.toml) and its authoritative .orbitworld documents under Worlds/; WorldDatabase is the SQLite store of one world with atomic transactions and WAL control."
owner_module = "OrbitDocuments"
keywords = ["project", "world", "document", "manifest", "orbitworld", "sqlite", "wal", "startup world", "project.orbit.toml"]
sources = [
  "engine/documents/include/orbit/documents/ProjectDocument.hpp",
  "engine/documents/include/orbit/documents/ProjectManifest.hpp",
  "engine/documents/include/orbit/documents/WorldDatabase.hpp",
  "engine/documents/CMakeLists.txt",
]
symbols = ["WorldDescriptor", "PluginRequirement", "WorldDatabase"]
invariants = [
  "The world catalogue is read from the world documents themselves, never from a parallel editor cache.",
  "Creating a world never overwrites an existing document; display-metadata changes do not rename or replace the authoritative document.",
  "Project metadata and startup-world changes persist Project.orbit.toml atomically; ProjectId and project directory identity never change.",
  "Authoritative world listing excludes derived data, runtime saves and symlink targets and is stable and lexically ordered.",
  "WorldDatabase transactions are atomic: if the callback throws the RAII transaction rolls back and the exception propagates.",
]
related = ["/legacy/v0-0-3-spec/10-project-and-document-format", "/authoring/scene"]
depends_on = ["/foundation/core"]
used_by = ["/apps/build-cli", "/apps/build-service", "/apps/player", "/apps/studio", "/authoring/cooked-project", "/authoring/plugins", "/authoring/scene", "/editor/model", "/editor/session", "/editor/studio-session", "/rendering/lighting/radiance-cache", "/world/paths", "/world/surface-composition", "/world/world-model"]
verify = [
  "ctest -R Orbit.Documents",
  "ctest -R Orbit.AuthoringModel",
]
verified = "b0a0de7f"
+++


