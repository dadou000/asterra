extends "res://scripts/terrain/spherical_geometry_clipmap_global.gd"
## GPU synthesis extension for the current 0.0.5 resident global clipmap.
##
## This class deliberately inherits the latest global renderer instead of
## replacing its topology/LOD implementation. Therefore the following remain
## authoritative and unchanged:
##   - compact UV-addressed centre/ring sectors,
##   - screen-space promotion of the centre disc through L0..L14,
##   - per-level stationary lattice snapping,
##   - double-precision stable anchor reconstruction,
##   - resident Planet.global_height_texture macro terrain,
##   - resident Planet.global_material_texture fallback,
##   - dynamic ring visible-instance restoration after sector/debug culling.
##
## GPU geomorph/material/context integration is layered on top of this class in
## the shader/controller passes; no legacy clipmap implementation is copied here.


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	var context: Node = get_node_or_null("/root/PlanetContext")
	out["gpu_geomorph_latest_clipmap_base"] = true
	out["gpu_context_ready"] = context != null and bool(context.get("ready_state"))
	out["gpu_context_generation"] = int(context.get("generation")) if context != null else -1
	return out
