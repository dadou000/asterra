extends "res://scripts/terrain/spherical_geometry_clipmap_procedural_safe.gd"
## Final visual binding for the resident whole-planet terrain architecture.
##
## The procedural/safe parents own concentric geometry, debug tools and GPU
## detail. This layer binds the immutable global height and material-control
## textures; neither resource recentres or streams while the camera moves.
##
## Radius invariant: every active LOD level is evaluated around exactly
## Planet.cfg.planet_radius. The legacy annulus sink is disabled here because a
## per-level radial offset turns the clipmap into nested shells at the horizon.

var _bound_global_material: Texture2DArray
var _bound_global_material_res: int = 0


func _ready() -> void:
	super._ready()
	# Keep the legacy debug value at zero as an additional guard. The actual
	# radius invariant is enforced by clearing the annulus marker below.
	_debug_sink_scale = 0.0
	_sync_debug_uniforms()
	_enforce_shared_planet_radius()
	_bind_gpu_resources(true)
	_sync_material_control()


func _bind_gpu_resources(force: bool) -> void:
	if _material == null:
		return
	var macro: Texture2DArray = Planet.global_height_texture if Planet.ready_state else null
	var macro_res: int = Planet.global_height_face_res if Planet.ready_state else 0
	if force or macro != _bound_orbit:
		_bound_orbit = macro
		_material.set_shader_parameter("u_macro_elevation", macro)
	if force or macro_res != _bound_orbit_res:
		_bound_orbit_res = macro_res
		_material.set_shader_parameter("u_macro_face_res", float(macro_res))
	_material.set_shader_parameter("u_macro_ready", 1.0 if macro != null else 0.0)


## Override the old camera-centred material clipmap binding. The parent calls this
## every frame, but after the first reference comparison it only updates two tiny
## readiness scalars; no image construction/upload is possible here.
func _sync_material_control() -> void:
	if _material == null:
		return
	var texture: Texture2DArray = Planet.global_material_texture \
		if Planet.ready_state and Planet.global_material_texture != null else null
	var face_res: int = Planet.global_material_face_res if Planet.ready_state else 0
	if texture != _bound_global_material:
		_bound_global_material = texture
		_material.set_shader_parameter("u_material_global", texture)
	if face_res != _bound_global_material_res:
		_bound_global_material_res = face_res
		_material.set_shader_parameter("u_material_global_res", float(face_res))
	_material.set_shader_parameter("u_material_global_ready", 1.0 if texture != null else 0.0)
	# Prevent accidental fallback to the obsolete three-layer moving control map.
	_material.set_shader_parameter("u_material_clipmap_ready", 0.0)


## The compact parent stores a Z=1 marker in annular instance custom data solely
## to request the old radial sink in the UV shader. Re-write that marker to zero
## whenever the active LOD window changes. X remains the logical L level, so
## spacing/mip/detail selection is unchanged; only the false per-LOD radius offset
## is removed.
func _apply_active_level_window() -> void:
	super._apply_active_level_window()
	_enforce_shared_planet_radius()


func _enforce_shared_planet_radius() -> void:
	var ring_count: int = _active_ring_count()
	for sector: int in SECTOR_COUNT:
		if sector >= _sector_batches.size():
			break
		var rings: MultiMeshInstance3D = _sector_batches[sector]
		if rings.multimesh == null:
			continue
		for instance_index: int in ring_count:
			var logical_level: int = _active_min_level + instance_index + 1
			rings.multimesh.set_instance_custom_data(instance_index,
				Color(float(logical_level), float(sector), 0.0, 0.0))


func set_debug_sink_scale(_value: float) -> void:
	# Radial sinking is intentionally unavailable on the production global
	# clipmap. LODs may change sampling frequency, never the reference sphere.
	_debug_sink_scale = 0.0
	_sync_debug_uniforms()


func debug_sink_scale() -> float:
	return 0.0


## Side-cut inspection hides entire sector nodes. Keep their MultiMesh instance
## windows populated while hidden so disabling the cut cannot leave half of the
## dynamic LOD rings at visible_instance_count=0 until the next altitude change.
func _update_sector_visibility() -> void:
	super._update_sector_visibility()
	_restore_dynamic_ring_window()


func _show_all_active_sectors() -> void:
	super._show_all_active_sectors()
	_restore_dynamic_ring_window()


func _restore_dynamic_ring_window() -> void:
	var ring_count: int = _active_ring_count()
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = ring_count


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["coverage_ready"] = _bound_orbit != null
	out["global_heightmap"] = true
	out["global_face_res"] = Planet.global_height_face_res if Planet.ready_state else 0
	out["global_materialmap"] = true
	out["global_material_face_res"] = Planet.global_material_face_res if Planet.ready_state else 0
	out["material_streaming"] = false
	out["terrain_streaming"] = false
	out["visual_pages"] = false
	out["shared_planet_radius"] = true
	out["debug_sink_scale"] = 0.0
	return out
