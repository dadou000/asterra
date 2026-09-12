extends Node3D
## Local visual coat for sparse inland water and flooding.
##
## This consumes WaterSurfaceResources only. It has no reference to the sparse
## scheduler/pool and therefore cannot make simulation residency camera-dependent.
## The production gate defaults OFF until its renderer smoke test is run locally.

signal render_enabled_changed(enabled: bool)
signal renderer_ready

const GRID_CELLS := 256
const GRID_VERTS := GRID_CELLS + 1
const CULL_HALF_EXTENT_M := 10000.0

var render_enabled := false
var min_depth_m := 0.015
var edge_feather_m := 0.10
var surface_bias_m := 0.012
var ripple_strength := 0.035
var ripple_scale_m := 3.2

var _batch: MultiMeshInstance3D
var _material: ShaderMaterial
var _ready_renderer := false


func _ready() -> void:
	process_priority = 14
	_build_renderer()
	if not WaterSystem.dynamic_surface_ready.is_connected(_on_dynamic_surface_ready):
		WaterSystem.dynamic_surface_ready.connect(_on_dynamic_surface_ready)
	if not SparseHydroSurfaceCache.cache_ready.is_connected(_on_cache_ready):
		SparseHydroSurfaceCache.cache_ready.connect(_on_cache_ready)
	call_deferred("_try_bind")


func set_render_enabled(enabled: bool) -> void:
	render_enabled = enabled
	if _material != null:
		_material.set_shader_parameter("u_render_enabled", 1.0 if enabled else 0.0)
	_update_visibility()
	render_enabled_changed.emit(enabled)


func is_render_enabled() -> bool:
	return render_enabled


func renderer_available() -> bool:
	return _ready_renderer and _material != null and _batch != null \
		and WaterSystem.dynamic_surface_available()


func material() -> ShaderMaterial:
	return _material


func stats() -> Dictionary:
	return {
		"available": renderer_available(),
		"render_enabled": render_enabled,
		"grid_cells": GRID_CELLS,
		"triangles": GRID_CELLS * GRID_CELLS * 2,
		"min_depth_m": min_depth_m,
		"edge_feather_m": edge_feather_m,
		"surface_bias_m": surface_bias_m,
		"ripple_strength": ripple_strength,
		"ripple_scale_m": ripple_scale_m,
		"cache_available": SparseHydroSurfaceCache.available(),
	}


func _process(_dt: float) -> void:
	if not renderer_available() or not Planet.ready_state or Planet.cfg == null:
		_update_visibility()
		return
	_sync_material()
	_update_visibility()


func _build_renderer() -> void:
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/local_water_surface.gdshader")
	if _material.shader == null:
		push_error("LocalWaterSurface: failed to load local water shader.")
		return

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = _build_grid_mesh()
	mm.instance_count = 1
	mm.visible_instance_count = 1
	mm.set_instance_transform(0, Transform3D.IDENTITY)
	mm.custom_aabb = AABB(
		Vector3(-CULL_HALF_EXTENT_M, -CULL_HALF_EXTENT_M, -CULL_HALF_EXTENT_M),
		Vector3(CULL_HALF_EXTENT_M * 2.0, CULL_HALF_EXTENT_M * 2.0,
			CULL_HALF_EXTENT_M * 2.0))

	_batch = MultiMeshInstance3D.new()
	_batch.name = "LocalWaterSurfacePatch"
	_batch.multimesh = mm
	_batch.material_override = _material
	_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_batch.visible = false
	add_child(_batch)
	_ready_renderer = true
	_try_bind()
	renderer_ready.emit()


func _build_grid_mesh() -> ArrayMesh:
	var vertices := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	uvs.resize(GRID_VERTS * GRID_VERTS)
	indices.resize(GRID_CELLS * GRID_CELLS * 6)

	for y in GRID_VERTS:
		for x in GRID_VERTS:
			var i := y * GRID_VERTS + x
			vertices[i] = Vector3.ZERO
			uvs[i] = Vector2(float(x) / float(GRID_CELLS),
				float(y) / float(GRID_CELLS))

	var cursor := 0
	for y in GRID_CELLS:
		for x in GRID_CELLS:
			var i00 := y * GRID_VERTS + x
			var i10 := i00 + 1
			var i01 := i00 + GRID_VERTS
			var i11 := i01 + 1
			indices[cursor] = i00; cursor += 1
			indices[cursor] = i10; cursor += 1
			indices[cursor] = i11; cursor += 1
			indices[cursor] = i00; cursor += 1
			indices[cursor] = i11; cursor += 1
			indices[cursor] = i01; cursor += 1

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _on_dynamic_surface_ready() -> void:
	_try_bind()


