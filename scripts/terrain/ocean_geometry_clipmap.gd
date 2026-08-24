class_name OceanGeometryClipmap
extends Node3D
## GPU-first local/regional ocean renderer.
##
## A fixed concentric grid follows the observer. The CPU only updates a handful
## of uniforms and sector visibility; sea-level projection, finite-depth Gerstner
## displacement, bathymetry-aware shoreline refraction, normals and foam are all
## evaluated by the GPU in ocean_geometry_clipmap.gdshader.
##
## L0 is a circular disc. L1..L14 are the same annulus split into 12 angular
## sectors, so horizontal views submit only the wedges that can be seen. Short
## waves are filtered by vertex spacing in the shader, which keeps the outer
## rings cheap and prevents aliasing.

const TARGET_FINE_DEPTH: int = 16
const MAX_LEVEL: int = 14
const GRID_CELLS: int = 128
const GRID_VERTS: int = GRID_CELLS + 1
const HALF_CELLS: int = GRID_CELLS >> 1
const RING_INNER_HALF_CELLS: int = 28

const REANCHOR_M: float = 8000.0
const HORIZON_MARGIN_M: float = 12000.0
const ORBIT_HANDOFF_ALTITUDE_M: float = 120000.0
const GLOBAL_BOUNDS_M: float = 4000000.0

const SECTOR_COUNT: int = 12
const SECTOR_HALF_ANGLE: float = 0.2617993877991494
const SECTOR_CULL_MARGIN_RAD: float = 0.3490658503988659
const SECTOR_SHOW_ALL_RADIAL_DOT: float = 0.65

var _material: ShaderMaterial
var _center_batch: MultiMeshInstance3D
var _sector_batches: Array[MultiMeshInstance3D] = []

var _anchor_dir := Vector3(1.0, 0.0, 0.0)
var _anchor_right := Vector3(0.0, 0.0, -1.0)
var _anchor_up := Vector3(0.0, 1.0, 0.0)
var _center_dir := Vector3(1.0, 0.0, 0.0)
var _center_right := Vector3(0.0, 0.0, -1.0)
var _center_up := Vector3(0.0, 1.0, 0.0)
var _center_plane := Vector2.ZERO
var _have_anchor := false
var _base_spacing: float = 0.75

var _active_max_level: int = 0
var _visible_cap_arc_m: float = 0.0
var _visible_sector_count: int = 0
var _ocean_visible := false
var _bound_orbit: Texture2DArray
var _bound_orbit_res: int = -1
var _bound_coast: Texture2DArray
var _bound_coast_version: int = -1
var _physics: OceanGPUPhysics


func _ready() -> void:
	process_priority = 10
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/ocean_geometry_clipmap.gdshader")
	_build_batches()
	_set_visible(false)

	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)

	_physics = OceanGPUPhysics.new()
	_physics.name = "OceanGPUPhysics"
	add_child(_physics)

	if Planet.ready_state and Planet.cfg != null:
		_configure_world()


