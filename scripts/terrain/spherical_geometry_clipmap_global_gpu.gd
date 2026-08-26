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

# View-dependent submission. The old radial fallback used abs(forward.dot(up)),
# which made both straight-down and straight-up views submit every terrain sector
# and every horizon ring. Nine view rays are enough to distinguish three cases:
# horizon in view, a bounded down-looking surface footprint, or no surface at all.
const VIEW_SAMPLE_AXIS: int = 3
const VIEW_ARC_MARGIN_M: float = 8.0
const VIEW_HORIZON_ANGLE_MARGIN_RAD: float = 0.010
const UNDERGROUND_CULL_DEPTH_M: float = 2.0

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

var _view_surface_visible := true
var _view_ring_count: int = 0
var _view_max_arc_m: float = 0.0
var _view_underground := false


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
	# contribute a single triangle, so it is omitted from the visible instance window.
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
	# Keep the storage allocated by the current 0.0.5 clipmap (MAX_LEVEL slots per
	# sector). Only the visible prefix and its logical LOD IDs change. This avoids
	# renderer/RID allocation churn while preserving exactly the same horizon cull.
	_physical_ring_count = maxi(_active_max_level - _active_min_level, 0)
	super._apply_active_level_window()


func _set_visible(value: bool) -> void:
	# The procedural parent calls _set_visible(true) and then performs its explicit
	# camera visibility update. Avoid doing the same sector traversal twice on every
	# ordinary frame. A transition from hidden to visible still evaluates once here.
	var changed: bool = _terrain_visible != value
	_terrain_visible = value
	if not value:
		_view_surface_visible = false
		_view_ring_count = 0
		_visible_sector_count = 0
		for batch: MultiMeshInstance3D in _center_sector_batches:
			batch.visible = false
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return
	if changed:
		_update_sector_visibility()


func _update_sector_visibility() -> void:
	if not _terrain_visible or Planet.cfg == null:
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		return

	var observer_d: Vec3D = Frames.to_world(camera.global_position)
	var observer_world: Vector3 = observer_d.to_v3()
	var observer_radius: float = observer_world.length()
	if observer_radius <= 1.0:
		_hide_view_surface()
		return
	var observer_dir: Vector3 = observer_world / observer_radius

	# Use the asynchronous GPU surface query when it is fresh. This is important
	# for procedural valleys/ridges: the macro map alone can be hundreds of metres
	# away from the rendered surface and is not reliable enough for underground
	# rejection by itself.
	var macro_h: float = Planet.macro_height(observer_dir)
	var surface_h: float = macro_h
	var precise_surface := false
	var query: Node = get_node_or_null("/root/TerrainHeightQuery")
	if query != null and query.has_method("has_fresh_height") \
			and bool(query.call("has_fresh_height", observer_dir)):
		surface_h = float(query.call("height_for_direction", observer_dir, macro_h))
		precise_surface = true

	var surface_radius: float = Planet.cfg.planet_radius + surface_h
	var signed_surface_altitude: float = observer_radius - surface_radius
	_view_underground = precise_surface \
		and signed_surface_altitude < -UNDERGROUND_CULL_DEPTH_M
	if _view_underground:
		_hide_view_surface()
		return

	# If the coarse map happens to put a legitimate procedural valley camera inside
	# its reference sphere while no precise query is available, never intersect
	# rays against the far side of the planet. Treat the reference surface as just
	# below the camera until the GPU query catches up.
	if not precise_surface and surface_radius >= observer_radius:
		surface_radius = observer_radius - 0.25

	var max_arc_m: float = _frustum_surface_arc_m(
		camera, observer_world, observer_dir, surface_radius)
	if max_arc_m < 0.0:
		_hide_view_surface()
		return

	_view_surface_visible = true
	_view_max_arc_m = minf(_visible_cap_arc_m, max_arc_m + VIEW_ARC_MARGIN_M)
	_view_ring_count = _ring_count_for_view_arc(_view_max_arc_m)

	var forward: Vector3 = -camera.global_transform.basis.z.normalized()
	var radial_dot: float = forward.dot(_center_dir)
	var forward_plane := Vector2(forward.dot(_center_right), forward.dot(_center_up))

	# A down-looking frustum has a circular/elliptic surface footprint around the
	# centre and therefore legitimately spans every azimuth. The crucial difference
	# from the old code is that only the rings intersecting that footprint remain.
	var show_all_azimuth: bool = radial_dot <= -SECTOR_SHOW_ALL_RADIAL_DOT \
		or forward_plane.length_squared() < 1e-5

	var cos_limit: float = -1.0
	var forward_2d := Vector2.RIGHT
	if not show_all_azimuth:
		forward_2d = forward_plane.normalized()
		var viewport_size: Vector2 = get_viewport().get_visible_rect().size
		var aspect: float = viewport_size.x / maxf(viewport_size.y, 1.0)
		var vertical_half: float = deg_to_rad(camera.fov) * 0.5
		var horizontal_half: float = atan(tan(vertical_half) * aspect)
		var limit: float = minf(horizontal_half + SECTOR_HALF_ANGLE
			+ SECTOR_CULL_MARGIN_RAD, PI)
		cos_limit = cos(limit)

	_visible_sector_count = 0
	for sector: int in SECTOR_COUNT:
		var sector_visible: bool = show_all_azimuth
		if not show_all_azimuth:
			var angle: float = (float(sector) + 0.5) * TAU / float(SECTOR_COUNT)
			var sector_dir := Vector2(cos(angle), sin(angle))
			sector_visible = sector_dir.dot(forward_2d) >= cos_limit

		var center: MultiMeshInstance3D = _center_sector_batches[sector]
		var rings: MultiMeshInstance3D = _sector_batches[sector]
		center.visible = sector_visible
		rings.visible = sector_visible and _view_ring_count > 0
		if rings.multimesh != null:
			var wanted: int = _view_ring_count if sector_visible else 0
			wanted = mini(wanted, rings.multimesh.instance_count)
			if rings.multimesh.visible_instance_count != wanted:
				rings.multimesh.visible_instance_count = wanted
		if sector_visible:
			_visible_sector_count += 1