func _on_cache_ready() -> void:
	_try_bind()


func _try_bind() -> void:
	if not _ready_renderer or _material == null or not WaterSystem.dynamic_surface_available():
		return
	var resources := WaterSystem.surface_resources()
	var texture := resources.field_texture()
	if texture == null:
		return
	_material.set_shader_parameter("u_dynamic_surface_field", texture)
	_material.set_shader_parameter("u_dynamic_surface_enabled", 1.0)
	_material.set_shader_parameter("u_dynamic_surface_half_extent_m",
		resources.field_half_extent_m())
	_material.set_shader_parameter("u_render_enabled", 1.0 if render_enabled else 0.0)
	_sync_material()
	_update_visibility()


func _sync_material() -> void:
	if _material == null or not Planet.ready_state or Planet.cfg == null:
		return
	var frame := WaterSystem.dynamic_surface_anchor_frame()
	var dir_value: Variant = frame.get("dir", Vector3.RIGHT)
	var right_value: Variant = frame.get("right", Vector3(0.0, 0.0, -1.0))
	var up_value: Variant = frame.get("up", Vector3.UP)
	var center_value: Variant = frame.get("center_plane", Vector2.ZERO)
	if not (dir_value is Vector3 and right_value is Vector3 \
			and up_value is Vector3 and center_value is Vector2):
		return
	var anchor_dir := (dir_value as Vector3).normalized()
	var anchor_right := (right_value as Vector3).normalized()
	var anchor_up := (up_value as Vector3).normalized()
	var radius := maxf(Planet.cfg.planet_radius, 1.0)
	var anchor_world := Vec3D.new(
		float(anchor_dir.x) * radius,
		float(anchor_dir.y) * radius,
		float(anchor_dir.z) * radius)
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))

	_material.set_shader_parameter("u_origin", origin)
	_material.set_shader_parameter("u_anchor_render", Frames.to_render(anchor_world))
	_material.set_shader_parameter("u_planet_radius", radius)
	_material.set_shader_parameter("u_time_s", float(Time.get_ticks_usec()) / 1000000.0)
	_material.set_shader_parameter("u_dynamic_surface_planet_radius", radius)
	_material.set_shader_parameter("u_dynamic_surface_anchor_dir", anchor_dir)
	_material.set_shader_parameter("u_dynamic_surface_anchor_right", anchor_right)
	_material.set_shader_parameter("u_dynamic_surface_anchor_up", anchor_up)
	_material.set_shader_parameter("u_dynamic_surface_center_plane", center_value)
	_material.set_shader_parameter("u_min_depth_m", maxf(min_depth_m, 1.0e-5))
	_material.set_shader_parameter("u_edge_feather_m", maxf(edge_feather_m, 0.0))
	_material.set_shader_parameter("u_surface_bias_m", surface_bias_m)
	_material.set_shader_parameter("u_ripple_strength", maxf(ripple_strength, 0.0))
	_material.set_shader_parameter("u_ripple_scale_m", maxf(ripple_scale_m, 0.25))


func _update_visibility() -> void:
	if _batch == null:
		return
	_batch.visible = render_enabled and renderer_available() \
		and SparseHydroSurfaceCache.available() and Planet.ready_state


func _exit_tree() -> void:
	if WaterSystem.dynamic_surface_ready.is_connected(_on_dynamic_surface_ready):
		WaterSystem.dynamic_surface_ready.disconnect(_on_dynamic_surface_ready)
	if SparseHydroSurfaceCache.cache_ready.is_connected(_on_cache_ready):
		SparseHydroSurfaceCache.cache_ready.disconnect(_on_cache_ready)
