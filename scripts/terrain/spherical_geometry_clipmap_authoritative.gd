extends "res://scripts/terrain/spherical_geometry_clipmap_occlusion.gd"
## Final production terrain entry point.
##
## Production policy:
## - keep the expensive ~60 MiB staging cache resident while the observer is
##   travelling, then release it after an idle grace period;
## - render live deformation through the existing authoritative clipmap only.
##
## Do NOT draw a second contact/deformation terrain mesh over the production
## clipmap. The previous 256x256 overlay duplicated ~100k terrain triangles with
## the full terrain shader and shadows, caused severe overdraw/z-fighting, and made
## the same terrain field appear as two incoherent surfaces.

const STAGING_IDLE_RELEASE_S := 12.0
const STAGING_KEEP_MOTION_M := 0.25

var _staging_idle_s := 0.0
var _staging_allocations := 0
var _staging_reuses := 0
var _staging_idle_releases := 0


func _ready() -> void:
	super._ready()
	# Preserve low steady-state VRAM at startup. Once travel needs a spare cache,
	# retain that spare until motion has been quiet long enough to avoid churn.
	_release_staging_cache()


func _process(dt: float) -> void:
	super._process(dt)
	_manage_staging_cache_lifetime(dt)


func _start_handoff(target_offset: Vector2, retarget: bool) -> void:
	_ensure_staging_cache()
	_staging_idle_s = 0.0
	super._start_handoff(target_offset, retarget)


func _cancel_handoff() -> void:
	super._cancel_handoff()
	_staging_idle_s = 0.0
	# A failed spare is useless. A healthy spare remains resident for reuse.
	if _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging) \
			and bool(_terrain_cache_staging.get("failed")):
		_release_staging_cache()


func _manage_staging_cache_lifetime(dt: float) -> void:
	if _terrain_cache_staging == null or not is_instance_valid(_terrain_cache_staging):
		_staging_idle_s = 0.0
		return
	if _handoff_active:
		_staging_idle_s = 0.0
		return
	if _last_motion_m > STAGING_KEEP_MOTION_M:
		_staging_idle_s = 0.0
		return
	_staging_idle_s += maxf(dt, 0.0)
	if _staging_idle_s >= STAGING_IDLE_RELEASE_S:
		_staging_idle_releases += 1
		_release_staging_cache()


func _ensure_staging_cache() -> void:
	if _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging):
		if not bool(_terrain_cache_staging.get("failed")):
			_staging_reuses += 1
			return
		_release_staging_cache()
	_terrain_cache_staging = TerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheStaging"
	add_child(_terrain_cache_staging)
	_staging_allocations += 1


func _release_staging_cache() -> void:
	if _terrain_cache_staging == null:
		return
	if is_instance_valid(_terrain_cache_staging):
		_terrain_cache_staging.queue_free()
	_terrain_cache_staging = null
	_staging_idle_s = 0.0


## Compatibility hook used by the deformation experiment. Contact refinement must
## not be implemented as overlapping terrain. A future contact-centred refinement
## patch must replace/hole-punch the base topology rather than draw on top of it.
func set_contact_overlay(_direction: Vector3, _enabled: bool) -> void:
	pass


## Snapshot of the exact geometry inputs currently used by the production clipmap.
## RenderedTerrainContactQuery remains diagnostic; rigid contact uses the stable
## world-space physical query rather than camera-dependent LOD state.
func rendered_contact_sample_params() -> Dictionary:
	if Planet.cfg == null or _terrain_cache_active == null \
			or not is_instance_valid(_terrain_cache_active):
		return {}
	var cache_texture: Variant = _terrain_cache_active.call("texture")
	if cache_texture == null:
		return {}
	var surface_bias: float = 0.035
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
	var staging_allocated: bool = _terrain_cache_staging != null \
		and is_instance_valid(_terrain_cache_staging)
	out["terrain_cache_architecture"] = "lazy_retained_spare_toroidal_gpu"
	out["terrain_cache_staging_allocated"] = staging_allocated
	out["terrain_cache_resident_buffers"] = 2 if staging_allocated else 1
	out["terrain_cache_peak_buffers"] = 2
	out["terrain_cache_staging_idle_s"] = _staging_idle_s
	out["terrain_cache_staging_allocations"] = _staging_allocations
	out["terrain_cache_staging_reuses"] = _staging_reuses
	out["terrain_cache_staging_idle_releases"] = _staging_idle_releases
	out["deformation_overlay"] = false
	out["deformation_overlay_visible"] = false
	out["deformation_render_path"] = "authoritative_clipmap_only"
	return out
