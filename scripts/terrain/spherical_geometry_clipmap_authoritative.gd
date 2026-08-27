extends "res://scripts/terrain/spherical_geometry_clipmap_occlusion.gd"
## Final production terrain entry point.
##
## Two production policies live here:
## 1. The expensive ~60 MiB staging terrain cache is retained while the observer is
##    travelling and released only after an idle grace period. Sustained travel no
##    longer allocates/frees that cache at every handoff.
## 2. Live GPU terrain deformation gets its own deformation-centred 0.25 m geometry
##    overlay. A crater therefore keeps the same mesh sampling when it leaves the
##    camera-centred micro patch instead of abruptly degrading to ~0.75 m L0.

const STAGING_IDLE_RELEASE_S := 12.0
const STAGING_KEEP_MOTION_M := 0.25

const DEFORM_OVERLAY_GRID_CELLS := 256
const DEFORM_OVERLAY_GRID_VERTS := DEFORM_OVERLAY_GRID_CELLS + 1
const DEFORM_OVERLAY_HALF_CELLS := DEFORM_OVERLAY_GRID_CELLS >> 1
const DEFORM_OVERLAY_SPACING_M := 0.25
const DEFORM_OVERLAY_BIAS_M := 0.006
const DEFORM_OVERLAY_DRAW_DISTANCE_M := 650.0

var _staging_idle_s := 0.0
var _staging_allocations := 0
var _staging_reuses := 0
var _staging_idle_releases := 0

var _deformation_overlay: MultiMeshInstance3D
var _deformation_overlay_visible := false
var _deformation_overlay_distance_m := INF


func _ready() -> void:
	super._ready()
	# Preserve the original low steady-state VRAM behavior at startup. Once travel
	# actually needs a spare cache, retain that spare until motion has been quiet for
	# long enough that releasing it cannot cause allocation churn.
	_release_staging_cache()
	_build_deformation_overlay()
	_sync_deformation_overlay()


func _process(dt: float) -> void:
	super._process(dt)
	_manage_staging_cache_lifetime(dt)
	_sync_deformation_overlay()


func _start_handoff(target_offset: Vector2, retarget: bool) -> void:
	_ensure_staging_cache()
	_staging_idle_s = 0.0
	super._start_handoff(target_offset, retarget)


func _cancel_handoff() -> void:
	super._cancel_handoff()
	_staging_idle_s = 0.0
	# A failed spare is useless and should not be retained. A healthy spare remains
	# resident so the next fast-travel handoff reuses the allocation.
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
	# _last_motion_m is measured in the stable terrain tangent frame by the cached
	# renderer. Keep the spare indefinitely during sustained motion, then release it
	# only after a long quiet interval.
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


func _build_deformation_overlay() -> void:
	if _deformation_overlay != null:
		return
	var bounds := AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))
	_deformation_overlay = _make_batch(
		"ActiveTerrainDeformationSurface", _build_deformation_overlay_mesh(), 1, bounds)
	# x = logical terrain LOD, y = deformation-overlay flag, z = ring flag,
	# w = micro-like surface (avoids the base L0 overlap sink).
	_deformation_overlay.multimesh.set_instance_custom_data(0, Color(0.0, 1.0, 0.0, 1.0))
	_deformation_overlay.visible = false
	_deformation_overlay.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	add_child(_deformation_overlay)
	if _material != null:
		_material.set_shader_parameter("u_deformation_overlay_spacing_m", DEFORM_OVERLAY_SPACING_M)
		_material.set_shader_parameter("u_deformation_overlay_bias_m", DEFORM_OVERLAY_BIAS_M)


