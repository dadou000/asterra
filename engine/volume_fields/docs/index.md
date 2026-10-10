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
  "Tile coordinates are absolute on a cell grid offset by VolumeFieldStorage::GridAnchor() (default zero, world-aligned, which the live solver relies on). A static Coarse/Passive/Baked volume sets the anchor to its own minimum corner (VolumeRepresentationPass) so its tiles cover exactly the domain; the raymarch shader subtracts GpuVolumeParams.gridAnchor before reading tiles and the procedural/baked upload adds it back. Without the anchor a domain not aligned to a tile edge was covered by a window shifted by up to one tile and a baked storm uploaded all zeros. A baked cache keeps the authored field resolution; passive aggregates stay 8^3.",
  "Static (baked) volumes are lit by the same model as the planet clouds (CloudRenderer): sun colour from the body atmosphere transmittance LUT at the sample radius (VolumeAtmosphereLuts, bound as raymarch textures 2 and 3; the CPU sun transmittance at the volume bottom/top is only the no-atmosphere fallback), the sun optical depth from a 24-step shadow march feeding four octaves of multiple scattering, a small sky term, a soft shoulder and a dark flat base. The view-ray/sun cosine is dot(ray, sun) (looking at the sun is forward scattering). Live volumes keep the original lighting.",
  "Static volumes are cheap to draw because of four things that must survive edits: the raymarch runs at half the output resolution (Presentation.marchWidth/marchHeight; composite and copy-back stay full size and sample by uv), rays are clipped to the box that holds density (VolumeFieldStorage::SetOccupiedBounds, filled while the baked field is uploaded, GpuVolumeParams.occupancyMinimum/Maximum), the step is the distance to cross one cell on the ray fastest axis (capped at 128), and the baked field is only refilled and re-uploaded when its key changes (cache fingerprint, domain, anchor, resolution, residency). Measured on the supercell at 256^3: frame 49 ms to 15 ms, VolumeRaymarch 42 ms to 9 ms.",
  "Static clouds are given a type per sample from height above the ground and from the baked (pre-boost) density, in UniversalVolumeRenderer: thin cloud above ~7 km is ice (cirrus: fibres stretched along the storm wind, sharp forward scattering, no dark base), thin cloud at 2-7 km is a mid-level sheet (altocumulus/altostratus: ~1 km cloudlets, faint base), the dark flat base is nimbostratus-like and everything solid stays cumulus/cumulonimbus billows. The heuristic is height/density only because the fast-core solver has no ice and a single condensate field; a real type field needs a solver that produces one. The read-time optical-depth boost (1-(1-d)^3) is applied before typing, so classify on the un-boosted density.",
  "Cloud layers around a storm are separate Volume objects (Cloud Type 1-9 = the cloud_genera table, Baked, own cache from volume.generate_layer / weather_lab CloudLayers). StudioViewportRenderer draws the primary Baked volume (selected, else the first with a cache) through ComposeVolumePasses and every other enabled Baked volume with a cache through ComposeVolumeLayerPasses (own storage, sun lighting from ResolveVolumeSunLighting, no solver or authoring overlays). A layer is typed by its Cloud Type, not by height; ice and sheet layers skip the self-shadow march and cap at 48 steps, and a layer set to Extinction Scale ~1e-3 (ice) to ~1e-2 (puffs) stays translucent, because the storm default 0.8 per unit density turns any layer into an opaque ceiling.",
]
related = ["/rendering/volumes", "/legacy/tree-history-research-v007-universal-volumetrics"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/math", "/rendering/render-graph", "/rendering/rhi", "/rendering/volumes/representation", "/world/world-model"]
used_by = ["/apps/studio", "/editor/studio-ui", "/rendering/volumes/render", "/rendering/volumes/solver"]
verify = [
  "ctest -R Orbit.VolumeFieldStorage",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++



