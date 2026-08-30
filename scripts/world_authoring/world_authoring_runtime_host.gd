class_name WorldAuthoringRuntimeHost
extends Node
## Installs Planet Studio over Main and owns the boundary between staged authoring
## resources and the production runtime. BLANK terrain is an analytic backend: it
## never invokes PlanetBake and owns no generated height/material map.
##
## The host also owns the live celestial preview boundary. The selected body is
## treated as the local body-centred origin. If it owns the generated terrestrial
## runtime that terrain remains at the origin while every other staged body is
## rendered by the lightweight celestial-system preview at its orbital offset.

const LIVE_EDITOR_SCRIPT := preload("res://scripts/world_authoring/world_authoring_editor_live_phase22.gd")
const BIOME_PREVIEW_SCRIPT := preload("res://scripts/world_authoring/biome_authoring_preview.gd")
const CELESTIAL_PREVIEW_SCRIPT := preload("res://scripts/world_authoring/celestial_body_preview_runtime.gd")
const APPLY_PLANNER_SCRIPT := preload("res://scripts/world_authoring/world_authoring_apply_planner.gd")
const TERRAIN_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const AUTHORED_WATER_RUNTIME_SPATIAL_PATH := "res://scripts/world_authoring/authored_water_runtime_spatial.gd"
const AUTHORED_WATER_RUNTIME_QUERY_PATH := "res://scripts/world_authoring/authored_water_runtime_query.gd"
const AUTHORED_WATER_RUNTIME_BASE_PATH := "res://scripts/world_authoring/authored_water_runtime.gd"

const BASE_RAYLEIGH_COEFF := Vector3(5.5e-6, 13.0e-6, 22.4e-6)
const BASE_OZONE_COEFF := Vector3(0.650e-6, 1.881e-6, 0.085e-6)
const BASE_MIE_COEFF: float = 21.0e-6
const DEFAULT_CLOUD_THICKNESS_M: float = 5300.0
const BODY_FRAME_MARGIN: float = 1.18
const BODY_FRAME_SURFACE_MARGIN: float = 1.04
const FAMILY_FRAME_TRIGGER: float = 1.50

var _main: Node
var _layer: CanvasLayer
var _editor: Control
var _biome_preview: Node
var _authored_water_runtime: Node
var _celestial_preview: Node3D
var _authoring_session: WorldAuthoringSession
var _preview_player: Node
var _runtime_applied_snapshot: Resource
var _preview_body_id: String = ""
var _preview_sync_pending: bool = false
var _preview_focus_pending: bool = false
var _terrestrial_runtime_visible: bool = true
var _pending_apply_scope: int = 0
var _opened: bool = false

func _ready() -> void:
	_main = get_parent()
	if _launch_mode() != "planet_studio":
		set_process(false)
		return
	set_process(true)

func _process(_delta: float) -> void:
	if _opened or _main == null:
		return
	var player: Node = _main.get("player") as Node
	if player == null:
		return
	_open_live_editor(player)

func _launch_mode() -> String:
	if not get_tree().has_meta("launch_mode"):
		return "play"
	return String(get_tree().get_meta("launch_mode"))

