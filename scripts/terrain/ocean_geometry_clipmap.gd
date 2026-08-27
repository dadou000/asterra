class_name OceanGeometryClipmap
extends Node3D
## GPU-first local/regional ocean renderer.
##
## A fixed nested square grid follows the observer. The CPU only updates uniforms,
## bounded visual interaction events and sector visibility. Authoritative
## buoyancy remains in OceanGPUPhysics and is deliberately independent of the
## selected graphics preset.

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
const PROCEDURAL_DETAIL_STRENGTH: float = 1.0

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
var _terrain_base_spacing: float = 0.75

var _active_max_level: int = 0
var _visible_cap_arc_m: float = 0.0
var _visible_sector_count: int = 0
var _ocean_visible := false
var _debug_waves_disabled := false
var _debug_stable_displacement := true
var _stable_anchor_world: Vec3D = Vec3D.new()
var _bound_macro: Texture2DArray
var _bound_macro_res: int = -1
var _physics: OceanGPUPhysics
var _interactions := OceanVisualInteractions.new()
var _water_profile: Dictionary = GraphicsQuality.water_profile(GraphicsQuality.DEFAULT_PRESET)
var _quality_preset: int = GraphicsQuality.DEFAULT_PRESET


func _ready() -> void:
	process_priority = 10
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/ocean_geometry_clipmap.gdshader")
	_build_batches()
	_set_visible(false)
	_apply_quality(GraphicsQuality.DEFAULT_PRESET)

	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)

	_physics = OceanGPUPhysics.new()
	_physics.name = "OceanGPUPhysics"
	add_child(_physics)

	# OceanSystem is listed before AppSettings in the autoload table. Deferring the
	# initial read guarantees AppSettings has loaded user://settings.cfg first.
	call_deferred("_initialize_quality")

	if Planet.ready_state and Planet.cfg != null:
		_configure_world()


func _initialize_quality() -> void:
	if not AppSettings.graphics_quality_changed.is_connected(_on_graphics_quality_changed):
		AppSettings.graphics_quality_changed.connect(_on_graphics_quality_changed)
	_apply_quality(AppSettings.graphics_quality)


func _on_graphics_quality_changed(preset: int) -> void:
	_apply_quality(preset)


func _apply_quality(preset: int) -> void:
	_quality_preset = GraphicsQuality.sanitize(preset)
	_water_profile = GraphicsQuality.water_profile(_quality_preset)
	_refresh_base_spacing_for_quality()
	if _material == null:
		return
	_material.set_shader_parameter("u_quality_band_count", int(_water_profile["displacement_band_count"]))
	_material.set_shader_parameter("u_micro_band_count", int(_water_profile["micro_normal_band_count"]))
	_material.set_shader_parameter("u_displacement_spacing_scale", float(_water_profile["displacement_spacing_scale"]))
	_material.set_shader_parameter("u_bathymetry_level_limit", int(_water_profile["bathymetry_level_limit"]))
	_material.set_shader_parameter("u_foam_quality", float(_water_profile["foam_quality"]))
	_material.set_shader_parameter("u_crest_scatter_strength", float(_water_profile["crest_scatter_strength"]))
	_material.set_shader_parameter("u_interaction_range_m", float(_water_profile["interaction_range_m"]))
	_material.set_shader_parameter("u_interaction_vertex_level", int(_water_profile["interaction_vertex_level"]))
	_sync_interactions()


func _refresh_base_spacing_for_quality() -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return
	var nominal_spacing := PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	_terrain_base_spacing = nominal_spacing
	var scale := float(_water_profile.get("geometry_spacing_scale", 1.0))
	var new_spacing := nominal_spacing * clampf(scale, 0.35, 2.0)
	if not is_equal_approx(new_spacing, _base_spacing):
		_base_spacing = new_spacing
		# Every ring snap is expressed in multiples of ocean base spacing. Force a
		# clean re-anchor when the graphics preset changes instead of letting old and
		# new lattice coordinates coexist for one frame. Terrain sampling keeps the
		# unscaled nominal spacing and therefore does not change with water quality.
		_have_anchor = false


