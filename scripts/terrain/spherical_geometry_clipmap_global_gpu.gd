extends "res://scripts/terrain/spherical_geometry_clipmap_global.gd"
## GPU synthesis extension for the current 0.0.5 resident global clipmap.
##
## The latest global clipmap remains authoritative for compact sector topology,
## promoted-centre screen-space LOD, stationary per-level lattices, stable
## double-precision anchoring and resident whole-planet height/material maps.
## This layer adds horizon-exact visible ring windows plus immutable GPU context/PBR
## resources without performing any runtime CPU terrain synthesis.
##
## IMPORTANT: MultiMesh storage is fixed after startup. Earlier revisions resized
## all 12 ring buffers whenever the horizon LOD changed; travelling over relief can
## make L7/L8 (etc.) alternate and repeatedly reallocate renderer storage. We now
## keep MAX_LEVEL slots and change only custom data + visible_instance_count.

const GPU_SURFACE_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_global_surface.gdshader"
const HORIZON_RING_SAFETY: float = 1.015
const DETAIL_ORIGIN_WRAP_M: float = 4096.0
const DEFAULT_AERIAL_STRENGTH: float = 0.78

# View-dependent *submission* culling. This deliberately does not alter the
# logical LOD window. Normal/horizon views always retain the complete current
# clipmap stack. Only views whose entire frustum is safely above/below the local
# tangent plane get special treatment.
const SKY_CULL_MIN_ELEVATION_RAD: float = 0.3490658504 # +20 degrees
const NADIR_RING_LIMIT_MAX_ELEVATION_RAD: float = -0.2617993878 # -15 degrees
const NADIR_RING_GUARD_M: float = 750.0
const NADIR_RING_GUARD_SCALE: float = 0.25
const UNDERGROUND_CULL_DEPTH_M: float = 1.5

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
var _automatic_ring_count: int = 0
var _debug_manual_ring_count_enabled := false
var _debug_manual_ring_count: int = 0
var _pbr_bound := false
var _pbr_attempted := false
var _debug_pbr_enabled := true
var _debug_geomorph_mode: int = 0
var _aerial_strength: float = DEFAULT_AERIAL_STRENGTH

var _view_surface_culled := false
var _view_cull_reason := "none"
var _view_ring_instances: int = 0


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

	# Automatic production mode: include every annulus whose inner edge can reach
	# the horizon-safe visible cap. Because each preceding ring extends farther than
	# the next ring's inner edge, this guarantees continuous terrain coverage with no
	# radial hole while avoiding rings that cannot contribute a triangle.
	var automatic_max_level: int = _active_min_level
	var visible_radius_m: float = maxf(
		_visible_cap_arc_m,
		_base_spacing * pow(2.0, float(_active_min_level)) * float(HALF_CELLS))
	while automatic_max_level < MAX_LEVEL:
		var candidate: int = automatic_max_level + 1
		var candidate_spacing: float = _base_spacing * pow(2.0, float(candidate))
		var candidate_inner_m: float = candidate_spacing * float(RING_INNER_HALF_CELLS)
		if candidate_inner_m > visible_radius_m * HORIZON_RING_SAFETY:
			break
		automatic_max_level = candidate

	_horizon_max_level = automatic_max_level
	_automatic_ring_count = maxi(automatic_max_level - _active_min_level, 0)

	# Manual mode is diagnostics only. OFF is the exact automatic production path.
	if _debug_manual_ring_count_enabled:
		var manual_count: int = clampi(_debug_manual_ring_count, 0, MAX_LEVEL - _active_min_level)
		_active_max_level = _active_min_level + manual_count
	else:
		_active_max_level = automatic_max_level
	_apply_active_level_window()


func _apply_active_level_window() -> void:
	# Keep the storage allocated by the current 0.0.5 clipmap (MAX_LEVEL slots per
	# sector). Only the visible prefix and its logical LOD IDs change. This avoids
	# renderer/RID allocation churn while preserving exactly the same horizon cull.
	_physical_ring_count = maxi(_active_max_level - _active_min_level, 0)
	_view_ring_instances = _physical_ring_count
	super._apply_active_level_window()


func _restore_dynamic_ring_window() -> void:
	# Visibility/debug operations must never resurrect horizon-culled levels. Avoid
	# submitting redundant RenderingServer property writes on ordinary frames.
	_set_view_ring_prefix(_physical_ring_count)