func _open_live_editor(player: Node) -> void:
	_opened = true
	_preview_player = player
	_set_existing_ui_visible(false)
	player.set("input_enabled", false)
	if player.has_method("set_mouse_captured"):
		player.call("set_mouse_captured", false)
	_layer = CanvasLayer.new()
	_layer.name = "PlanetStudioLiveLayer"
	_layer.layer = 90
	add_child(_layer)
	var live_editor: Control = LIVE_EDITOR_SCRIPT.new()
	live_editor.name = "PlanetStudioLive"
	live_editor.call("bind_world", _main)
	live_editor.connect("runtime_apply_requested", Callable(self, "_on_runtime_apply_requested"))
	if live_editor.has_signal("body_focus_requested"):
		live_editor.connect("body_focus_requested", Callable(self, "_on_body_focus_requested"))
	_layer.add_child(live_editor)
	_editor = live_editor

	var session_value: Variant = live_editor.get("_session")
	if session_value is WorldAuthoringSession:
		var session: WorldAuthoringSession = session_value as WorldAuthoringSession
		_authoring_session = session
		if session.applied_system != null:
			_runtime_applied_snapshot = session.applied_system.duplicate(true)
		if not session.changed.is_connected(_on_authoring_session_changed):
			session.changed.connect(_on_authoring_session_changed)

		var celestial_preview: Node3D = CELESTIAL_PREVIEW_SCRIPT.new() as Node3D
		if celestial_preview != null:
			celestial_preview.name = "PlanetStudioCelestialSystemPreview"
			_main.add_child(celestial_preview)
			_celestial_preview = celestial_preview

		var biome_preview: Node = BIOME_PREVIEW_SCRIPT.new()
		biome_preview.name = "PlanetStudioBiomePreview"
		biome_preview.call("bind", session, _main)
		add_child(biome_preview)
		_biome_preview = biome_preview
		if live_editor.has_signal("biome_preview_stroke_added"):
			live_editor.connect("biome_preview_stroke_added",
				Callable(biome_preview, "append_transient_stroke"))
		if live_editor.has_signal("biome_preview_transient_cleared"):
			live_editor.connect("biome_preview_transient_cleared",
				Callable(biome_preview, "clear_transient_strokes"))

		var authored_water_script: Script = _resolve_authored_water_runtime_script()
		if authored_water_script != null:
			var authored_water: Node = authored_water_script.new() as Node
			if authored_water != null:
				authored_water.name = "PlanetStudioAuthoredWaterRuntime"
				authored_water.call("bind", session, _main)
				add_child(authored_water)
				authored_water.add_to_group(&"authored_water_query")
				_authored_water_runtime = authored_water
				if live_editor.has_signal("water_preview_changed"):
					live_editor.connect("water_preview_changed", Callable(authored_water, "mark_dirty"))
		else:
			_set_editor_status("Authored-water runtime scripts are missing from this checkout; terrain authoring remains available.")

		if Planet.has_signal("world_ready") and not Planet.world_ready.is_connected(_on_planet_world_ready):
			Planet.world_ready.connect(_on_planet_world_ready)
		_schedule_active_body_preview(true)
	set_process(false)

func _on_authoring_session_changed(dirty_state: bool, apply_scope: int) -> void:
	if dirty_state:
		_pending_apply_scope = apply_scope
	var active_id: String = _active_staged_body_id()
	_schedule_active_body_preview(active_id != _preview_body_id)

func _on_body_focus_requested(_body_id: String) -> void:
	_schedule_active_body_preview(true)

func _on_planet_world_ready(_fields: PlanetFields) -> void:
	# Main finishes adopting the new fields and clears its rebake flag in the same
	# frame. Defer one turn before replacing the staged selected sphere with terrain.
	_schedule_active_body_preview(true)

func _active_staged_body_id() -> String:
	if _authoring_session == null or _authoring_session.staged_system == null:
		return ""
	return String(_authoring_session.staged_system.get(&"active_body_id"))

func _schedule_active_body_preview(focus: bool = false) -> void:
	_preview_focus_pending = _preview_focus_pending or focus
	if _preview_sync_pending:
		return
	_preview_sync_pending = true
	call_deferred("_flush_active_body_preview")

func _flush_active_body_preview() -> void:
	_preview_sync_pending = false
	if _authoring_session == null or _main == null:
		_preview_focus_pending = false
		return
	var body: Resource = _authoring_session.active_body() as Resource
	if body == null:
		_preview_focus_pending = false
		return
	var body_id: String = String(body.get(&"body_id"))
	var body_changed: bool = body_id != _preview_body_id
	_preview_body_id = body_id

	var body_type: int = int(body.get(&"body_type"))
	var is_star: bool = body_type == BODY_SCRIPT.BodyType.STAR
	var applied_id: String = ""
	if _runtime_applied_snapshot != null:
		applied_id = String(_runtime_applied_snapshot.get(&"active_body_id"))
	var rebaking: bool = bool(_main.get("_rebaking"))
	var owns_live_terrain: bool = not is_star and body_id == applied_id and not rebaking
	var staged_preview: bool = not owns_live_terrain

	_set_terrestrial_runtime_visible(owns_live_terrain)
	if _celestial_preview != null:
		# Even when the selected body owns the detailed terrain runtime, keep all
		# *other* bodies rendered. For staged bodies the selected sphere is rendered
		# too because no generated terrain belongs to it yet.
		_celestial_preview.call("show_system", _authoring_session.staged_system,
			body_id, staged_preview)

	var should_focus: bool = _preview_focus_pending or body_changed
	_preview_focus_pending = false
	if should_focus:
		_focus_camera_on_body(body, staged_preview)
		if is_star:
			_set_editor_status("Focused %s — stellar preview active; orbital companions remain visible." % String(body.get(&"display_name")))
		elif staged_preview:
			_set_editor_status("Focused %s — staged body preview with parent/moons kept at their orbital positions. Apply to generate this body's own terrain." % String(body.get(&"display_name")))
		else:
			_set_editor_status("Focused %s — applied terrain active; moons and other staged bodies remain visible around it." % String(body.get(&"display_name")))

