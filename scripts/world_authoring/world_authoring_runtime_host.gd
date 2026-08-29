class_name WorldAuthoringRuntimeHost
extends Node
## Installs Planet Studio over Main when launched in planet_studio mode and owns
## the deliberate boundary between staged authoring resources and the production
## one-planet runtime. Apply can rebuild the currently selected terrestrial body;
## biome/water/graph layers remain staged until their runtime rasterizers compile.

const LIVE_EDITOR_SCRIPT := preload("res://scripts/world_authoring/world_authoring_editor_live.gd")

var _main: Node
var _layer: CanvasLayer
var _editor: Control
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
	var player := _main.get("player") as Node
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
	set_process(false)

func _set_existing_ui_visible(visible: bool) -> void:
	for property_name: StringName in [&"hud", &"map", &"debug_menu", &"coastline_profile_editor"]:
		var node := _main.get(property_name) as CanvasItem
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
	var cfg := _main.get("cfg") as Resource
	if generation == null or cfg == null:
		_set_editor_status("Apply rejected: generation/runtime configuration is unavailable.")
		return
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
	if bool(_main.get("_rebaking")):
		_set_editor_status("A terrain rebuild is already running.")
		return
	_main.call("_on_rebake_requested")
	var terrain_profile: Resource = profile.get(&"terrain") as Resource
	var biome_count := 0
	var displacement_count := 0
	var material_count := 0
	if terrain_profile != null:
		biome_count = (terrain_profile.get(&"biome_override_layers") as Array).size()
		displacement_count = (terrain_profile.get(&"displacement_slots") as Array).size()
		material_count = (terrain_profile.get(&"material_slots") as Array).size()
	var water: Resource = profile.get(&"water") as Resource
	var water_count := (water.get(&"authored_features") as Array).size() if water != null else 0
	_set_editor_status("Applying %s to live runtime — generator/planet/atmosphere rebuild started. Staged runtime layers: %d biome, %d displacement, %d material, %d water feature(s)." % [String(body.get(&"display_name")), biome_count, displacement_count, material_count, water_count])

func _update_environment_radius(cfg: Resource) -> void:
	var sky_material := _main.get("sky_mat") as ShaderMaterial
	if sky_material == null:
		return
	var radius := float(cfg.get(&"planet_radius"))
	var atmosphere_height := float(cfg.get(&"atmosphere_height"))
	sky_material.set_shader_parameter("u_planet_radius", radius)
	sky_material.set_shader_parameter("u_atmosphere_radius", radius + atmosphere_height)

func _set_editor_status(message: String) -> void:
	if _editor != null and _editor.has_method("_set_status"):
		_editor.call("_set_status", message)
