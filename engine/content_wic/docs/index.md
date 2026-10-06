+++
path = "/authoring/content-wic"
title = "WIC texture importers (Windows)"
kind = "subsystem"
status = "stable"
summary = "Registers Windows Imaging Component decoders as texture importers that produce Orbit's platform-neutral runtime texture container, and offers a direct decode to RGBA8 for live previews that must see today's file contents."
owner_module = "OrbitContentWic"
keywords = ["wic", "texture import", "png", "jpg", "tga", "decode", "importer", "preview"]
sources = [
  "engine/content_wic/include/orbit/content_wic/WicTextureImporter.hpp",
  "engine/content_wic/CMakeLists.txt",
]
symbols = []
invariants = [
  "The direct RGBA8 decode bypasses the ImporterRegistry and the derived-data cache on purpose: it is for live previews (the Shading tab's texture parameters), never for cooked output.",
]
related = ["/authoring/content", "/legacy/orbit-shading"]
depends_on = ["/authoring/content"]
used_by = ["/rendering/shading"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


