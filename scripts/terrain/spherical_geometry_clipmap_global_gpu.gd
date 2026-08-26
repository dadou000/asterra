extends "res://scripts/terrain/spherical_geometry_clipmap_global.gd"
## GPU synthesis extension for the current 0.0.5 resident global clipmap.
##
## The latest global clipmap remains authoritative for compact sector topology,
## promoted-centre screen-space LOD, stationary per-level lattices, stable
## double-precision anchoring and resident whole-planet height/material maps.
## This layer adds horizon-exact ring buffers plus immutable GPU context/PBR
## resources without performing any runtime CPU terrain synthesis.

const GPU_SURFACE_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_global_surface.gdshader"
const HORIZON_RING_SAFETY: float = 1.015
const DETAIL_ORIGIN_WRAP_M: float = 4096.0
const DEFAULT_AERIAL_STRENGTH: float = 0.78

# Keep imported textures out of script parse-time dependency resolution. They are
# loaded once after Godot has imported the project resources.
const PBR_GROUND_ALBEDO_PATH := "res://assets/textures/terrain/ground003_color_2k.jpg"
const PBR_GROUND_NORMAL_PATH := "res://assets/textures/terrain/ground003_normal_gl_2k.jpg"
const PBR_GROUND_ROUGHNESS_PATH := "res://assets/textures/terrain/ground003_roughness_2k.jpg"
const PBR_GRASS_ALBEDO_PATH := "res://assets/textures/terrain/leafy_grass_diff_2k.jpg"
const PBR_GRASS_NORMAL_PATH := "res://assets/textures/terrain/leafy_grass_nor_gl_2k.jpg"
const PBR_GRASS_ROUGHNESS_PATH := "res://assets/textures/terrain/leafy_grass_rough_2k.jpg"
const PBR_MUD_ALBEDO_PATH := "res://assets/textures/terrain/brown_mud_diff_2k.jpg"
const PBR_MUD_NORMAL_PATH := "res://assets/textures/terrain/brown_mud_nor_gl_2k.jpg"
const PBR_MUD_ROUGHNESS_PATH := "res://assets/textures/terrain/brown_mud_rough_2k.jpg"
const PBR_FOREST_ALBEDO_PATH := "res://assets/textures/terrain/forrest_ground_01_diff_2k.jpg"
const PBR_FOREST_NORMAL_PATH := "res://assets/textures/terrain/forrest_ground_01_nor_gl_2k.jpg"
const PBR_FOREST_ROUGHNESS_PATH := "res://assets/textures/terrain/forrest_ground_01_rough_2k.jpg"

var _gpu_ctx_generation: int = -1
var _physical_ring_count: int = 0
var _horizon_max_level: int = 0
var _pbr_bound := false
var _pbr_attempted := false
var _debug_pbr_enabled := true
var _debug_geomorph_mode: int = 0
var _aerial_strength: float = DEFAULT_AERIAL_STRENGTH


func _ready() -> void:
	# Complete the latest 0.0.5 clipmap initialization first, then replace only its
	# material shader. Geometry/topology/LOD objects are not rebuilt here.
	super._ready()
	_material.shader = load(GPU_SURFACE_SHADER_PATH)
	# Shader replacement clears parameter bindings.
	super._bind_gpu_resources(true)
	super._sync_material_control()
	_bind_gpu_context(true)
	_bind_surface_pbr(true)
	_sync_detail_seed()
	_sync_debug_uniforms()


func _process(dt: float) -> void:
	super._process(dt)
	_bind_gpu_context(false)


func _update_active_levels() -> void:
	_update_screen_space_min_level()

	# A ring whose inner edge begins beyond the visible horizon-safe cap cannot
	# contribute a single triangle, so it is omitted from the physical draw buffer.
	var max_level: int = _active_min_level
	var visible_radius_m: float = maxf(
		_visible_cap_arc_m,
		_base_spacing * pow(2.0, float(_active_min_level)) * float(HALF_CELLS))
	while max_level < MAX_LEVEL:
		var candidate: int = max_level + 1
		var candidate_spacing: float = _base_spacing * pow(2.0, float(candidate))
		var candidate_inner_m: float = candidate_spacing * float(RING_INNER_HALF_CELLS)
		if candidate_inner_m > visible_radius_m * HORIZON_RING_SAFETY:
			break
		max_level = candidate

	_active_max_level = maxi(max_level, _active_min_level)
	_horizon_max_level = _active_max_level
	_apply_active_level_window()