func _process(_dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_visible(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_visible(false)
		return

	var observer_world: Vec3D = Frames.to_world(camera.global_position)
	var observer_radius: float = observer_world.length()
	if observer_radius <= 1.0:
		_set_visible(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var camera_alt: float = observer_radius - radius
	if camera_alt >= ORBIT_HANDOFF_ALTITUDE_M:
		_set_visible(false)
		return

	var observer_unit_world: Vec3D = observer_world.normalized()
	var observer_dir: Vector3 = observer_unit_world.to_v3()
	if not _have_anchor:
		_reset_anchor(observer_dir)
		_capture_stable_anchor(observer_unit_world.mul(radius))

	var observer_surface_world: Vec3D = observer_unit_world.mul(radius)
	var rel: Vec3D = observer_surface_world.sub(_stable_anchor_world)
	var px: float = rel.x * _anchor_right.x + rel.y * _anchor_right.y + rel.z * _anchor_right.z
	var py: float = rel.x * _anchor_up.x + rel.y * _anchor_up.y + rel.z * _anchor_up.z
	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_anchor(observer_dir)
		_capture_stable_anchor(observer_surface_world)
		px = 0.0
		py = 0.0

	var snapped := Vector2(round(px / _base_spacing) * _base_spacing, round(py / _base_spacing) * _base_spacing)
	if snapped.distance_squared_to(_center_plane) > 1e-8:
		_center_plane = snapped
		_update_center_basis()

	_update_visible_cap(observer_radius, radius)
	_update_active_levels()
	_bind_gpu_terrain(false)
	_sync_interactions()
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_sync_uniforms(origin)
	_set_visible(_bound_macro != null)
	if _ocean_visible:
		_update_sector_visibility()


func _sync_interactions() -> void:
	if _material == null:
		return
	var now_s := float(Time.get_ticks_usec()) / 1000000.0
	_interactions.prune(now_s, float(_water_profile["interaction_lifetime_s"]), int(_water_profile["interaction_budget"]))
	_material.set_shader_parameter("u_interaction_tex", _interactions.sync_texture())
	_material.set_shader_parameter("u_interaction_count", _interactions.event_count())


## Visual surface impulse. Gameplay code can call this when an object crosses the
## water surface. The result is intentionally non-authoritative.
func add_impact(world_position: Vec3D, amplitude_m: float, radius_m: float,
		wavelength_m := 5.0, propagation_speed_mps := 8.0, foam := 0.35) -> void:
	_interactions.add_impact(world_position, amplitude_m, radius_m, wavelength_m, propagation_speed_mps, foam)


## Visual wake sample. A moving hull should submit these periodically along its
## waterline/centreline. The shader turns the history into a Kelvin-like V wake.
func add_wake(world_position: Vec3D, travel_direction: Vector3, amplitude_m: float,
		beam_m: float, wavelength_m := 12.0, propagation_speed_mps := 10.0,
		foam := 0.7) -> void:
	_interactions.add_wake(world_position, travel_direction, amplitude_m, beam_m,
		wavelength_m, propagation_speed_mps, foam)


func clear_visual_interactions() -> void:
	_interactions.clear()
	_sync_interactions()


func _configure_world() -> void:
	_refresh_base_spacing_for_quality()
	_have_anchor = false
	_bound_macro = null
	_bound_macro_res = -1
	_bind_gpu_terrain(true)


func _on_world_ready(_fields: PlanetFields) -> void:
	_configure_world()


func _on_coast_profile_changed() -> void:
	_bind_gpu_terrain(true)


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
	_center_dir = _direction_for_offset(_anchor_dir, _anchor_right, _anchor_up, _center_plane, Planet.cfg.planet_radius)
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
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius, horizon_arc + HORIZON_MARGIN_M)


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


func _bind_gpu_terrain(force: bool) -> void:
	if _material == null or not Planet.ready_state:
		return
	var macro: Texture2DArray = Planet.orbit_elevation_texture
	var macro_res: int = Planet.orbit_texture_face_res
	if force or macro != _bound_macro:
		_bound_macro = macro
		_material.set_shader_parameter("u_macro_elevation", macro)
	if force or macro_res != _bound_macro_res:
		_bound_macro_res = macro_res
		_material.set_shader_parameter("u_macro_face_res", float(macro_res))
	_material.set_shader_parameter("u_macro_ready", 1.0 if macro != null else 0.0)


func _sync_uniforms(origin: Vector3) -> void:
	_material.set_shader_parameter("u_origin", origin)
	_material.set_shader_parameter("u_anchor_render", Frames.to_render(_stable_anchor_world))
	_material.set_shader_parameter("u_anchor_dir", _anchor_dir)
	_material.set_shader_parameter("u_anchor_right", _anchor_right)
	_material.set_shader_parameter("u_anchor_up", _anchor_up)
	_material.set_shader_parameter("u_lattice_center_plane", _center_plane)
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height)
	_material.set_shader_parameter("u_center_dir", _center_dir)
	_material.set_shader_parameter("u_center_right", _center_right)
	_material.set_shader_parameter("u_center_up", _center_up)
	# Terrain and ocean topology deliberately have separate metric spacings. The
	# terrain-height include must stay on the renderer's nominal LOD scale even when
	# Ultra water uses a denser nested lattice.
	_material.set_shader_parameter("u_base_spacing", _terrain_base_spacing)
	_material.set_shader_parameter("u_ocean_base_spacing", _base_spacing)
	_material.set_shader_parameter("u_grid_cells", float(GRID_CELLS))
	_material.set_shader_parameter("u_visible_cap_angle", minf(_visible_cap_arc_m / Planet.cfg.planet_radius * 1.03, PI * 0.5))
	_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_material.set_shader_parameter("u_sun_intensity", GraphicsQuality.solar_irradiance())
	_material.set_shader_parameter("u_orbit_handoff_altitude", ORBIT_HANDOFF_ALTITUDE_M)
	_material.set_shader_parameter("u_wave_scale", debug_wave_scale())
	_material.set_shader_parameter("u_stable_displacement", 1.0 if _debug_stable_displacement else 0.0)
	_material.set_shader_parameter("u_time_s", float(Time.get_ticks_usec()) / 1000000.0)

	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	_material.set_shader_parameter("u_detail_seed", maxi(detail_seed, 1))
	_material.set_shader_parameter("u_detail_strength", PROCEDURAL_DETAIL_STRENGTH * maxf(0.05, Planet.cfg.detail_amplitude / 260.0))


func _build_batches() -> void:
	_sector_batches.clear()
	var center_mesh := _build_center_mesh()
	var bounds := AABB(Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M), Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))
	_center_batch = _make_batch("OceanClipmapL0", center_mesh, 1, bounds)
	_center_batch.multimesh.set_instance_custom_data(0, Color(0.0, 0.0, 0.0, 0.0))
	add_child(_center_batch)

	for sector in SECTOR_COUNT:
		var mesh := _build_sector_mesh(sector)
		var batch := _make_batch("OceanClipmapSector%02d" % sector, mesh, MAX_LEVEL, bounds)
		for instance_index in MAX_LEVEL:
			batch.multimesh.set_instance_custom_data(instance_index, Color(float(instance_index + 1), float(sector), 0.0, 0.0))
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