func _hide_view_surface() -> void:
	_view_surface_visible = false
	_view_ring_count = 0
	_view_max_arc_m = 0.0
	_visible_sector_count = 0
	for batch: MultiMeshInstance3D in _center_sector_batches:
		batch.visible = false
	for batch: MultiMeshInstance3D in _sector_batches:
		batch.visible = false
		if batch.multimesh != null and batch.multimesh.visible_instance_count != 0:
			batch.multimesh.visible_instance_count = 0


func _frustum_surface_arc_m(camera: Camera3D, observer_world: Vector3,
		observer_dir: Vector3, surface_radius: float) -> float:
	var viewport_size: Vector2 = get_viewport().get_visible_rect().size
	if viewport_size.x <= 1.0 or viewport_size.y <= 1.0:
		return _visible_cap_arc_m

	var min_elevation := INF
	var max_elevation := -INF
	var max_hit_arc_m := -1.0
	var denom: float = float(VIEW_SAMPLE_AXIS - 1)

	for yi: int in VIEW_SAMPLE_AXIS:
		var sy: float = viewport_size.y * float(yi) / denom
		for xi: int in VIEW_SAMPLE_AXIS:
			var sx: float = viewport_size.x * float(xi) / denom
			var ray: Vector3 = camera.project_ray_normal(Vector2(sx, sy)).normalized()
			var elevation: float = asin(clampf(ray.dot(observer_dir), -1.0, 1.0))
			min_elevation = minf(min_elevation, elevation)
			max_elevation = maxf(max_elevation, elevation)

			var hit_t: float = _ray_sphere_nearest_t(observer_world, ray, surface_radius)
			if hit_t <= 0.0:
				continue
			var hit_dir: Vector3 = (observer_world + ray * hit_t).normalized()
			var arc_m: float = acos(clampf(_center_dir.dot(hit_dir), -1.0, 1.0)) \
				* Planet.cfg.planet_radius
			max_hit_arc_m = maxf(max_hit_arc_m, arc_m)

	# When the planetary limb crosses the frustum, exact ray samples near the limb
	# are numerically fragile and a 3x3 grid can sit on either side of the tangent.
	# In that case retain the already horizon-capped LOD window. This preserves the
	# long view straight ahead while still allowing aggressive nadir/zenith culls.
	var observer_radius: float = observer_world.length()
	var horizon_dip: float = 0.0
	if observer_radius > surface_radius + 1e-4:
		horizon_dip = acos(clampf(surface_radius / observer_radius, -1.0, 1.0))
	var horizon_elevation: float = -horizon_dip
	if horizon_elevation >= min_elevation - VIEW_HORIZON_ANGLE_MARGIN_RAD \
			and horizon_elevation <= max_elevation + VIEW_HORIZON_ANGLE_MARGIN_RAD:
		return _visible_cap_arc_m

	return max_hit_arc_m


static func _ray_sphere_nearest_t(origin: Vector3, direction: Vector3,
		radius: float) -> float:
	var b: float = origin.dot(direction)
	var c: float = origin.length_squared() - radius * radius
	var discriminant: float = b * b - c
	if discriminant < 0.0:
		return -1.0
	var root: float = sqrt(discriminant)
	var near_t: float = -b - root
	if near_t > 1e-4:
		return near_t
	var far_t: float = -b + root
	return far_t if far_t > 1e-4 else -1.0


func _ring_count_for_view_arc(max_arc_m: float) -> int:
	if max_arc_m <= 0.0 or _physical_ring_count <= 0:
		return 0
	var count := 0
	for instance_index: int in _physical_ring_count:
		var logical_level: int = _active_min_level + instance_index + 1
		var spacing: float = _base_spacing * pow(2.0, float(logical_level))
		var inner_m: float = spacing * float(RING_INNER_HALF_CELLS)
		if inner_m > max_arc_m:
			break
		count = instance_index + 1
	return count


func _restore_dynamic_ring_window() -> void:
	# Debug operations may explicitly ask for the complete logical window. Ordinary
	# view culling writes its own smaller per-sector prefix in _update_sector_visibility.
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh == null:
			continue
		var wanted: int = mini(_physical_ring_count, batch.multimesh.instance_count)
		if batch.multimesh.visible_instance_count != wanted:
			batch.multimesh.visible_instance_count = wanted


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
	out["horizon_exact_ring_window"] = true
	out["ring_storage_fixed"] = true
	out["view_surface_visible"] = _view_surface_visible
	out["view_ring_instances"] = _view_ring_count
	out["view_max_arc_m"] = _view_max_arc_m
	out["view_underground_cull"] = _view_underground
	out["frustum_surface_culling"] = true
	out["physical_surface_classifier"] = true
	out["scanned_pbr"] = _pbr_bound and _debug_pbr_enabled
	out["terrain_aerial_perspective"] = true
	out["terrain_aerial_strength"] = _aerial_strength
	return out