func _focus_camera_on_body(body: Resource, staged_preview: bool) -> void:
	if _preview_player == null or body == null:
		return
	var camera: Camera3D = _preview_player.get("camera") as Camera3D
	if camera == null:
		return
	var radius_m: float = maxf(float(body.get(&"radius_m")), 1.0)
	var selected_visual_radius: float = radius_m * BODY_FRAME_SURFACE_MARGIN
	var frame_radius: float = selected_visual_radius
	var system_extent: float = selected_visual_radius
	if _celestial_preview != null:
		selected_visual_radius = maxf(float(_celestial_preview.call("visual_radius_m")), radius_m)
		frame_radius = maxf(selected_visual_radius,
			float(_celestial_preview.call("family_frame_radius_m")))
		system_extent = maxf(frame_radius,
			float(_celestial_preview.call("system_extent_m")))
	elif staged_preview:
		selected_visual_radius = radius_m
		frame_radius = selected_visual_radius

	var frame_distance: float = float(CELESTIAL_PREVIEW_SCRIPT.frame_distance_for_radius(
		frame_radius, camera.fov, BODY_FRAME_MARGIN))

	var radial_axis := Vector3(1.0, 0.18, 0.32).normalized()
	var current_world: Vec3D = _preview_player.get("world_pos") as Vec3D
	if current_world != null and current_world.length_sq() > 1.0:
		radial_axis = current_world.normalized().to_v3()
	# The preview's Keplerian reference plane is XZ. Looking down the Y axis keeps
	# a nearby parent/moon pair spread across the viewport instead of stacking one
	# behind the other. Single-body focus preserves the author's current radial side.
	if frame_radius > selected_visual_radius * FAMILY_FRAME_TRIGGER:
		radial_axis = Vector3.UP
	var next_world: Vec3D = Vec3D.from_v3(radial_axis).mul(frame_distance)
	_preview_player.set("world_pos", next_world)
	# At -90 degrees the player's spherical camera points exactly toward -up, i.e.
	# the selected body's centre. Yaw remains available for tangent navigation.
	_preview_player.set("pitch", -PI * 0.5)
	Frames.rebase(next_world)
	if _preview_player.has_method("_sync_transform"):
		_preview_player.call("_sync_transform")
	camera.far = maxf(camera.far, frame_distance + system_extent * 1.10)
	_preview_player.emit_signal("moved", next_world)

func _set_terrestrial_runtime_visible(value: bool) -> void:
	if _terrestrial_runtime_visible == value:
		return
	_terrestrial_runtime_visible = value
	var nodes: Array[Node] = []
	for candidate: Variant in [
		_main.get("terrain"),
		_main.get("orbit_ocean"),
		get_node_or_null("/root/GroundGeometryClipmap"),
		get_node_or_null("/root/OceanGeometryClipmap"),
		get_node_or_null("/root/TerrainScatter"),
	]:
		if candidate is Node:
			nodes.append(candidate as Node)
	for node: Node in nodes:
		if node is Node3D:
			(node as Node3D).visible = value
		node.set_process(value)
		node.set_physics_process(value)
	if _authored_water_runtime != null:
		_authored_water_runtime.set_process(value)
		if _authored_water_runtime.has_method("_set_visible"):
			_authored_water_runtime.call("_set_visible", value)
		if value and _authored_water_runtime.has_method("mark_dirty"):
			_authored_water_runtime.call("mark_dirty")
	if value:
		_refresh_clipmap_without_bake()

