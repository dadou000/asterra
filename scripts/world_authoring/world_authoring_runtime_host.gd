class_name WorldAuthoringRuntimeHost
extends Node
## Installs Planet Studio over Main when launched in planet_studio mode and owns
## the deliberate boundary between staged authoring resources and the production
## one-planet runtime. Apply can rebuild the currently selected terrestrial body;
## sparse sculpt data is synchronized immediately, biome paint is mirrored into a
## high-resolution non-destructive near-field preview, and authored lakes/rivers
## compile to disposable terrain-aware local water meshes. Shader-graph runtime
## compilation remains staged behind its dedicated pass.

const LIVE_EDITOR_SCRIPT := preload("res://scripts/world_authoring/world_authoring_editor_live_phase12.gd")
const BIOME_PREVIEW_SCRIPT := preload("res://scripts/world_authoring/biome_authoring_preview.gd")
const AUTHORED_WATER_RUNTIME_SCRIPT := preload("res://scripts/world_authoring/authored_water_runtime_spatial.gd")

var _main: Node
var _layer: CanvasLayer
var _editor: Control
var _biome_preview: Node
var _authored_water_runtime: Node
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
	# initialized by _ready(). Bind all disposable runtime mirrors to that exact
	# staged session instead of keeping parallel authoring state.
	var session_value: Variant = live_editor.get("_session")
	if session_value is WorldAuthoringSession:
		var session: WorldAuthoringSession = session_value as WorldAuthoringSession
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

		var authored_water: Node = AUTHORED_WATER_RUNTIME_SCRIPT.new()
		authored_water.name = "PlanetStudioAuthoredWaterRuntime"
		authored_water.call("bind", session, _main)
		add_child(authored_water)
		authored_water.add_to_group(&"authored_water_query")
		_authored_water_runtime = authored_water
		if live_editor.has_signal("water_preview_changed"):
			live_editor.connect("water_preview_changed", Callable(authored_water, "mark_dirty"))
	set_process(false)

func _set_existing_ui_visible(visible: bool) -> void:
	for property_name: StringName in [&"hud", &"map", &"debug_menu", &"coastline_profile_editor"]:
		var node: CanvasItem = _main.get(property_name) as CanvasItem
		if node != null:
			node.visible = visible

func _on_runtime_apply_requested(system: Resource) -> void:
	if _main == null or system == null:
		return
	var body: Resource = system.call("active_body") as Resource
	if body == null:
		_set_editor_status("Apply rejected: no active celestial body.")
		return
	if int(body.get(&"body_type")) == 0:
		_set_editor_status("Stars are editable in the system definition, but the current terrain runtime previews terrestrial bodies only.")
		return
	var profile: Resource = body.get(&"planet_profile") as Resource
	if profile == null:
		_set_editor_status("Apply rejected: active body has no terrestrial profile.")
		return
	var terrain: Resource = profile.get(&"terrain") as Resource
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource
	if terrain == null:
		_set_editor_status("Apply rejected: active body has no terrain profile.")
		return
	var generation: Resource = terrain.get(&"generation_profile") as Resource
	var cfg: Resource = _main.get("cfg") as Resource
	if generation == null or cfg == null:
		_set_editor_status("Apply rejected: generation/runtime configuration is unavailable.")
		return

	# Sculpt deltas are already authored in the runtime while painting, but Apply is
	# also the boundary used after preset loads/body changes. Re-adopt the staged
	# sparse state here so the production renderer/contact stack cannot retain data
	# from a previously selected body.
	if terrain.has_method("sculpt_delta_serialized"):
		var sculpt_value: Variant = terrain.call("sculpt_delta_serialized")
		if sculpt_value is Dictionary:
			Deltas.deserialize(sculpt_value as Dictionary)

	generation.call("copy_to_resource", cfg)
	cfg.set(&"planet_radius", maxf(1.0, float(body.get(&"radius_m"))))
	cfg.set(&"axial_tilt_deg", float(body.get(&"axial_tilt_deg")))
	if atmosphere != null:
		cfg.set(&"atmosphere_height", maxf(1.0, float(atmosphere.get(&"atmosphere_height_m"))))
	Frames.set_planet_radius(float(cfg.get(&"planet_radius")))
	Frames.axial_tilt_deg = float(body.get(&"axial_tilt_deg"))
	Frames.day_seconds = maxf(0.001, absf(float(body.get(&"sidereal_rotation_period_s"))))
	Planet.configure(cfg)
	_update_environment_radius(cfg)
	if _biome_preview != null and _biome_preview.has_method("mark_dirty"):
		_biome_preview.call("mark_dirty")
	if _authored_water_runtime != null and _authored_water_runtime.has_method("mark_dirty"):
		_authored_water_runtime.call("mark_dirty")
	if bool(_main.get("_rebaking")):
		_set_editor_status("A terrain rebuild is already running.")
		return
	_main.call("_on_rebake_requested")

	var terrain_profile: Resource = profile.get(&"terrain") as Resource
	var biome_count: int = 0
	var displacement_count: int = 0
	var material_count: int = 0
	var sculpt_tile_count: int = 0
	if terrain_profile != null:
		biome_count = (terrain_profile.get(&"biome_override_layers") as Array).size()
		displacement_count = (terrain_profile.get(&"displacement_slots") as Array).size()
		material_count = (terrain_profile.get(&"material_slots") as Array).size()
		if terrain_profile.has_method("sculpt_edited_tile_count"):
			sculpt_tile_count = int(terrain_profile.call("sculpt_edited_tile_count"))
	var water: Resource = profile.get(&"water") as Resource
	var water_count: int = (water.get(&"authored_features") as Array).size() if water != null else 0
	_set_editor_status("Applying %s to live runtime — generator/planet/atmosphere rebuild started. Runtime layers: %d sculpt tiles, %d biome, %d displacement, %d material, %d water feature(s)." % [String(body.get(&"display_name")), sculpt_tile_count, biome_count, displacement_count, material_count, water_count])

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
