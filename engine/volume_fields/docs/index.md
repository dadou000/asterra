+++
path = "/rendering/volumes/fields"
title = "Volume field storage"
kind = "subsystem"
status = "stable"
summary = "VolumeFieldStorage is the tiled GPU allocation backend for volume channels: tile coordinates and slots, residency updates, diagnostics and imported fields."
owner_module = "OrbitVolumeFields"
keywords = ["volume fields", "tiles", "residency", "channels", "volume storage", "m31"]
sources = [
  "engine/volume_fields/include/orbit/volume_fields/VolumeFieldStorage.hpp",
  "engine/volume_fields/CMakeLists.txt",
]
symbols = ["FieldChannelInfo"]
invariants = [
  "The M31 storage implementation is retained as the allocation backend for the M36 representation policy.",
]
related = ["/rendering/volumes", "/legacy/tree-history-research-v007-universal-volumetrics"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/math", "/rendering/render-graph", "/rendering/rhi", "/rendering/volumes/representation", "/world/world-model"]
used_by = ["/apps/studio", "/editor/studio-ui", "/rendering/volumes/render", "/rendering/volumes/solver"]
verify = [
  "ctest -R Orbit.VolumeFieldStorage",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


