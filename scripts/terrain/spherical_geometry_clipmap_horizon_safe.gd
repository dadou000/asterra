extends "res://scripts/terrain/spherical_geometry_clipmap_cached.gd"
## Final horizon-safe terrain layer.
##
## Horizon distance is allowed to choose how many logical rings exist, but it must
## never alter rendered vertex positions or shorten the submitted ring prefix. The
## previous dynamic visible-cap uniform made far spherical projection follow camera
## altitude, so descending could pull the apparent terrain edge toward the player.
## The inherited nadir optimisation also shortened the ring prefix from AGL. Both
## are disabled here without changing the proven cache, handoff, topology or LOD
## process chain.

const RENDER_HEMISPHERE_CAP_RAD: float = PI * 0.5


func _sync_uniforms(origin: Vector3) -> void:
	super._sync_uniforms(origin)
	if _material != null:
		# Keep the shader's geometric projection/cap independent of observer altitude.
		# _visible_cap_arc_m still drives automatic ring-count selection on the CPU.
		_material.set_shader_parameter("u_visible_cap_angle", RENDER_HEMISPHERE_CAP_RAD)


func _update_sector_visibility() -> void:
	super._update_sector_visibility()
	if _view_surface_culled:
		return
	# Preserve azimuth/sky/underground culling, but never let the AGL-dependent
	# nadir footprint remove outer logical rings. A submitted terrain edge must stay
	# safely beyond the geometric horizon and therefore remain unobservable.
	_set_view_ring_prefix(_physical_ring_count)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_horizon_safe"] = true
	out["terrain_render_cap_dynamic"] = false
	out["terrain_nadir_ring_prefix_cull"] = false
	return out