func _apply_active_level_window() -> void:
	if _active_min_level == _last_applied_min_level \
			and _active_max_level == _last_applied_max_level:
		return
	_last_applied_min_level = _active_min_level
	_last_applied_max_level = _active_max_level
	var ring_count: int = maxi(_active_max_level - _active_min_level, 0)
	_physical_ring_count = ring_count

	for sector: int in SECTOR_COUNT:
		var center: MultiMeshInstance3D = _center_sector_batches[sector]
		if center.multimesh != null:
			center.multimesh.set_instance_custom_data(0,
				Color(float(_active_min_level), float(sector), 0.0, 0.0))

		var rings: MultiMeshInstance3D = _sector_batches[sector]
		if rings.multimesh == null:
			continue
		var mm: MultiMesh = rings.multimesh
		# Resize only when the LOD window changes. This makes the physical instance
		# buffer exactly match the logical visible rings instead of keeping L1..L14
		# permanently allocated behind visible_instance_count.
		if mm.instance_count != ring_count:
			mm.visible_instance_count = 0
			mm.instance_count = ring_count
		for instance_index: int in ring_count:
			var logical_level: int = _active_min_level + instance_index + 1
			mm.set_instance_transform(instance_index, Transform3D.IDENTITY)
			mm.set_instance_custom_data(instance_index,
				Color(float(logical_level), float(sector), 1.0, 0.0))
		mm.visible_instance_count = ring_count


func _restore_dynamic_ring_window() -> void:
	# Visibility/debug operations must never resurrect horizon-culled levels.
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = mini(
				_physical_ring_count, batch.multimesh.instance_count)


func _sync_uniforms(origin: Vector3) -> void:
	# Preserve all stable-anchor/latest-clipmap uniforms from the parent.
	super._sync_uniforms(origin)
	if _material == null:
		return
	_material.set_shader_parameter("u_detail_origin", _wrapped_detail_origin())
	_material.set_shader_parameter("u_terrain_atmosphere_height", Planet.cfg.atmosphere_height)
	_material.set_shader_parameter("u_terrain_aerial_strength", _aerial_strength)
	_material.set_shader_parameter("u_debug_geomorph_mode", _debug_geomorph_mode)
	_material.set_shader_parameter("u_pbr_enabled", 1.0 if _debug_pbr_enabled and _pbr_bound else 0.0)


func _wrapped_detail_origin() -> Vector3:
	# Modulo before converting the double-precision floating origin to Vector3.
	# All scan periods divide 4096 m, so wrap crossings do not change texture phase.
	return Vector3(
		fposmod(float(Frames.origin.x), DETAIL_ORIGIN_WRAP_M),
		fposmod(float(Frames.origin.y), DETAIL_ORIGIN_WRAP_M),
		fposmod(float(Frames.origin.z), DETAIL_ORIGIN_WRAP_M))


func _bind_gpu_context(force: bool) -> void:
	if _material == null:
		return
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		_material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _gpu_ctx_generation:
		_gpu_ctx_generation = generation
		_material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
		_material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
		_material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
		_material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
		_material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
		_material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
		_material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
		_material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
		_material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
	_material.set_shader_parameter("u_ctx_ready", 1.0)


func _load_pbr_texture(path: String) -> Texture2D:
	if not ResourceLoader.exists(path, "Texture2D"):
		push_warning("Terrain PBR texture unavailable: %s" % path)
		return null
	var resource: Resource = load(path)
	return resource as Texture2D if resource is Texture2D else null


