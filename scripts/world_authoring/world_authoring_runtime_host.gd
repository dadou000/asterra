class_name WorldAuthoringRuntimeHost
extends Node
## Installs Planet Studio over Main and owns the boundary between staged authoring
## resources and the production runtime. BLANK terrain is an analytic backend: it
## never invokes PlanetBake and owns no generated height/material map.
##
## Celestial preview invariant: Frames always remains one stable system/world frame.
## Selecting a moon/planet changes only the camera interest point. The production
## terrain/ocean/contact stack may represent one root terrestrial body at system
## origin; orbital bodies stay lightweight until that stack is body-centre-aware.

# Main.tscn instantiates the editor through this host rather than PlanetStudio.tscn.
# Keep the live game on the public Phase 46 entry point so its Terrain tab matches
# the simplified no-code authoring UI instead of the retired shader composer.
const LIVE_EDITOR_SCRIPT := preload("res://scripts/world_authoring/world_authoring_editor_live_phase46.gd")
const BIOME_PREVIEW_SCRIPT := preload("res://scripts/world_authoring/biome_authoring_preview.gd")
const CELESTIAL_PREVIEW_SCRIPT := preload("res://scripts/world_authoring/celestial_body_preview_runtime.gd")
const ORBITAL_MOTION_SCRIPT := preload("res://scripts/world_authoring/orbital_motion_runtime.gd")
const APPLY_PLANNER_SCRIPT := preload("res://scripts/world_authoring/world_authoring_apply_planner.gd")
const TERRAIN_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const GEN_CONFIG_SCRIPT := preload("res://scripts/gen/gen_config.gd")
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
var _orbital_motion: Node
var _authoring_session: WorldAuthoringSession
var _preview_player: Node
var _runtime_applied_snapshot: Resource
var _detailed_runtime_body_id: String = ""
var _preview_body_id: String = ""
var _selected_center_world: Vec3D = Vec3D.new()
var _selected_radius_m: float = 1.0
var _selected_uses_detailed_surface: bool = false
var _preview_sync_pending: bool = false
var _preview_focus_pending: bool = false
var _terrestrial_runtime_visible: bool = true
var _pending_apply_scope: int = 0
var _opened: bool = false

func _ready() -> void:
	process_priority = 200
	_main = get_parent()
	if _launch_mode() != "planet_studio":
		set_process(false)
		return
	set_process(true)

func _process(_delta: float) -> void:
	if _main == null:
		return
	if not _opened:
		var player: Node = _main.get("player") as Node
		if player != null:
			_open_live_editor(player)
		return
	_sync_selected_preview_environment()

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
			_detailed_runtime_body_id = String(session.applied_system.get(&"active_body_id"))
			# Recover from a bad earlier Apply that pointed the single resident
			# terrain runtime at a moon / sub-body: snap it back to the primary.
			if not _is_primary_terrestrial(session.applied_system,
					session.applied_system.call("find_body", _detailed_runtime_body_id)):
				_detailed_runtime_body_id = _primary_terrestrial_id(session.applied_system)
		if not session.changed.is_connected(_on_authoring_session_changed):
			session.changed.connect(_on_authoring_session_changed)

		var celestial_preview: Node3D = CELESTIAL_PREVIEW_SCRIPT.new() as Node3D
		if celestial_preview != null:
			celestial_preview.name = "PlanetStudioCelestialSystemPreview"
			_main.add_child(celestial_preview)
			_celestial_preview = celestial_preview

		var orbital_motion: Node = ORBITAL_MOTION_SCRIPT.new()
		orbital_motion.name = "PlanetStudioOrbitalMotion"
		orbital_motion.call("bind", session, _main)
		add_child(orbital_motion)
		_orbital_motion = orbital_motion
		# Seed the sim clock from the persisted system epoch (dormant until played).
		var staged: Resource = session.staged_system as Resource
		if staged != null:
			Frames.system_time_s = float(staged.get(&"sim_start_epoch_s"))
			Frames.time_scale = maxf(float(staged.get(&"sim_time_scale")), 0.0)
			Frames.playing = false

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

func _on_authoring_session_changed(dirty_state: bool, apply_scope: int) -> void:
	if dirty_state:
		_pending_apply_scope = apply_scope
	var active_id: String = _active_staged_body_id()
	_schedule_active_body_preview(active_id != _preview_body_id)