func set_debug_waves_disabled(disabled: bool) -> void:
	_debug_waves_disabled = disabled
	if _material != null:
		_material.set_shader_parameter("u_wave_scale", debug_wave_scale())


func debug_waves_disabled() -> bool:
	return _debug_waves_disabled


func debug_wave_scale() -> float:
	return 0.0 if _debug_waves_disabled else 1.0


func set_debug_stable_displacement(value: bool) -> void:
	_debug_stable_displacement = value
	if _material != null:
		_material.set_shader_parameter("u_stable_displacement", 1.0 if value else 0.0)


func debug_stable_displacement_enabled() -> bool:
	return _debug_stable_displacement


func _capture_stable_anchor(surface_world: Vec3D) -> void:
	_stable_anchor_world = surface_world.dup()


## Every logical ocean-grid coordinate is stored explicitly in UV. The mesh is a
## nested 2:1 square clipmap: a Cartesian lattice should have Cartesian boundaries.
## Circularly cutting that lattice created unrelated staircase edges at every LOD.
static func _build_center_mesh() -> ArrayMesh:
	var vertices: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var indices: Array[int] = []
	var remap: Dictionary = {}
	for y in GRID_CELLS:
		for x in GRID_CELLS:
			_append_compact_cell(remap, vertices, uvs, indices, x, y)
	return _mesh_from_compact(vertices, uvs, indices)