func _bind_surface_pbr(force: bool) -> void:
	if _material == null or (_pbr_attempted and not force):
		return
	_pbr_attempted = true
	var ground_albedo := _load_pbr_texture(PBR_GROUND_ALBEDO_PATH)
	var ground_normal := _load_pbr_texture(PBR_GROUND_NORMAL_PATH)
	var ground_roughness := _load_pbr_texture(PBR_GROUND_ROUGHNESS_PATH)
	var grass_albedo := _load_pbr_texture(PBR_GRASS_ALBEDO_PATH)
	var grass_normal := _load_pbr_texture(PBR_GRASS_NORMAL_PATH)
	var grass_roughness := _load_pbr_texture(PBR_GRASS_ROUGHNESS_PATH)
	var mud_albedo := _load_pbr_texture(PBR_MUD_ALBEDO_PATH)
	var mud_normal := _load_pbr_texture(PBR_MUD_NORMAL_PATH)
	var mud_roughness := _load_pbr_texture(PBR_MUD_ROUGHNESS_PATH)
	var forest_albedo := _load_pbr_texture(PBR_FOREST_ALBEDO_PATH)
	var forest_normal := _load_pbr_texture(PBR_FOREST_NORMAL_PATH)
	var forest_roughness := _load_pbr_texture(PBR_FOREST_ROUGHNESS_PATH)

	_material.set_shader_parameter("u_pbr_ground_albedo", ground_albedo)
	_material.set_shader_parameter("u_pbr_ground_normal", ground_normal)
	_material.set_shader_parameter("u_pbr_ground_roughness", ground_roughness)
	_material.set_shader_parameter("u_pbr_grass_albedo", grass_albedo)
	_material.set_shader_parameter("u_pbr_grass_normal", grass_normal)
	_material.set_shader_parameter("u_pbr_grass_roughness", grass_roughness)
	_material.set_shader_parameter("u_pbr_mud_albedo", mud_albedo)
	_material.set_shader_parameter("u_pbr_mud_normal", mud_normal)
	_material.set_shader_parameter("u_pbr_mud_roughness", mud_roughness)
	_material.set_shader_parameter("u_pbr_forest_albedo", forest_albedo)
	_material.set_shader_parameter("u_pbr_forest_normal", forest_normal)
	_material.set_shader_parameter("u_pbr_forest_roughness", forest_roughness)

	_pbr_bound = ground_albedo != null and ground_normal != null and ground_roughness != null \
		and grass_albedo != null and grass_normal != null and grass_roughness != null \
		and mud_albedo != null and mud_normal != null and mud_roughness != null \
		and forest_albedo != null and forest_normal != null and forest_roughness != null
	_material.set_shader_parameter("u_pbr_enabled", 1.0 if _debug_pbr_enabled and _pbr_bound else 0.0)


func set_debug_geomorph_mode(mode: int) -> void:
	_debug_geomorph_mode = clampi(mode, 0, 6)
	if _material != null:
		_material.set_shader_parameter("u_debug_geomorph_mode", _debug_geomorph_mode)


func set_debug_pbr_enabled(value: bool) -> void:
	_debug_pbr_enabled = value
	if _material != null:
		_material.set_shader_parameter("u_pbr_enabled", 1.0 if value and _pbr_bound else 0.0)


func set_aerial_strength(value: float) -> void:
	_aerial_strength = clampf(value, 0.0, 2.0)
	if _material != null:
		_material.set_shader_parameter("u_terrain_aerial_strength", _aerial_strength)


func debug_geomorph_mode() -> int:
	return _debug_geomorph_mode


func debug_pbr_enabled() -> bool:
	return _debug_pbr_enabled


func aerial_strength() -> float:
	return _aerial_strength


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	var context: Node = get_node_or_null("/root/PlanetContext")
	out["gpu_geomorph_latest_clipmap_base"] = true
	out["gpu_context_ready"] = context != null and bool(context.get("ready_state"))
	out["gpu_context_generation"] = _gpu_ctx_generation
	out["physical_ring_instances"] = _physical_ring_count
	out["horizon_max_level"] = _horizon_max_level
	out["horizon_exact_ring_buffers"] = true
	out["physical_surface_classifier"] = true
	out["scanned_pbr"] = _pbr_bound and _debug_pbr_enabled
	out["terrain_aerial_perspective"] = true
	out["terrain_aerial_strength"] = _aerial_strength
	return out
