+++
path = "/rendering/volumes/representation"
title = "Volume representation, caches and outputs"
kind = "subsystem"
status = "stable"
summary = "VolumeRepresentationService decides how each authored Volume is represented (explicit mode or the Auto policy); VolumeCache bakes and loads cache files; VolumeOutputRuntime/Coupling turn volumes into transport-neutral particle spawn and surface-deposit requests."
owner_module = "OrbitVolumeRepresentation"
keywords = ["volume representation", "volume cache", "bake", "output coupling", "particle spawn", "surface deposit", "representation policy", "m36", "m37", "m38"]
sources = [
  "engine/volume_representation/include/orbit/volume_representation/VolumeCache.hpp",
  "engine/volume_representation/include/orbit/volume_representation/VolumeOutputCoupling.hpp",
  "engine/volume_representation/include/orbit/volume_representation/VolumeOutputRuntime.hpp",
  "engine/volume_representation/include/orbit/volume_representation/VolumeRepresentation.hpp",
  "engine/volume_representation/CMakeLists.txt",
]
symbols = ["VolumeCacheBakeSettings", "VolumeOutputSettings", "VolumeOutputRuntimeDiagnostics", "VolumeRepresentationSettings"]
invariants = [
  "Object is semantic identity; the host runtime resolves its frame-space position through an actor/transform adapter, so this module depends on no game-specific transform type.",
  "Volume outputs advance exactly once per authored Volume from simulation time, not render/UI cadence; at M38 a current M37 cache is the CPU-readable authority and stale caches are rejected rather than silently emitting from authored state that no longer matches the world.",
  "The volume layer owns only the transport-neutral classification of a surface deposit; material and render systems decide how each channel changes BRDF, albedo, thermal state or particles.",
  "Particle systems, terrain, meshes and wetness systems consume the published requests without making the volume solver depend on any one presentation backend.",
  "Output rejection sampling is explicitly bounded even when only a tiny fraction of a volume is above the output threshold.",
  "The pending request batch is handed to its consumer in one transfer that atomically leaves the queue empty, so requests are never observed twice.",
]
related = ["/rendering/volumes", "/legacy/research-v007-universal-volumetrics"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/math", "/foundation/time"]
used_by = ["/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/solver"]
verify = [
  "ctest -R Orbit.VolumeRepresentation",
]
verified = "b0a0de7f"
+++