static func _build_sector_mesh(sector_index: int) -> ArrayMesh:
	var vertices: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var indices: Array[int] = []
	var remap: Dictionary = {}
	var inner := float(RING_INNER_HALF_CELLS)
	for y in GRID_CELLS:
		var cy := float(y) + 0.5 - float(HALF_CELLS)
		for x in GRID_CELLS:
			var cx := float(x) + 0.5 - float(HALF_CELLS)
			# Chebyshev distance produces a square annulus whose boundaries are exact
			# members of the 2:1 nested lattice. The loop bounds are already the outer
			# square, so only the inner square needs to be removed.
			if maxf(absf(cx), absf(cy)) < inner:
				continue
			# Sectors remain angular only for view culling; they no longer define the
			# LOD boundary itself.
			var angle := atan2(cy, cx)
			if angle < 0.0:
				angle += TAU
			var owner := clampi(int(floor(angle / TAU * float(SECTOR_COUNT))), 0, SECTOR_COUNT - 1)
			if owner != sector_index:
				continue
			_append_compact_cell(remap, vertices, uvs, indices, x, y)
	return _mesh_from_compact(vertices, uvs, indices)


static func _append_compact_cell(remap: Dictionary, vertices: Array[Vector3],
		uvs: Array[Vector2], indices: Array[int], x: int, y: int) -> void:
	var i00 := _compact_vertex(remap, vertices, uvs, x, y)
	var i10 := _compact_vertex(remap, vertices, uvs, x + 1, y)
	var i01 := _compact_vertex(remap, vertices, uvs, x, y + 1)
	var i11 := _compact_vertex(remap, vertices, uvs, x + 1, y + 1)
	indices.append(i00)
	indices.append(i10)
	indices.append(i11)
	indices.append(i00)
	indices.append(i11)
	indices.append(i01)


static func _compact_vertex(remap: Dictionary, vertices: Array[Vector3],
		uvs: Array[Vector2], gx: int, gy: int) -> int:
	var logical_index := gy * GRID_VERTS + gx
	var existing: Variant = remap.get(logical_index, null)
	if existing != null:
		return int(existing)
	var local_index := vertices.size()
	remap[logical_index] = local_index
	vertices.append(Vector3.ZERO)
	uvs.append(Vector2(float(gx), float(gy)))
	return local_index


static func _mesh_from_compact(vertices: Array[Vector3], uvs: Array[Vector2],
		indices: Array[int]) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	if indices.is_empty():
		return mesh
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array(vertices)
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array(uvs)
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array(indices)
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
	var show_all := absf(radial_dot) >= SECTOR_SHOW_ALL_RADIAL_DOT or forward_plane.length_squared() < 1e-5
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
		"base_spacing_m": _base_spacing,
		"terrain_base_spacing_m": _terrain_base_spacing,
		"geometry_spacing_scale": float(_water_profile.get("geometry_spacing_scale", 1.0)),
		"gpu_waves": not _debug_waves_disabled,
		"stable_displacement": _debug_stable_displacement,
		"gpu_coast_height": true,
		"gpu_buoyancy_queries": _physics != null,
		"quality": GraphicsQuality.preset_name(_quality_preset),
		"visual_interactions": _interactions.event_count(),
		"interaction_budget": int(_water_profile["interaction_budget"]),
		"orbit_handoff_m": ORBIT_HANDOFF_ALTITUDE_M,
	}
