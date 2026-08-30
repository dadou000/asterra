class_name WorldAuthoringRuntimeHost
extends Node
## Installs Planet Studio over Main when launched in planet_studio mode and owns
## the deliberate boundary between staged authoring resources and the production
## one-planet runtime. Apply now computes the minimum dirty runtime subsystems so
## HOT/local authoring never falls through to the expensive PlanetBake path.

const LIVE_EDITOR_SCRIPT := preload("res://scripts/world_authoring/world_authoring_editor_live_phase18.gd")
const BIOME_PREVIEW_SCRIPT := preload("res://scripts/world_authoring/biome_authoring_preview.gd")
const APPLY_PLANNER_SCRIPT := preload("res://scripts/world_authoring/world_authoring_apply_planner.gd")
const AUTHORED_WATER_RUNTIME_SPATIAL_PATH := "res://scripts/world_authoring/authored_water_runtime_spatial.gd"
const AUTHORED_WATER_RUNTIME_QUERY_PATH := "res://scripts/world_authoring/authored_water_runtime_query.gd"
const AUTHORED_WATER_RUNTIME_BASE_PATH := "res://scripts/world_authoring/authored_water_runtime.gd"

const BASE_RAYLEIGH_COEFF := Vector3(5.5e-6, 13.0e-6, 22.4e-6)
const BASE_OZONE_COEFF := Vector3(0.650e-6, 1.881e-6, 0.085e-6)
const BASE_MIE_COEFF: float = 21.0e-6
const DEFAULT_CLOUD_THICKNESS_M: float = 5300.0

var _main: Node
var _layer: CanvasLayer
var _editor: Control
var _biome_preview: Node
var _authored_water_runtime: Node
var _runtime_applied_snapshot: Resource
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
	_layer.add_child(live_editor)
	_editor = live_editor

	# The live editor is now inside the tree, so its transactional session has been
	# initialized by _ready(). Keep a private copy of the last state actually sent
	# to the runtime; ApplyScope is reset by session.apply() before `applied` emits.
	var session_value: Variant = live_editor.get("_session")
	if session_value is WorldAuthoringSession:
		var session: WorldAuthoringSession = session_value as WorldAuthoringSession
		if session.applied_system != null:
			_runtime_applied_snapshot = session.applied_system.duplicate(true)
		if not session.changed.is_connected(_on_authoring_session_changed):
			session.changed.connect(_on_authoring_session_changed)

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
	set_process(false)

func _on_authoring_session_changed(dirty_state: bool, apply_scope: int) -> void:
	# apply() emits changed(false, NONE) immediately before its applied signal.
	# Preserve the last dirty scope until _on_runtime_apply_requested consumes it.
	if dirty_state:
		_pending_apply_scope = apply_scope

func _resolve_authored_water_runtime_script() -> Script:
	# Keep the path constants themselves as compile-time String expressions. The
	# candidate container is created here at runtime because PackedStringArray(...)
	# is not a valid GDScript constant expression in Godot 4.7.1.
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
	if int(body.get(&"body_type")) == 0:
		_set_editor_status("Stars are editable in the system definition, but the current terrain runtime previews terrestrial bodies only.")
		_runtime_applied_snapshot = system.duplicate(true)
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

	if bool(plan.get("sculpt", false)) or bool(plan.get("full_rebuild", false)):
		_apply_sculpt_state(terrain_profile)

	if bool(plan.get("full_rebuild", false)):
		_apply_full_rebuild(plan, body, atmosphere, generation, cfg)
		_runtime_applied_snapshot = system.duplicate(true)
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
		# Current authored-water materials are configured when their disposable mesh
		# is compiled. This is still local work and deliberately never PlanetBake.
		_mark_water_dirty()
		refreshed.append("water material/wave parameters")

	if bool(plan.get("ocean", false)):
		_refresh_ocean_runtime()
		refreshed.append("ocean runtime")

	if bool(plan.get("tiles", false)):
		# Fallback for a future TILES edit the planner does not know yet. Refresh only
		# disposable local mirrors; never regenerate PlanetFields.
		_mark_biome_dirty()
		_mark_water_dirty()
		refreshed.append("local authored tiles")

	if bool(plan.get("graph", false)):
		refreshed.append("shader graph staging")
	if bool(plan.get("sculpt", false)):
		refreshed.append("sparse sculpt deltas")

	_runtime_applied_snapshot = system.duplicate(true)
	if refreshed.is_empty():
		_set_editor_status("Applied %s metadata only — no terrain, heightmap or biome rebuild required." % String(body.get(&"display_name")))
	else:
		_set_editor_status("Applied %s without PlanetBake — refreshed: %s." % [
			String(body.get(&"display_name")), ", ".join(refreshed)])

func _apply_full_rebuild(plan: Dictionary, body: Resource, atmosphere: Resource,
		generation: Resource, cfg: Resource) -> void:
	if bool(_main.get("_rebaking")):
		_set_editor_status("A generator rebuild is already running; this Apply was not started.")
		return
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
	# Planet.configure() only replaces runtime configuration; it does not invoke
	# PlanetBake or Planet.adopt(). Rebuild static GPU topology/material bindings
	# against the already resident PlanetFields.
	var terrain_node: Node = _main.get("terrain") as Node
	if terrain_node != null and terrain_node.has_method("build_roots"):
		terrain_node.call("build_roots")
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