func _process(_dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_visible(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_visible(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_visible(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var camera_alt: float = planet_pos.length() - radius
	if camera_alt >= ORBIT_HANDOFF_ALTITUDE_M:
		_set_visible(false)
		return

	var observer_dir := planet_pos.normalized()
	if not _have_anchor:
		_reset_anchor(observer_dir)

	var observer_surface := observer_dir * radius
	var anchor_surface := _anchor_dir * radius
	var rel := observer_surface - anchor_surface
	var px := rel.dot(_anchor_right)
	var py := rel.dot(_anchor_up)
	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_anchor(observer_dir)
		px = 0.0
		py = 0.0

	var snapped := Vector2(
		round(px / _base_spacing) * _base_spacing,
		round(py / _base_spacing) * _base_spacing)
	if snapped.distance_squared_to(_center_plane) > 1e-8:
		_center_plane = snapped
		_update_center_basis()

	_update_visible_cap(planet_pos.length(), radius)
	_update_active_levels()
	_bind_textures(false)
	_sync_uniforms(origin)
	_set_visible(_bound_orbit != null)
	if _ocean_visible:
		_update_sector_visibility()


func _configure_world() -> void:
	_base_spacing = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	_have_anchor = false
	_bound_orbit = null
	_bound_orbit_res = -1
	_bound_coast = null
	_bound_coast_version = -1
	_bind_textures(true)


func _on_world_ready(_fields: PlanetFields) -> void:
	_configure_world()


func _on_coast_profile_changed() -> void:
	_bound_coast_version = -1
	_bind_textures(true)


func _reset_anchor(observer_dir: Vector3) -> void:
	_anchor_dir = observer_dir.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_anchor_dir)
	_anchor_right = tangent[0]
	_anchor_up = tangent[1]
	_center_plane = Vector2.ZERO
	_center_dir = _anchor_dir
	_center_right = _anchor_right
	_center_up = _anchor_up
	_have_anchor = true


func _update_center_basis() -> void:
	_center_dir = _direction_for_offset(_anchor_dir, _anchor_right, _anchor_up,
		_center_plane, Planet.cfg.planet_radius)
	var tangent: Array = CubeSphere.tangent_basis(_center_dir)
	_center_right = tangent[0]
	_center_up = tangent[1]


static func _direction_for_offset(center: Vector3, right: Vector3, up: Vector3,
		offset_m: Vector2, radius: float) -> Vector3:
	var arc := offset_m.length()
	if arc <= 1e-6:
		return center.normalized()
	var theta := arc / maxf(radius, 1.0)
	var tangent := (right * offset_m.x + up * offset_m.y).normalized()
	return (center * cos(theta) + tangent * sin(theta)).normalized()


func _update_visible_cap(observer_radius: float, planet_radius: float) -> void:
	var safe_r := maxf(observer_radius, planet_radius + 0.01)
	var horizon_angle := acos(clampf(planet_radius / safe_r, -1.0, 1.0))
	var horizon_arc := horizon_angle * planet_radius
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius,
		horizon_arc + HORIZON_MARGIN_M)


func _update_active_levels() -> void:
	var target_radius := maxf(_visible_cap_arc_m, _base_spacing * float(HALF_CELLS))
	var level := 0
	var outer := _base_spacing * float(HALF_CELLS)
	while level < MAX_LEVEL and outer < target_radius:
		level += 1
		outer *= 2.0
	_active_max_level = level
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = _active_max_level


func _bind_textures(force: bool) -> void:
	if _material == null or not Planet.ready_state:
		return

	var orbit := Planet.orbit_elevation_texture
	var orbit_res := Planet.orbit_texture_face_res
	if force or orbit != _bound_orbit:
		_bound_orbit = orbit
		_material.set_shader_parameter("u_orbit_elevation", orbit)
	if force or orbit_res != _bound_orbit_res:
		_bound_orbit_res = orbit_res
		_material.set_shader_parameter("u_orbit_face_res", float(orbit_res))

	var coast: Node = get_node_or_null("/root/CoastlineClipmap")
	if coast == null:
		_material.set_shader_parameter("u_coast_clipmap_ready", 0.0)
		return
	var version := int(coast.get("_published_version"))
	var value: Variant = coast.get("_texture")
	var texture: Texture2DArray = value if value is Texture2DArray else null
	if not force and version == _bound_coast_version and texture == _bound_coast:
		return
	_bound_coast_version = version
	_bound_coast = texture
	_material.set_shader_parameter("u_coast_clipmap_ready", 1.0 if texture != null else 0.0)
	_material.set_shader_parameter("u_coast_center", coast.get("_published_center"))
	_material.set_shader_parameter("u_coast_right", coast.get("_published_right"))
	_material.set_shader_parameter("u_coast_up", coast.get("_published_up"))
	_material.set_shader_parameter("u_coast_res", float(coast.get("CLIPMAP_RES")) if false else 512.0)
	_material.set_shader_parameter("u_coast_texel_m", Vector3(35.0, 120.0, 480.0))
	if texture != null:
		_material.set_shader_parameter("u_coast_clipmap", texture)


func _sync_uniforms(origin: Vector3) -> void:
	_material.set_shader_parameter("u_origin", origin)
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height)
	_material.set_shader_parameter("u_center_dir", _center_dir)
	_material.set_shader_parameter("u_center_right", _center_right)
	_material.set_shader_parameter("u_center_up", _center_up)
	_material.set_shader_parameter("u_base_spacing", _base_spacing)
	_material.set_shader_parameter("u_grid_cells", float(GRID_CELLS))
	_material.set_shader_parameter("u_visible_cap_angle",
		minf(_visible_cap_arc_m / Planet.cfg.planet_radius * 1.03, PI * 0.5))
	_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_material.set_shader_parameter("u_sun_intensity", GraphicsQuality.solar_irradiance())
	_material.set_shader_parameter("u_orbit_handoff_altitude", ORBIT_HANDOFF_ALTITUDE_M)
	_material.set_shader_parameter("u_wave_scale", 1.0)


func _build_batches() -> void:
	_sector_batches.clear()
	var center_mesh := _build_center_mesh()
	var bounds := AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))
	_center_batch = _make_batch("OceanClipmapL0", center_mesh, 1, bounds)
	_center_batch.multimesh.set_instance_custom_data(0, Color(0.0, 0.0, 0.0, 0.0))
	add_child(_center_batch)

	for sector in SECTOR_COUNT:
		var mesh := _build_sector_mesh(sector)
		var batch := _make_batch("OceanClipmapSector%02d" % sector, mesh, MAX_LEVEL, bounds)
		for instance_index in MAX_LEVEL:
			batch.multimesh.set_instance_custom_data(instance_index,
				Color(float(instance_index + 1), float(sector), 0.0, 0.0))
		batch.multimesh.visible_instance_count = 0
		batch.visible = false
		_sector_batches.append(batch)
		add_child(batch)