func _on_body_focus_requested(_body_id: String) -> void:
	_schedule_active_body_preview(true)

func _on_planet_world_ready(_fields: PlanetFields) -> void:
	# Main finishes adopting generated fields in the same frame. Defer one turn so
	# preview visibility switches only after the runtime is coherent again.
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
	_selected_radius_m = maxf(float(body.get(&"radius_m")), 1.0)

	var rebaking: bool = bool(_main.get("_rebaking"))
	var detailed_available: bool = _detailed_runtime_is_root_usable(
		_authoring_session.staged_system) and not rebaking
	_selected_uses_detailed_surface = detailed_available \
		and body_id == _detailed_runtime_body_id

	# A body-centred detailed renderer is only exposed while its own body is the
	# selected interest frame. Otherwise every body uses stable lightweight meshes;
	# this prevents water/biome/cloud/terrain subsystems from being rebound to a moon
	# while still spatially centred on the root planet.
	_set_terrestrial_runtime_visible(_selected_uses_detailed_surface)
	if _celestial_preview != null:
		var hidden_detailed_id: String = _detailed_runtime_body_id \
			if _selected_uses_detailed_surface else ""
		_celestial_preview.call("show_system", _authoring_session.staged_system,
			body_id, hidden_detailed_id)
		_selected_center_world = _celestial_preview.call("selected_center_world") as Vec3D
	if _orbital_motion != null:
		_orbital_motion.call("set_anchor",
			_detailed_runtime_body_id if detailed_available else body_id)
	if _selected_center_world == null:
		_selected_center_world = Vec3D.new()

	if _editor != null and _editor.has_method("set_camera_interest"):
		_editor.call("set_camera_interest", _selected_center_world,
			_selected_radius_m, _selected_uses_detailed_surface)
	_apply_preview_atmosphere(body)
	_sync_depth_cloud_preview()

	var should_focus: bool = _preview_focus_pending or body_changed
	_preview_focus_pending = false
	if should_focus:
		_focus_camera_on_body(body)
		if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
			_set_editor_status("Focused %s — fixed system frame; stellar preview and orbital companions remain stable." % String(body.get(&"display_name")))
		elif _selected_uses_detailed_surface:
			_set_editor_status("Focused %s — detailed terrain owns this root body; companions stay at absolute orbital positions." % String(body.get(&"display_name")))
		else:
			_set_editor_status("Focused %s — lightweight orbital preview in the fixed system frame. No other planet runtime is re-centred." % String(body.get(&"display_name")))

func _detailed_runtime_is_root_usable(system: Resource) -> bool:
	if system == null or _detailed_runtime_body_id.is_empty():
		return false
	var detailed: Resource = system.call("find_body", _detailed_runtime_body_id) as Resource
	if detailed == null:
		return false
	# The single resident terrain/contact/ocean stack is centred on ONE body, which
	# OrbitalMotionRuntime keeps at the Frames origin. That body may ORBIT the
	# system's root star (a planet around Helion) -- its position never reaches the
	# terrain stack -- but it cannot sit under another planet/moon, which would need
	# a second detailed runtime that does not exist.
	return _is_primary_terrestrial(system, detailed)

## Can `body` be (re)bound as the single resident detailed terrain runtime?
## Only the body that already owns it (re-applying rebakes it in place), or -- when
## there is no valid resident body -- a top-level terrestrial body. Never STEALS a
## still-valid resident runtime, which would rebake it with the wrong config and
## break both bodies' previews.
func _body_can_own_detailed_runtime(body: Resource) -> bool:
	if body == null or int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		return false
	if String(body.get(&"body_id")) == _detailed_runtime_body_id:
		return true
	var system: Resource = _authoring_session.staged_system as Resource \
		if _authoring_session != null else null
	# The Bodies pool owns a per-body detailed stack: swapping to `body` bakes it
	# cache-first (instant on a revisit) and drops the previous owner to a cached
	# FAR slot, so a *primary* terrestrial body may take the detailed runtime over
	# from another one without corrupting either -- routed via _adopt_pool_detailed_runtime.
	if _pool_hosts_body(body):
		return _is_primary_terrestrial(system, body)
	# No pool slot (single-planet project): never steal a still-valid resident,
	# which would rebake it with the wrong config and break both bodies' previews.
	if system != null and system.call("find_body", _detailed_runtime_body_id) != null:
		return false
	return _is_primary_terrestrial(system, body)

