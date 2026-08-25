extends Node
## Runtime test harness for the temporary boat/car/plane prototypes.
##
## F6 = boat, F7 = car, F8 = plane, F10 = return to player. The selected body
## becomes the observer by mirroring its canonical position into AsterraPlayer;
## this keeps the GPU terrain, collision streamer, weather nest, sky and existing
## HUD all centred on the thing being tested without adding a second observer API.

var _player: AsterraPlayer
var _active: WindVehicleTest
var _vehicles: Dictionary = {}
var _overlay: Label
var _scene_root: Node

func _ready() -> void:
	process_priority = -2
	set_process_unhandled_input(true)
	_build_overlay()

func _build_overlay() -> void:
	_overlay = Label.new()
	_overlay.visible = false
	_overlay.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_overlay.position = Vector2(-390, 16)
	_overlay.custom_minimum_size = Vector2(370, 150)
	_overlay.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_overlay.add_theme_font_size_override("font_size", 14)
	add_child(_overlay)

func _process(_dt: float) -> void:
	_find_player()
	if _active == null or not is_instance_valid(_active) or _player == null:
		if _overlay != null:
			_overlay.visible = false
		return

	# Existing systems already use AsterraPlayer as their observer. Mirroring only
	# the canonical coordinate keeps those systems working while the player input
	# and camera remain disabled during vehicle possession.
	_player.world_pos = _active.world_pos()
	_update_overlay()

func _find_player() -> void:
	if _player != null and is_instance_valid(_player):
		return
	var root := get_tree().current_scene
	if root == null:
		return
	_scene_root = root
	_player = _find_player_recursive(root)

func _find_player_recursive(node: Node) -> AsterraPlayer:
	if node is AsterraPlayer:
		return node as AsterraPlayer
	for child in node.get_children():
		var found := _find_player_recursive(child)
		if found != null:
			return found
	return null

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	match event.keycode:
		KEY_F6:
			_select(WindVehicleTest.Kind.BOAT)
			get_viewport().set_input_as_handled()
		KEY_F7:
			_select(WindVehicleTest.Kind.CAR)
			get_viewport().set_input_as_handled()
		KEY_F8:
			_select(WindVehicleTest.Kind.PLANE)
			get_viewport().set_input_as_handled()
		KEY_F10:
			_return_to_player()
			get_viewport().set_input_as_handled()

func _select(kind: WindVehicleTest.Kind) -> void:
	_find_player()
	if _player == null or not Planet.ready_state:
		return
	if _active != null and is_instance_valid(_active):
		_active.controlled = false
		_active.camera.current = false

	var body: WindVehicleTest = _vehicles.get(kind)
	if body == null or not is_instance_valid(body):
		body = WindVehicleTest.new()
		body.name = "Test%s" % _kind_name(kind).capitalize()
		var parent := _scene_root if _scene_root != null else get_tree().current_scene
		if parent == null:
			return
		parent.add_child(body)
		var spawn_dir := _player.up_dir()
		if kind == WindVehicleTest.Kind.BOAT:
			spawn_dir = _find_nearby_ocean(spawn_dir)
		body.configure(kind, spawn_dir)
		_vehicles[kind] = body

	_active = body
	_active.controlled = true
	_active.camera.current = true
	_player.camera.current = false
	_player.input_enabled = false
	_player.set_mouse_captured(false)
	_player.world_pos = _active.world_pos()
	_overlay.visible = true

func _return_to_player() -> void:
	if _active != null and is_instance_valid(_active):
		# Keep the player at the vehicle instead of snapping back to the old test site.
		if _player != null:
			_player.world_pos = _active.world_pos()
		_active.controlled = false
		_active.camera.current = false
	_active = null
	if _player != null and is_instance_valid(_player):
		_player.camera.current = true
		_player.input_enabled = true
		_player.vertical_speed = 0.0
		_player.set_mouse_captured(true)
	if _overlay != null:
		_overlay.visible = false

func _find_nearby_ocean(center: Vector3) -> Vector3:
	if TerrainContactSampler.height(center) < -2.0:
		return center
	var tangent: Array = CubeSphere.tangent_basis(center)
	var east: Vector3 = tangent[0]
	var north: Vector3 = tangent[1]
	var best: Vector3 = center
	var best_height: float = TerrainContactSampler.height(center)
	var radii: Array[float] = [2000.0, 5000.0, 10000.0, 20000.0, 40000.0, 80000.0]
	for radius_m in radii:
		for i in 24:
			var a: float = TAU * float(i) / 24.0
			var offset: Vector3 = east * cos(a) + north * sin(a)
			var angle: float = radius_m / float(Planet.cfg.planet_radius)
			var d: Vector3 = (center * cos(angle) + offset * sin(angle)).normalized()
			var h: float = TerrainContactSampler.height(d)
			if h < best_height:
				best_height = h
				best = d
			if h < -2.0:
				return d
	return best

func _update_overlay() -> void:
	if _active == null:
		return
	var speed_kmh: float = _active.linear_velocity.length() * 3.6
	var wind_speed: float = _active.last_wind.length()
	var extra: String = "W/S throttle · A/D steer" if _active.kind != WindVehicleTest.Kind.PLANE \
		else "W/S pitch · A/D roll · Space/Shift throttle"
	_overlay.text = "%s TEST   F10 return to player\n%s\nSpeed %.1f km/h   Wind %.1f m/s\nMSL %.1f m   AGL %.1f m\nApparent air %.1f m/s\nF6 boat · F7 car · F8 plane" % [
		_active.vehicle_name(), extra, speed_kmh, wind_speed,
		_active.altitude_msl(), _active.last_agl, _active.last_airspeed]

func _kind_name(kind: WindVehicleTest.Kind) -> String:
	match kind:
		WindVehicleTest.Kind.BOAT: return "boat"
		WindVehicleTest.Kind.CAR: return "car"
		_: return "plane"
