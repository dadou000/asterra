extends "res://scripts/terrain/spherical_geometry_clipmap_occlusion.gd"
## Final production terrain entry point.
##
## Production policy:
## - render one authoritative terrain surface only;
## - keep the expensive staging cache allocation resident while travelling;
## - prewarm that spare with a strict low GPU budget instead of running a second
##   full 12k-sample terrain synthesis workload beside the visible cache.

const BudgetedTerrainClipmapCacheScript := preload(
	"res://scripts/terrain/gpu_terrain_clipmap_cache_budgeted.gd")
const STAGING_IDLE_RELEASE_S := 12.0
const STAGING_KEEP_MOTION_M := 0.25
# Far-ring occlusion performs a full-resolution depth reduction before testing its
# coarse 128x72 tiles. It is temporal, conservative and fail-open, so running it
# every render frame only creates compute/readback pressure at high FPS.
const OCCLUSION_CADENCE_FRAMES: int = 6

var _staging_idle_s := 0.0
var _staging_allocations := 0
var _staging_reuses := 0
var _staging_idle_releases := 0
var _occlusion_cadence_frame: int = OCCLUSION_CADENCE_FRAMES - 1
var _occlusion_enabled_frames: int = 0
var _occlusion_skipped_frames: int = 0


func _ready() -> void:
	super._ready()
	# The cached parent creates its first active/spare pair during _ready(). Replace
	# the active node before the first process update so both future swap roles use
	# the role-aware scheduler. No terrain cache has been populated at this point.
	_replace_initial_active_cache()
	_release_staging_cache()


func _process(dt: float) -> void:
	_update_occlusion_cadence()
	super._process(dt)
	_manage_staging_cache_lifetime(dt)


func _update_occlusion_cadence() -> void:
	if _occlusion_effect == null:
		return
	_occlusion_cadence_frame += 1
	var run_now: bool = _occlusion_cadence_frame >= OCCLUSION_CADENCE_FRAMES
	if run_now:
		_occlusion_cadence_frame = 0
		_occlusion_enabled_frames += 1
	else:
		_occlusion_skipped_frames += 1
	_occlusion_effect.enabled = run_now


func _replace_initial_active_cache() -> void:
	var previous_active: Node = _terrain_cache_active
	if previous_active != null and is_instance_valid(previous_active):
		previous_active.queue_free()
	_terrain_cache_active = BudgetedTerrainClipmapCacheScript.new()
	_terrain_cache_active.name = "GPUTerrainClipmapCacheActiveBudgeted"
	_terrain_cache_active.call("set_staging_budget_mode", false)
	add_child(_terrain_cache_active)
	_force_cache_rebind()


func _set_cache_role(cache: Node, staging: bool) -> void:
	if cache != null and is_instance_valid(cache) \
			and cache.has_method("set_staging_budget_mode"):
		cache.call("set_staging_budget_mode", staging)


func _start_handoff(target_offset: Vector2, retarget: bool) -> void:
	_ensure_staging_cache()
	_set_cache_role(_terrain_cache_staging, true)
	_staging_idle_s = 0.0
	super._start_handoff(target_offset, retarget)


func _cancel_handoff() -> void:
	super._cancel_handoff()
	_staging_idle_s = 0.0
	# _commit_staged_anchor() swaps these pointers before dynamically calling this
	# override. Re-apply the scheduler roles after every swap/cancel.
	_set_cache_role(_terrain_cache_active, false)
	_set_cache_role(_terrain_cache_staging, true)
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
			_set_cache_role(_terrain_cache_staging, true)
			_staging_reuses += 1
			return
		_release_staging_cache()
	_terrain_cache_staging = BudgetedTerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheStagingBudgeted"
	_terrain_cache_staging.call("set_staging_budget_mode", true)
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
## not be implemented as overlapping terrain. Deformation follows normal clipmap LOD.
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