func _update_sector_visibility() -> void:
	if not _terrain_visible or Planet.cfg == null:
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		return

	var observer_world_d: Vec3D = Frames.to_world(camera.global_position)
	var observer_world: Vector3 = observer_world_d.to_v3()
	var observer_radius: float = observer_world.length()
	if observer_radius <= 1.0:
		_hide_surface_for_view("invalid observer")
		return
	var observer_dir: Vector3 = observer_world / observer_radius
	var forward: Vector3 = -camera.global_transform.basis.z.normalized()
	var forward_elevation: float = asin(clampf(forward.dot(observer_dir), -1.0, 1.0))
	var corner_half_angle: float = _camera_corner_half_angle(camera)
	var minimum_view_elevation: float = forward_elevation - corner_half_angle
	var maximum_view_elevation: float = forward_elevation + corner_half_angle

	# If every corner of the view cone is well above the local tangent plane, no
	# height-field terrain can enter the frame. The old ±4000 km custom AABBs made
	# Godot submit all GPU-displaced batches anyway, so explicitly suppress them.
	if minimum_view_elevation > SKY_CULL_MIN_ELEVATION_RAD:
		_hide_surface_for_view("sky")
		return

	# Precise below-ground rejection uses the asynchronous GPU terrain query. Never
	# use the coarse macro surface for this decision: procedural valleys/ridges can
	# differ from it by hundreds of metres.
	var macro_h: float = Planet.macro_height(observer_dir)
	var local_surface_h: float = macro_h
	var precise_surface := false
	var query: Node = get_node_or_null("/root/TerrainHeightQuery")
	if query != null and query.has_method("has_fresh_height") \
			and bool(query.call("has_fresh_height", observer_dir)):
		local_surface_h = float(query.call("height_for_direction", observer_dir, macro_h))
		precise_surface = true
	var agl_m: float = observer_radius - (Planet.cfg.planet_radius + local_surface_h)
	if precise_surface and agl_m < -UNDERGROUND_CULL_DEPTH_M:
		_hide_surface_for_view("underground")
		return

	_view_surface_culled = false
	_view_cull_reason = "none"

	# Keep the existing, known-good azimuth sector culling. This never changes the
	# logical LOD stack or the per-instance LOD IDs.
	super._update_sector_visibility()

	var wanted_rings: int = _physical_ring_count
	# A strongly downward frustum sees a bounded local footprint. Restrict only the
	# submitted MultiMesh instance prefix in this special case. As soon as any part
	# of the view approaches the horizon, restore the complete ring window.
	if maximum_view_elevation < NADIR_RING_LIMIT_MAX_ELEVATION_RAD \
			and _physical_ring_count > 0:
		var from_nadir: float = clampf(maximum_view_elevation + PI * 0.5, 0.0, 1.48)
		var footprint_m: float = maxf(agl_m, 0.5) * tan(from_nadir)
		var guarded_radius_m: float = footprint_m \
			+ maxf(NADIR_RING_GUARD_M, footprint_m * NADIR_RING_GUARD_SCALE)
		wanted_rings = _ring_prefix_for_radius(guarded_radius_m)
	_set_view_ring_prefix(wanted_rings)


func _camera_corner_half_angle(camera: Camera3D) -> float:
	var viewport_size: Vector2 = get_viewport().get_visible_rect().size
	var viewport_h: float = maxf(viewport_size.y, 1.0)
	var aspect: float = viewport_size.x / viewport_h
	var vertical_half: float = deg_to_rad(camera.fov) * 0.5
	var horizontal_half: float
	if camera.keep_aspect == Camera3D.KEEP_WIDTH:
		horizontal_half = vertical_half
		vertical_half = atan(tan(horizontal_half) / maxf(aspect, 1e-6))
	else:
		horizontal_half = atan(tan(vertical_half) * aspect)
	var tx: float = tan(horizontal_half)
	var ty: float = tan(vertical_half)
	return atan(sqrt(tx * tx + ty * ty))


func _ring_prefix_for_radius(radius_m: float) -> int:
	if radius_m <= 0.0 or _physical_ring_count <= 0:
		return 0
	var count := 0
	for instance_index: int in _physical_ring_count:
		var logical_level: int = _active_min_level + instance_index + 1
		var spacing: float = _base_spacing * pow(2.0, float(logical_level))
		var inner_m: float = spacing * float(RING_INNER_HALF_CELLS)
		if inner_m > radius_m:
			break
		count = instance_index + 1
	return count


func _set_view_ring_prefix(wanted: int) -> void:
	wanted = clampi(wanted, 0, _physical_ring_count)
	_view_ring_instances = wanted
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh == null:
			continue
		var actual: int = mini(wanted, batch.multimesh.instance_count)
		if batch.multimesh.visible_instance_count != actual:
			batch.multimesh.visible_instance_count = actual


func _hide_surface_for_view(reason: String) -> void:
	_view_surface_culled = true
	_view_cull_reason = reason
	_view_ring_instances = 0
	_visible_sector_count = 0
	for batch: MultiMeshInstance3D in _center_sector_batches:
		batch.visible = false
	for batch: MultiMeshInstance3D in _sector_batches:
		batch.visible = false


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


func set_debug_manual_ring_count_enabled(value: bool) -> void:
	if _debug_manual_ring_count_enabled == value:
		return
	if value:
		# Enter manual mode without changing the currently correct automatic shape.
		_debug_manual_ring_count = _automatic_ring_count
	_debug_manual_ring_count_enabled = value
	_refresh_manual_ring_window()


func set_debug_manual_ring_count(value: int) -> void:
	_debug_manual_ring_count = clampi(value, 0, MAX_LEVEL)
	if _debug_manual_ring_count_enabled:
		_refresh_manual_ring_window()


func _refresh_manual_ring_window() -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return
	_update_active_levels()
	if _terrain_visible:
		_update_sector_visibility()


func debug_manual_ring_count_enabled() -> bool:
	return _debug_manual_ring_count_enabled


func debug_manual_ring_count() -> int:
	return _debug_manual_ring_count


func automatic_ring_count() -> int:
	return _automatic_ring_count


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
	out["automatic_ring_count"] = _automatic_ring_count
	out["manual_ring_count_enabled"] = _debug_manual_ring_count_enabled
	out["manual_ring_count"] = _debug_manual_ring_count
	out["horizon_exact_ring_window"] = true
	out["ring_storage_fixed"] = true
	out["view_surface_culled"] = _view_surface_culled
	out["view_cull_reason"] = _view_cull_reason
	out["view_ring_instances"] = _view_ring_instances
	out["nadir_ring_submission_cull"] = true
	out["sky_submission_cull"] = true
	out["physical_surface_classifier"] = true
	out["scanned_pbr"] = _pbr_bound and _debug_pbr_enabled
	out["terrain_aerial_perspective"] = true
	out["terrain_aerial_strength"] = _aerial_strength
	return out