## A "primary" terrestrial body: parentless, or whose only parent is the system's
## parentless root STAR.
func _is_primary_terrestrial(system: Resource, body: Resource) -> bool:
	if body == null or int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		return false
	var parent_id: String = String(body.get(&"parent_body_id"))
	if parent_id.is_empty():
		return true
	if system == null:
		return false
	var parent: Resource = system.call("find_body", parent_id) as Resource
	return parent != null and int(parent.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR \
		and String(parent.get(&"parent_body_id")).is_empty()

func _primary_terrestrial_id(system: Resource) -> String:
	if system == null:
		return ""
	for body_value: Variant in system.get(&"bodies"):
		var body: Resource = body_value as Resource
		if _is_primary_terrestrial(system, body):
			return String(body.get(&"body_id"))
	return ""

func _focus_camera_on_body(body: Resource) -> void:
	if _preview_player == null or body == null:
		return
	var camera: Camera3D = _preview_player.get("camera") as Camera3D
	if camera == null:
		return
	var radius_m: float = maxf(float(body.get(&"radius_m")), 1.0)
	var selected_visual_radius: float = radius_m * BODY_FRAME_SURFACE_MARGIN
	var frame_radius: float = selected_visual_radius
	if _celestial_preview != null:
		selected_visual_radius = maxf(float(_celestial_preview.call("visual_radius_m")), radius_m)
		frame_radius = maxf(selected_visual_radius,
			float(_celestial_preview.call("family_frame_radius_m")))

	var frame_distance: float = float(CELESTIAL_PREVIEW_SCRIPT.frame_distance_for_radius(
		frame_radius, camera.fov, BODY_FRAME_MARGIN))
	var radial_axis := Vector3(1.0, 0.18, 0.32).normalized()
	var current_world: Vec3D = _preview_player.get("world_pos") as Vec3D
	if current_world != null:
		var current_radial: Vec3D = current_world.sub(_selected_center_world)
		if current_radial.length_sq() > 1.0:
			radial_axis = current_radial.normalized().to_v3()
	# The staged Keplerian reference plane is XZ. Looking down Y keeps nearby
	# parent/moon pairs separated on screen when family framing is requested.
	if frame_radius > selected_visual_radius * FAMILY_FRAME_TRIGGER:
		radial_axis = Vector3.UP
	var next_world: Vec3D = _selected_center_world.add(
		Vec3D.from_v3(radial_axis).mul(frame_distance))
	_preview_player.set("world_pos", next_world)
	_preview_player.set("pitch", -PI * 0.5)
	Frames.rebase(next_world)
	if _editor != null and _editor.has_method("_sync_interest_camera_transform"):
		_editor.call("_sync_interest_camera_transform")
	camera.far = maxf(camera.far, frame_distance + frame_radius * 1.15)
	_preview_player.emit_signal("moved", next_world)

func _sync_selected_preview_environment() -> void:
	if _preview_player == null or _main == null:
		return
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material == null:
		return
	var world_pos: Vec3D = _preview_player.get("world_pos") as Vec3D
	if world_pos == null:
		return
	var local: Vec3D = world_pos.sub(_selected_center_world)
	var up: Vector3 = local.normalized().to_v3() if local.length_sq() > 1.0 \
		else Vector3.UP
	var height_m: float = local.length() - _selected_radius_m
	# main.gd still publishes its root-planet values earlier in the frame. Planet
	# Studio runs later and replaces only these genuinely camera-dependent values.
	sky_material.set_shader_parameter("u_up", up)
	sky_material.set_shader_parameter("u_camera_height", height_m)

func _apply_preview_atmosphere(body: Resource) -> void:
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material == null or body == null:
		return
	if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		# The star is explicit geometry. Keep the sky from inventing a second giant
		# planet limb/atmosphere around the camera while inspecting it.
		sky_material.set_shader_parameter("u_planet_radius", 1.0)
		sky_material.set_shader_parameter("u_atmosphere_radius", 2.0)
		sky_material.set_shader_parameter("u_rayleigh_coeff", Vector3.ZERO)
		sky_material.set_shader_parameter("u_mie_coeff", 0.0)
		sky_material.set_shader_parameter("u_ozone_coeff", Vector3.ZERO)
		sky_material.set_shader_parameter("u_cloud_enabled", 0.0)
		return

	var profile: Resource = body.get(&"planet_profile") as Resource
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource if profile != null else null
	var radius: float = maxf(float(body.get(&"radius_m")), 1.0)
	var atmosphere_height: float = 1.0
	if atmosphere != null:
		atmosphere_height = maxf(float(atmosphere.get(&"atmosphere_height_m")), 1.0)
	sky_material.set_shader_parameter("u_planet_radius", radius)
	sky_material.set_shader_parameter("u_atmosphere_radius", radius + atmosphere_height)
	if atmosphere == null:
		sky_material.set_shader_parameter("u_rayleigh_coeff", Vector3.ZERO)
		sky_material.set_shader_parameter("u_mie_coeff", 0.0)
		sky_material.set_shader_parameter("u_ozone_coeff", Vector3.ZERO)
		sky_material.set_shader_parameter("u_cloud_enabled", 0.0)
		return
	var enabled: bool = bool(atmosphere.get(&"enabled"))
	var enabled_scale: float = 1.0 if enabled else 0.0
	sky_material.set_shader_parameter("u_rayleigh_coeff", BASE_RAYLEIGH_COEFF
		* maxf(float(atmosphere.get(&"rayleigh_strength")), 0.0) * enabled_scale)
	sky_material.set_shader_parameter("u_mie_coeff", BASE_MIE_COEFF
		* maxf(float(atmosphere.get(&"mie_strength")), 0.0) * enabled_scale)
	sky_material.set_shader_parameter("u_ozone_coeff", BASE_OZONE_COEFF
		* maxf(float(atmosphere.get(&"ozone_strength")), 0.0) * enabled_scale)
	sky_material.set_shader_parameter("u_cloud_coverage",
		clampf(float(atmosphere.get(&"cloud_coverage")), 0.0, 1.0))
	sky_material.set_shader_parameter("u_cloud_density",
		maxf(float(atmosphere.get(&"cloud_density")), 0.0))
	var cloud_base: float = maxf(float(atmosphere.get(&"cloud_altitude_m")), 1.0)
	sky_material.set_shader_parameter("u_cloud_base", cloud_base)
	sky_material.set_shader_parameter("u_cloud_top", cloud_base + DEFAULT_CLOUD_THICKNESS_M)
	sky_material.set_shader_parameter("u_cloud_enabled", enabled_scale)

func _selected_cloud_enabled() -> bool:
	if _authoring_session == null:
		return false
	var body: Resource = _authoring_session.active_body() as Resource
	if body == null or int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		return false
	var profile: Resource = body.get(&"planet_profile") as Resource
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource if profile != null else null
	return atmosphere != null and bool(atmosphere.get(&"enabled"))

func _sync_depth_cloud_preview() -> void:
	var clouds: Node = get_node_or_null("/root/VolumetricClouds")
	var effect: Object = clouds.get("_depth_effect") as Object if clouds != null else null
	var depth_ready: bool = false
	if effect != null and effect.has_method("is_ready"):
		depth_ready = bool(effect.call("is_ready"))
	var use_depth: bool = _selected_uses_detailed_surface and depth_ready
	if effect != null:
		effect.set("enabled", use_depth)
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material != null:
		# The depth compositor is centred on the production root body. For staged
		# orbital bodies use the body-local sky fallback rather than a spatially wrong
		# cloud volume. Detailed root view keeps one renderer only, avoiding doubles.
		sky_material.set_shader_parameter("u_cloud_enabled",
			0.0 if use_depth else (1.0 if _selected_cloud_enabled() else 0.0))

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
	if _biome_preview != null:
		_biome_preview.set_process(value)
		if value and _biome_preview.has_method("mark_dirty"):
			_biome_preview.call("mark_dirty")
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
	var body_id: String = String(body.get(&"body_id"))
	if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		_set_editor_status("Applied stellar authoring state for %s — no terrestrial terrain rebuild." % String(body.get(&"display_name")))
		_runtime_applied_snapshot = system.duplicate(true)
		_schedule_active_body_preview(false)
		return

	# The current production terrain/contact/ocean stack is centred at system origin.
	# Applying an orbital moon/planet into that singleton used to move radius/config
	# without moving all dependent coordinate systems, producing the visible breakup.
	# Preserve the authored snapshot but leave that orbital body on the lightweight
	# preview until a body-centre-aware detailed runtime exists.
	if not _body_can_own_detailed_runtime(body):
		_runtime_applied_snapshot = system.duplicate(true)
		_set_editor_status("Applied %s authoring data without PlanetBake — orbital bodies stay on the coherent lightweight preview until detailed terrain supports non-zero body centres." % String(body.get(&"display_name")))
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
		_detailed_runtime_body_id = body_id
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
		# A pool-hosted (non-primary) body swaps the single resident detailed stack
		# through the Bodies pool: cache-first bake, exactly one body resident, an
		# instant return to any previously-baked body. The primary body keeps the
		# in-place rebake path below (its stack IS the resident autoload set).
		if _adopt_pool_detailed_runtime(body, true):
			_runtime_applied_snapshot = system.duplicate(true)
			_schedule_active_body_preview(true)
			return
		_detailed_runtime_body_id = body_id
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

## The Bodies pool autoload, resolved via the main loop so it works even when this
## host node is not (yet) inside the scene tree (some headless tests drive it
## detached). Absolute get_node paths would error from an out-of-tree node.
func _bodies_pool() -> Node:
	var loop := Engine.get_main_loop()
	if loop is SceneTree and (loop as SceneTree).root != null:
		return (loop as SceneTree).root.get_node_or_null(^"Bodies")
	return null

## True when the Bodies pool has a runtime slot for `body` (the seeded celestial
## system was populated -- Planet Studio + the game share one generator/seed).
func _pool_hosts_body(body: Resource) -> bool:
	if body == null:
		return false
	var bodies: Node = _bodies_pool()
	if bodies == null:
		return false
	return bodies.call(&"slot", StringName(String(body.get(&"body_id")))) != null

## A body-local observer position handed to Bodies.load_active. The editor's
## _focus_camera_on_body reframes the preview camera on the deferred flush, so the
## exact value only needs to be a sane point above the surface.
func _pool_swap_player_world(body: Resource) -> Vec3D:
	var r: float = maxf(float(body.get(&"radius_m")), 1.0)
	return Vec3D.new(0.0, r * 4.0, 0.0)

## Route the single resident detailed terrain/ocean/contact stack onto `body`
## through the Bodies pool. Exactly one body is ever baked/resident -- the active
## authoring target -- and a return to any body baked earlier this session (or
## cached on disk from a previous one) is instant (BodyRuntime.warm ->
## PlanetBake.bake(_, true) is cache-first). Returns true when the pool owns the
## swap; false (no pool slot) leaves the caller on its legacy in-place path.
##
## PARITY WITH THE GAME: the pool slot's `gen_config` was registered by the same
## `CelestialSystemGenerator.populate_pool()` the standalone game runs, so an
## unedited switch bakes byte-for-byte what the game bakes for that body. Only a
## deliberate generation Apply (`force_rebake`) pulls the authored edits into the
## slot's config and drops its cached fields so warm() re-bakes; PlanetBake's
## cache key covers those params, so an unedited revisit still hits the cache.
func _adopt_pool_detailed_runtime(body: Resource, force_rebake: bool) -> bool:
	if body == null or int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		return false
	var bodies: Node = _bodies_pool()
	if bodies == null:
		return false
	var body_id := StringName(String(body.get(&"body_id")))
	var rt: Object = bodies.call(&"slot", body_id)
	if rt == null:
		return false
	_detailed_runtime_body_id = String(body_id)
	if bodies.get(&"active") == rt:
		return true

	var is_primary: bool = bool(rt.get(&"is_primary"))
	var profile: Resource = body.get(&"planet_profile") as Resource
	var terrain_profile: Resource = profile.get(&"terrain") as Resource \
		if profile != null else null
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource \
		if profile != null else null

	if not is_primary:
		if _main != null and _main.get("_rebaking") == true:
			_set_editor_status("A generator rebuild is already running; this switch was not started.")
			return true
		# Only a deliberate generation Apply diverges this body from the shared
		# generator output; a plain switch leaves populate_pool's config intact.
		if force_rebake:
			var generation: Resource = terrain_profile.get(&"generation_profile") as Resource \
				if terrain_profile != null else null
			if generation == null:
				return false
			var gc: Resource = rt.get(&"gen_config") as Resource
			if gc == null:
				gc = GEN_CONFIG_SCRIPT.new()
				rt.set(&"gen_config", gc)
			generation.call("copy_to_resource", gc)
			# A gas giant bakes its solid core, not the cloud tops (parity with the
			# game's gen_config_for). surface_reference_radius_m() == radius_m else.
			var bake_radius: float = float(body.get(&"radius_m"))
			if body.has_method("surface_reference_radius_m"):
				bake_radius = float(body.call("surface_reference_radius_m"))
			gc.set(&"planet_radius", maxf(1.0, bake_radius))
			gc.set(&"axial_tilt_deg", float(body.get(&"axial_tilt_deg")))
			if atmosphere != null:
				gc.set(&"atmosphere_height",
					maxf(1.0, float(atmosphere.get(&"atmosphere_height_m"))))
			gc.set(&"system_seed", 0)
			rt.set(&"fields", null)
		var slot_cfg: Resource = rt.get(&"gen_config") as Resource
		if slot_cfg != null:
			# The bake config's radius is the SURFACE datum (a gas giant's core);
			# radius_m stays the reference / cloud-top radius from register_body.
			rt.set(&"surface_radius_m", float(slot_cfg.get(&"planet_radius")))
			if float(body.get(&"radius_m")) > 1.0:
				rt.set(&"radius_m", float(body.get(&"radius_m")))
		Planet.call("set_blank_mode", false)
		_set_ground_generated_height_enabled(true)

	var baking: bool = (not is_primary) and (rt.get(&"fields") == null)
	if baking:
		_set_editor_status("Baking %s — one-time; a later return to it is instant." \
			% String(body.get(&"display_name")))

	# Re-place the preview observer onto the new body BEFORE the pool re-origins
	# the canonical frame, so main._process / _on_active_body_changed never read a
	# stale (previous-body, or star-framing ~1 AU) world_pos against the new
	# radius datum -- that mismatch is what spiked the on-screen altitude and
	# collapsed the free-fly camera speed when switching bodies. The deferred
	# _flush_active_body_preview -> _focus_camera_on_body then frames it exactly.
	var swap_pos: Vec3D = _pool_swap_player_world(body)
	var observer: Node = _preview_player if _preview_player != null \
		else _main.get("player") as Node
	if observer != null:
		observer.set("world_pos", swap_pos)

	# Cache-first activation: warm() loads the cached bake (instant) or bakes once
	# and caches it; only this one body becomes resident. load_active emits
	# active_changed -> main._on_active_body_changed, the SAME post-swap refresh
	# (build_roots / orbit textures / ocean / editor) the game runs on arrival.
	bodies.call(&"load_active", body_id, swap_pos)
	_detailed_runtime_body_id = String(body_id)
	if observer != null:
		observer.set("world_pos", swap_pos)
		if observer.has_signal("moved"):
			observer.emit_signal("moved", swap_pos)

	# Frames diurnal/tilt datum for the now-resident body (Planet.adopt already set
	# the radius). The game gets these from OrbitalMotionRuntime per body.
	if Planet.cfg != null:
		_sync_frames(body, Planet.cfg)
	if terrain_profile != null:
		_apply_sculpt_state(terrain_profile)

	if not baking:
		_set_editor_status("Activated %s — resident detailed terrain (cached, no rebake)." \
			% String(body.get(&"display_name")))
	return true

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
	# Anchor identity + diurnal phase. helion_dir / helion_distance_m stay owned by
	# OrbitalMotionRuntime, which overwrites these every frame anyway; this just
	# keeps a coherent value between an Apply and the next runtime tick.
	Frames.anchor_body_id = String(body.get(&"body_id"))
	Frames._rotation_phase0_deg = float(body.get(&"rotation_phase_at_epoch_deg"))

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
