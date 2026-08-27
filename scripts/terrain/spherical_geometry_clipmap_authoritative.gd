extends "res://scripts/terrain/spherical_geometry_clipmap_occlusion.gd"
## Final production terrain entry point.
##
## Adds lazy lifetime management to the inherited double-buffered terrain cache.
## The active cache is always resident. The spare cache exists only while a
## reanchor is being prewarmed, reducing steady terrain-cache VRAM by ~60 MiB
## while preserving the parent's atomic warm-cache swap behavior.

func _ready() -> void:
	super._ready()
	_release_staging_cache()


func _start_handoff(target_offset: Vector2, retarget: bool) -> void:
	_ensure_staging_cache()
	super._start_handoff(target_offset, retarget)


func _cancel_handoff() -> void:
	super._cancel_handoff()
	_release_staging_cache()


func _ensure_staging_cache() -> void:
	if _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging):
		return
	_terrain_cache_staging = TerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheStaging"
	add_child(_terrain_cache_staging)


func _release_staging_cache() -> void:
	if _terrain_cache_staging == null:
		return
	if is_instance_valid(_terrain_cache_staging):
		_terrain_cache_staging.queue_free()
	_terrain_cache_staging = null


## Snapshot of the exact geometry inputs currently used by the production clipmap.
## A contact query consumes the same active cache texture, anchor/lattice state and
## surface bias as the vertex shader instead of regenerating terrain independently.
func rendered_contact_sample_params() -> Dictionary:
	if Planet.cfg == null or _terrain_cache_active == null \
			or not is_instance_valid(_terrain_cache_active):
		return {}
	var cache_texture: Variant = _terrain_cache_active.call("texture")
	if cache_texture == null:
		return {}
	var surface_bias := 0.035
	if _material != null:
		var bias_value: Variant = _material.get_shader_parameter("u_surface_bias")
		if bias_value is float:
			surface_bias = float(bias_value)
	return {
		"cache_texture": cache_texture,
		"cache_ready": bool(_terrain_cache_active.call("cache_ready")),
		"cache_generation": int(_terrain_cache_active.call("anchor_generation")),
		"cache_res": int(_terrain_cache_active.call("cache_resolution")),
		"anchor_dir": _anchor_dir,
		"anchor_right": _anchor_right,
		"anchor_up": _anchor_up,
		"lattice_center_plane": _center_plane,
		"base_spacing": _base_spacing,
		"grid_cells": float(GRID_CELLS),
		"active_min": _active_min_level,
		"active_max": _active_max_level,
		"surface_bias": surface_bias,
	}


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_cache_architecture"] = "lazy_double_buffered_toroidal_gpu"
	out["terrain_cache_staging_allocated"] = _terrain_cache_staging != null \
		and is_instance_valid(_terrain_cache_staging)
	out["terrain_cache_steady_buffers"] = 1
	out["terrain_cache_peak_buffers"] = 2
	return out