func _sync_deformation_overlay() -> void:
	if _deformation_overlay == null or _material == null or Planet.cfg == null \
			or not Planet.ready_state or not _have_anchor:
		_set_deformation_overlay_visible(false)
		return
	var deformation: Node = get_node_or_null("/root/TerrainDeformationGPU")
	if deformation == null or not bool(deformation.get("ready_state")) \
			or bool(deformation.get("failed")) \
			or not bool(deformation.get("_has_active_content")):
		_set_deformation_overlay_visible(false)
		return

	var center_value: Variant = deformation.get("center_dir")
	if not (center_value is Vector3):
		_set_deformation_overlay_visible(false)
		return
	var deform_dir: Vector3 = (center_value as Vector3).normalized()
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_deformation_overlay_visible(false)
		return
	var camera_world: Vec3D = Frames.to_world(camera.global_position)
	if camera_world.length_sq() <= 1.0:
		_set_deformation_overlay_visible(false)
		return
	var camera_dir: Vector3 = camera_world.normalized().to_v3()
	_deformation_overlay_distance_m = acos(clampf(camera_dir.dot(deform_dir), -1.0, 1.0)) \
		* Planet.cfg.planet_radius
	if _deformation_overlay_distance_m > DEFORM_OVERLAY_DRAW_DISTANCE_M \
			or not _terrain_visible or _view_surface_culled:
		_set_deformation_overlay_visible(false)
		return

	var denom: float = deform_dir.dot(_anchor_dir)
	if denom <= 0.01:
		_set_deformation_overlay_visible(false)
		return
	var scale_m: float = Planet.cfg.planet_radius / denom
	var center_plane := Vector2(
		deform_dir.dot(_anchor_right) * scale_m,
		deform_dir.dot(_anchor_up) * scale_m)
	_material.set_shader_parameter("u_deformation_overlay_center_plane", center_plane)
	_material.set_shader_parameter("u_deformation_overlay_spacing_m", DEFORM_OVERLAY_SPACING_M)
	_material.set_shader_parameter("u_deformation_overlay_bias_m", DEFORM_OVERLAY_BIAS_M)
	_set_deformation_overlay_visible(true)


func _set_deformation_overlay_visible(value: bool) -> void:
	_deformation_overlay_visible = value
	if _deformation_overlay != null:
		_deformation_overlay.visible = value


func _set_visible(value: bool) -> void:
	super._set_visible(value)
	if not value:
		_set_deformation_overlay_visible(false)


func rebuild_static_topology() -> void:
	super.rebuild_static_topology()
	if _deformation_overlay != null and _deformation_overlay.multimesh != null:
		_deformation_overlay.multimesh.mesh = _build_deformation_overlay_mesh()


static func _build_deformation_overlay_mesh() -> ArrayMesh:
	var vertices: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var indices := PackedInt32Array()
	var remap: Dictionary = {}
	var radius_sq: float = float(DEFORM_OVERLAY_HALF_CELLS * DEFORM_OVERLAY_HALF_CELLS)
	var terrain_grid_center: float = float(GRID_CELLS) * 0.5
	for y: int in DEFORM_OVERLAY_GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(DEFORM_OVERLAY_HALF_CELLS)
		for x: int in DEFORM_OVERLAY_GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(DEFORM_OVERLAY_HALF_CELLS)
			if cx * cx + cy * cy > radius_sq:
				continue
			var i00: int = _deformation_overlay_vertex(
				remap, vertices, uvs, x, y, terrain_grid_center)
			var i10: int = _deformation_overlay_vertex(
				remap, vertices, uvs, x + 1, y, terrain_grid_center)
			var i01: int = _deformation_overlay_vertex(
				remap, vertices, uvs, x, y + 1, terrain_grid_center)
			var i11: int = _deformation_overlay_vertex(
				remap, vertices, uvs, x + 1, y + 1, terrain_grid_center)
			indices.append_array(PackedInt32Array([i00, i10, i11, i00, i11, i01]))
	var mesh := ArrayMesh.new()
	if indices.is_empty():
		return mesh
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array(vertices)
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array(uvs)
	arrays[Mesh.ARRAY_INDEX] = indices
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _deformation_overlay_vertex(remap: Dictionary, vertices: Array[Vector3],
		uvs: Array[Vector2], gx: int, gy: int, terrain_grid_center: float) -> int:
	var logical_index: int = gy * DEFORM_OVERLAY_GRID_VERTS + gx
	var existing: Variant = remap.get(logical_index, null)
	if existing != null:
		return int(existing)
	var local_index: int = vertices.size()
	remap[logical_index] = local_index
	vertices.append(Vector3.ZERO)
	uvs.append(Vector2(
		terrain_grid_center + float(gx - DEFORM_OVERLAY_HALF_CELLS),
		terrain_grid_center + float(gy - DEFORM_OVERLAY_HALF_CELLS)))
	return local_index


## Snapshot of the exact geometry inputs currently used by the production clipmap.
## RenderedTerrainContactQuery is retained as a diagnostic path; rigid contact now
## uses the stable world-space contact query instead of camera-dependent LOD state.
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
	out["deformation_overlay"] = _deformation_overlay != null
	out["deformation_overlay_visible"] = _deformation_overlay_visible
	out["deformation_overlay_spacing_m"] = DEFORM_OVERLAY_SPACING_M
	out["deformation_overlay_radius_m"] = DEFORM_OVERLAY_SPACING_M \
		* float(DEFORM_OVERLAY_HALF_CELLS)
	out["deformation_overlay_distance_m"] = _deformation_overlay_distance_m
	return out