## Debug-only lookup for the actual lattice vertex nearest a looked-at direction.
## It uses the same gnomonic anchor coordinates as the production vertex shader.
## The dense L0 micro lattice is preferred while it is the visible centre surface;
## otherwise the finest active clipmap level that contains the target is used.
func debug_closest_rendered_vertex(target_direction: Vector3) -> Dictionary:
	if Planet.cfg == null or not _have_anchor or target_direction.length_squared() <= 1e-12:
		return {}
	var d: Vector3 = target_direction.normalized()
	var denom: float = d.dot(_anchor_dir)
	if denom <= 0.01:
		return {}
	var radius: float = Planet.cfg.planet_radius
	var target_plane := Vector2(d.dot(_anchor_right), d.dot(_anchor_up)) \
		/ denom * radius

	# The micro mesh encodes quarter-L0 cell coordinates in UV, but its shader still
	# multiplies those UV offsets by the ordinary L0 spacing. Snap to that exact grid.
	if _micro_l0_active and _active_min_level == 0 and debug_level_enabled(0):
		var l0_spacing: float = _base_spacing
		var l0_center := Vector2(
			round(_center_plane.x / l0_spacing) * l0_spacing,
			round(_center_plane.y / l0_spacing) * l0_spacing)
		var relative: Vector2 = target_plane - l0_center
		var micro_radius: float = l0_spacing * MICRO_OUTER_L0_CELLS
		var micro_spacing: float = l0_spacing * MICRO_STEP_L0
		if relative.length() <= micro_radius + micro_spacing * 0.75:
			var snapped_relative := Vector2(
				round(relative.x / micro_spacing) * micro_spacing,
				round(relative.y / micro_spacing) * micro_spacing)
			if snapped_relative.length() <= micro_radius + micro_spacing * 0.75:
				var snapped_plane: Vector2 = l0_center + snapped_relative
				var vertex_dir: Vector3 = _gnomonic_direction_for_offset(
					_anchor_dir, _anchor_right, _anchor_up, snapped_plane, radius)
				return {
					"dir": vertex_dir,
					"plane_m": snapped_plane,
					"level": 0,
					"spacing_m": micro_spacing,
					"micro": true,
					"target_error_m": snapped_plane.distance_to(target_plane),
				}

	var selected_level: int = -1
	var selected_spacing: float = _base_spacing
	var selected_center := Vector2.ZERO
	var selected_cell := Vector2.ZERO
	for level: int in range(_active_min_level, _active_max_level + 1):
		if not debug_level_enabled(level):
			continue
		var spacing: float = _base_spacing * pow(2.0, float(level))
		var level_center := Vector2(
			round(_center_plane.x / spacing) * spacing,
			round(_center_plane.y / spacing) * spacing)
		var cell: Vector2 = (target_plane - level_center) / spacing
		if cell.length() <= float(HALF_CELLS) + 1e-4:
			selected_level = level
			selected_spacing = spacing
			selected_center = level_center
			selected_cell = cell
			break
	if selected_level < 0:
		return {}

	# Search a tiny neighbourhood because simply rounding a point near a circular
	# disc/annulus edge can select a grid coordinate that is not referenced by any
	# rendered cell. The 5x5 search remains debug-only and runs at low cadence.
	var rounded_cell := Vector2i(roundi(selected_cell.x), roundi(selected_cell.y))
	var best_cell := rounded_cell
	var best_distance_sq: float = INF
	var found := false
	for oy: int in range(-2, 3):
		for ox: int in range(-2, 3):
			var candidate_i := Vector2i(rounded_cell.x + ox, rounded_cell.y + oy)
			var candidate := Vector2(float(candidate_i.x), float(candidate_i.y))
			var candidate_radius: float = candidate.length()
			if candidate_radius > float(HALF_CELLS) + 1.0:
				continue
			if selected_level > _active_min_level \
					and candidate_radius < float(RING_INNER_HALF_CELLS) - 1.0:
				continue
			var distance_sq: float = candidate.distance_squared_to(selected_cell)
			if distance_sq < best_distance_sq:
				best_distance_sq = distance_sq
				best_cell = candidate_i
				found = true
	if not found:
		return {}

	var snapped_plane: Vector2 = selected_center \
		+ Vector2(float(best_cell.x), float(best_cell.y)) * selected_spacing
	var vertex_dir: Vector3 = _gnomonic_direction_for_offset(
		_anchor_dir, _anchor_right, _anchor_up, snapped_plane, radius)
	return {
		"dir": vertex_dir,
		"plane_m": snapped_plane,
		"level": selected_level,
		"spacing_m": selected_spacing,
		"micro": false,
		"target_error_m": snapped_plane.distance_to(target_plane),
	}


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	var staging_allocated: bool = _terrain_cache_staging != null \
		and is_instance_valid(_terrain_cache_staging)
	out["terrain_cache_architecture"] = "budgeted_retained_spare_toroidal_gpu"
	out["terrain_cache_staging_allocated"] = staging_allocated
	out["terrain_cache_resident_buffers"] = 2 if staging_allocated else 1
	out["terrain_cache_peak_buffers"] = 2
	out["terrain_cache_staging_idle_s"] = _staging_idle_s
	out["terrain_cache_staging_allocations"] = _staging_allocations
	out["terrain_cache_staging_reuses"] = _staging_reuses
	out["terrain_cache_staging_idle_releases"] = _staging_idle_releases
	out["terrain_cache_staging_sample_budget"] = 2048
	out["terrain_occlusion_cadence_frames"] = OCCLUSION_CADENCE_FRAMES
	out["terrain_occlusion_enabled_frames"] = _occlusion_enabled_frames
	out["terrain_occlusion_skipped_frames"] = _occlusion_skipped_frames
	out["deformation_overlay"] = false
	out["deformation_overlay_visible"] = false
	out["deformation_render_path"] = "authoritative_clipmap_lod"
	return out