func _resolve_authored_water_runtime_script() -> Script:
	var candidates := PackedStringArray([
		AUTHORED_WATER_RUNTIME_SPATIAL_PATH,
		AUTHORED_WATER_RUNTIME_QUERY_PATH,
		AUTHORED_WATER_RUNTIME_BASE_PATH,
	])
	for path: String in candidates:
		if not ResourceLoader.exists(path, "Script"):
			continue
		var resource: Resource = load(path)
		if resource is Script:
			return resource as Script
	return null

func _set_existing_ui_visible(visible: bool) -> void:
	for property_name: StringName in [&"hud", &"map", &"debug_menu", &"coastline_profile_editor"]:
		var node: CanvasItem = _main.get(property_name) as CanvasItem
		if node != null:
			node.visible = visible

func _on_runtime_apply_requested(system: Resource) -> void:
	if _main == null or system == null:
		return
	var captured_scope: int = _pending_apply_scope
	_pending_apply_scope = 0
	var plan: Dictionary = APPLY_PLANNER_SCRIPT.build(
		_runtime_applied_snapshot, system, captured_scope)

	var body: Resource = system.call("active_body") as Resource
	if body == null:
		_set_editor_status("Apply rejected: no active celestial body.")
		return
	if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		_set_editor_status("Applied stellar authoring state for %s — no terrestrial terrain rebuild." % String(body.get(&"display_name")))
		_runtime_applied_snapshot = system.duplicate(true)
		_schedule_active_body_preview(false)
		return

	var profile: Resource = body.get(&"planet_profile") as Resource
	if profile == null:
		_set_editor_status("Apply rejected: active body has no terrestrial profile.")
		return
	var terrain_profile: Resource = profile.get(&"terrain") as Resource
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource
	var water: Resource = profile.get(&"water") as Resource
	if terrain_profile == null:
		_set_editor_status("Apply rejected: active body has no terrain profile.")
		return
	var generation: Resource = terrain_profile.get(&"generation_profile") as Resource
	var cfg: Resource = _main.get("cfg") as Resource
	if generation == null or cfg == null:
		_set_editor_status("Apply rejected: generation/runtime configuration is unavailable.")
		return

	var blank: bool = int(terrain_profile.get(&"generation_mode")) \
		== TERRAIN_PROFILE_SCRIPT.GenerationMode.BLANK
	if blank:
		_apply_blank_terrain(body, atmosphere, generation, terrain_profile, cfg)
		_runtime_applied_snapshot = system.duplicate(true)
		_schedule_active_body_preview(false)
		return

	# Procedural runtime. Deltas are meaningful only in this backend.
	if bool(Planet.get("blank_mode")):
		Planet.call("set_blank_mode", false)
		_set_ground_generated_height_enabled(true)
	if bool(plan.get("sculpt", false)) or bool(plan.get("full_rebuild", false)):
		_apply_sculpt_state(terrain_profile)

	if bool(plan.get("full_rebuild", false)):
		_apply_full_rebuild(plan, body, atmosphere, generation, cfg)
		_runtime_applied_snapshot = system.duplicate(true)
		_schedule_active_body_preview(false)
		return

	var refreshed: PackedStringArray = PackedStringArray()
	if bool(plan.get("clipmap", false)):
		generation.call("copy_to_resource", cfg)
		_sync_body_config(body, atmosphere, cfg)
		Planet.configure(cfg)
		_refresh_clipmap_without_bake()
		refreshed.append("terrain clipmap/runtime parameters")
	elif bool(plan.get("frames", false)):
		_sync_frames(body, cfg)
		refreshed.append("planet frame/rotation")

	if bool(plan.get("atmosphere", false)):
		if atmosphere != null:
			cfg.set(&"atmosphere_height",
				maxf(1.0, float(atmosphere.get(&"atmosphere_height_m"))))
		_apply_atmosphere_hot(body, atmosphere, cfg)
		refreshed.append("atmosphere/cloud uniforms")

	if bool(plan.get("biome", false)):
		_mark_biome_dirty()
		refreshed.append("authored biome preview")
	if bool(plan.get("water_geometry", false)):
		_mark_water_dirty()
		refreshed.append("authored water geometry")
	elif bool(plan.get("water_material", false)):
		_mark_water_dirty()
		refreshed.append("water material/wave parameters")
	if bool(plan.get("ocean", false)):
		_refresh_ocean_runtime()
		refreshed.append("ocean runtime")
	if bool(plan.get("tiles", false)):
		_mark_biome_dirty()
		_mark_water_dirty()
		refreshed.append("local authored tiles")
	if bool(plan.get("graph", false)):
		refreshed.append("shader graph staging")
	if bool(plan.get("sculpt", false)):
		refreshed.append("sparse sculpt deltas")

	_runtime_applied_snapshot = system.duplicate(true)
	_schedule_active_body_preview(false)
	if refreshed.is_empty():
		_set_editor_status("Applied %s metadata only — no terrain, heightmap or biome rebuild required." % String(body.get(&"display_name")))
	else:
		_set_editor_status("Applied %s without PlanetBake — refreshed: %s." % [
			String(body.get(&"display_name")), ", ".join(refreshed)])