func _make_batch(node_name: String, mesh: ArrayMesh, count: int, bounds: AABB) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.custom_aabb = bounds
	mm.instance_count = count
	mm.visible_instance_count = count
	for instance_index in count:
		mm.set_instance_transform(instance_index, Transform3D.IDENTITY)
	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.material_override = _material
	batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return batch


static func _build_center_mesh() -> ArrayMesh:
	var vertices := PackedVector3Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	var indices := PackedInt32Array()
	var outer_sq := float(HALF_CELLS * HALF_CELLS)
	for y in GRID_CELLS:
		var cy := float(y) + 0.5 - float(HALF_CELLS)
		for x in GRID_CELLS:
			var cx := float(x) + 0.5 - float(HALF_CELLS)
			if cx * cx + cy * cy <= outer_sq:
				_append_cell(indices, x, y)
	return _mesh_from_indices(vertices, indices)


static func _build_sector_mesh(sector_index: int) -> ArrayMesh:
	var vertices := PackedVector3Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	var indices := PackedInt32Array()
	var outer_sq := float(HALF_CELLS * HALF_CELLS)
	var inner_sq := float(RING_INNER_HALF_CELLS * RING_INNER_HALF_CELLS)
	for y in GRID_CELLS:
		var cy := float(y) + 0.5 - float(HALF_CELLS)
		for x in GRID_CELLS:
			var cx := float(x) + 0.5 - float(HALF_CELLS)
			var r_sq := cx * cx + cy * cy
			if r_sq > outer_sq or r_sq < inner_sq:
				continue
			var angle := atan2(cy, cx)
			if angle < 0.0:
				angle += TAU
			var owner := clampi(int(floor(angle / TAU * float(SECTOR_COUNT))), 0, SECTOR_COUNT - 1)
			if owner == sector_index:
				_append_cell(indices, x, y)
	return _mesh_from_indices(vertices, indices)


static func _append_cell(indices: PackedInt32Array, x: int, y: int) -> void:
	var i00 := y * GRID_VERTS + x
	var i10 := i00 + 1
	var i01 := (y + 1) * GRID_VERTS + x
	var i11 := i01 + 1
	indices.append_array([i00, i10, i11, i00, i11, i01])


static func _mesh_from_indices(vertices: PackedVector3Array, indices: PackedInt32Array) -> ArrayMesh:
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _set_visible(value: bool) -> void:
	_ocean_visible = value
	if _center_batch != null:
		_center_batch.visible = value
	if not value:
		_visible_sector_count = 0
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return
	_update_sector_visibility()


func _update_sector_visibility() -> void:
	if not _ocean_visible or _active_max_level <= 0:
		_visible_sector_count = 0
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return
	var forward := -camera.global_transform.basis.z.normalized()
	var radial_dot := forward.dot(_center_dir)
	var forward_plane := Vector2(forward.dot(_center_right), forward.dot(_center_up))
	var show_all := absf(radial_dot) >= SECTOR_SHOW_ALL_RADIAL_DOT \
		or forward_plane.length_squared() < 1e-5
	var cos_limit := -1.0
	var forward_2d := Vector2.RIGHT
	if not show_all:
		forward_2d = forward_plane.normalized()
		var viewport_size := get_viewport().get_visible_rect().size
		var aspect := viewport_size.x / maxf(viewport_size.y, 1.0)
		var vertical_half := deg_to_rad(camera.fov) * 0.5
		var horizontal_half := atan(tan(vertical_half) * aspect)
		cos_limit = cos(minf(horizontal_half + SECTOR_HALF_ANGLE + SECTOR_CULL_MARGIN_RAD, PI))
	_visible_sector_count = 0
	for sector in _sector_batches.size():
		var visible := show_all
		if not show_all:
			var angle := (float(sector) + 0.5) * TAU / float(SECTOR_COUNT)
			visible = Vector2(cos(angle), sin(angle)).dot(forward_2d) >= cos_limit
		_sector_batches[sector].visible = visible
		if visible:
			_visible_sector_count += 1


func material() -> ShaderMaterial:
	return _material


func gpu_stats() -> Dictionary:
	return {
		"active_levels": _active_max_level + 1,
		"visible_sectors": _visible_sector_count,
		"grid_cells": GRID_CELLS,
		"gpu_waves": true,
		"gpu_buoyancy_queries": _physics != null,
		"orbit_handoff_m": ORBIT_HANDOFF_ALTITUDE_M,
	}