func _apply_blank_terrain(body: Resource, atmosphere: Resource, generation: Resource,
		terrain_profile: Resource, cfg: Resource) -> void:
	# Runtime config still carries radius and clipmap resolution controls, but none
	# of the dormant macro/geology/climate values are evaluated while Blank is live.
	generation.call("copy_to_resource", cfg)
	_sync_body_config(body, atmosphere, cfg)
	Planet.configure(cfg)
	Planet.call("set_blank_mode", true)

	# Persisted sculpt data is intentionally retained in TerrainAuthoringProfile for
	# lossless mode switching, but it is not part of Blank's runtime surface.
	Deltas.clear()
	_set_ground_generated_height_enabled(false)
	_refresh_clipmap_without_bake()
	_mark_biome_dirty()
	_apply_atmosphere_hot(body, atmosphere, cfg)
	_set_editor_status("Applied %s as BLANK terrain — analytic sphere, no PlanetBake, no generated heightmap/material/biome map. Terrain shape is shader-authored; only custom biome paint is categorical." % String(body.get(&"display_name")))

func _apply_full_rebuild(plan: Dictionary, body: Resource, atmosphere: Resource,
		generation: Resource, cfg: Resource) -> void:
	if bool(_main.get("_rebaking")):
		_set_editor_status("A generator rebuild is already running; this Apply was not started.")
		return
	Planet.call("set_blank_mode", false)
	_set_ground_generated_height_enabled(true)
	generation.call("copy_to_resource", cfg)
	_sync_body_config(body, atmosphere, cfg)
	Planet.configure(cfg)
	_apply_atmosphere_hot(body, atmosphere, cfg)
	_mark_biome_dirty()
	_mark_water_dirty()
	_main.call("_on_rebake_requested")
	var reason: String = String(plan.get("reason", "generator data changed"))
	if reason.is_empty():
		reason = "generator data changed"
	_set_editor_status("Applying %s — full PlanetBake required (%s). Generated height/climate/biomes are rebuilding." % [
		String(body.get(&"display_name")), reason])

func _apply_sculpt_state(terrain_profile: Resource) -> void:
	if not terrain_profile.has_method("sculpt_delta_serialized"):
		return
	var sculpt_value: Variant = terrain_profile.call("sculpt_delta_serialized")
	if sculpt_value is Dictionary:
		Deltas.deserialize(sculpt_value as Dictionary)

func _set_ground_generated_height_enabled(enabled: bool) -> void:
	var ground: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if ground != null and ground.has_method("set_heightmap_enabled"):
		ground.call("set_heightmap_enabled", enabled)

func _sync_body_config(body: Resource, atmosphere: Resource, cfg: Resource) -> void:
	cfg.set(&"planet_radius", maxf(1.0, float(body.get(&"radius_m"))))
	cfg.set(&"axial_tilt_deg", float(body.get(&"axial_tilt_deg")))
	if atmosphere != null:
		cfg.set(&"atmosphere_height",
			maxf(1.0, float(atmosphere.get(&"atmosphere_height_m"))))
	_sync_frames(body, cfg)

func _sync_frames(body: Resource, cfg: Resource) -> void:
	Frames.set_planet_radius(float(cfg.get(&"planet_radius")))
	Frames.axial_tilt_deg = float(body.get(&"axial_tilt_deg"))
	Frames.day_seconds = maxf(0.001,
		absf(float(body.get(&"sidereal_rotation_period_s"))))

func _refresh_clipmap_without_bake() -> void:
	var terrain_node: Node = _main.get("terrain") as Node
	if terrain_node != null and terrain_node.has_method("build_roots"):
		terrain_node.call("build_roots")
	var ground: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if ground != null and ground.has_method("_configure_world"):
		ground.call("_configure_world")
	_refresh_ocean_runtime()
	var runtime_editor: Node = _main.get("editor") as Node
	if runtime_editor != null and runtime_editor.has_method("refresh"):
		runtime_editor.call("refresh")

func _refresh_ocean_runtime() -> void:
	var orbit_ocean: Node = _main.get("orbit_ocean") as Node
	if orbit_ocean != null and orbit_ocean.has_method("refresh_surface"):
		orbit_ocean.call("refresh_surface")
	var local_ocean: Node = get_node_or_null("/root/OceanGeometryClipmap")
	if local_ocean != null and local_ocean.has_method("_configure_world"):
		local_ocean.call("_configure_world")

func _mark_biome_dirty() -> void:
	if _biome_preview != null and _biome_preview.has_method("mark_dirty"):
		_biome_preview.call("mark_dirty")

func _mark_water_dirty() -> void:
	if _authored_water_runtime != null and _authored_water_runtime.has_method("mark_dirty"):
		_authored_water_runtime.call("mark_dirty")

func _apply_atmosphere_hot(body: Resource, atmosphere: Resource,
		cfg: Resource) -> void:
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material == null:
		return
	var radius: float = maxf(1.0, float(body.get(&"radius_m")))
	var atmosphere_height: float = maxf(1.0, float(cfg.get(&"atmosphere_height")))
	sky_material.set_shader_parameter("u_planet_radius", radius)
	sky_material.set_shader_parameter("u_atmosphere_radius", radius + atmosphere_height)
	if atmosphere == null:
		return
	var enabled: bool = bool(atmosphere.get(&"enabled"))
	var enabled_scale: float = 1.0 if enabled else 0.0
	sky_material.set_shader_parameter("u_rayleigh_coeff", BASE_RAYLEIGH_COEFF
		* maxf(float(atmosphere.get(&"rayleigh_strength")), 0.0) * enabled_scale)
	sky_material.set_shader_parameter("u_mie_coeff", BASE_MIE_COEFF
		* maxf(float(atmosphere.get(&"mie_strength")), 0.0) * enabled_scale)
	sky_material.set_shader_parameter("u_ozone_coeff", BASE_OZONE_COEFF
		* maxf(float(atmosphere.get(&"ozone_strength")), 0.0) * enabled_scale)
	sky_material.set_shader_parameter("u_cloud_enabled", enabled_scale)
	sky_material.set_shader_parameter("u_cloud_coverage",
		clampf(float(atmosphere.get(&"cloud_coverage")), 0.0, 1.0))
	sky_material.set_shader_parameter("u_cloud_density",
		maxf(float(atmosphere.get(&"cloud_density")), 0.0))
	var cloud_base: float = maxf(float(atmosphere.get(&"cloud_altitude_m")), 1.0)
	sky_material.set_shader_parameter("u_cloud_base", cloud_base)
	sky_material.set_shader_parameter("u_cloud_top", cloud_base + DEFAULT_CLOUD_THICKNESS_M)

func _update_environment_radius(cfg: Resource) -> void:
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material == null:
		return
	var radius: float = float(cfg.get(&"planet_radius"))
	var atmosphere_height: float = float(cfg.get(&"atmosphere_height"))
	sky_material.set_shader_parameter("u_planet_radius", radius)
	sky_material.set_shader_parameter("u_atmosphere_radius", radius + atmosphere_height)

func _set_editor_status(message: String) -> void:
	if _editor != null and _editor.has_method("_set_status"):
		_editor.call("_set_status", message)
