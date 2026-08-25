extends CanvasLayer
## Full-screen barycentric celestial-system camera.
##
## Press the `"` character to toggle it. The system view is rendered in its own
## SubViewport so astronomical coordinates never enter the main float32 world.
## Orbital positions are scaled linearly from the double-precision N-body state;
## body radii are deliberately exaggerated so Helion and Asterra remain visible.

const QUOTE_UNICODE := 34
const ORBIT_VIEW_RADIUS := 100.0
const MIN_CAMERA_DISTANCE := 38.0
const MAX_CAMERA_DISTANCE := 420.0
const CAMERA_DRAG_SENSITIVITY := 0.006

var _overlay: Control
var _viewport_container: SubViewportContainer
var _subviewport: SubViewport
var _scene_root: Node3D
var _camera: Camera3D
var _helion_mesh: MeshInstance3D
var _asterra_mesh: MeshInstance3D
var _asterra_axis: MeshInstance3D
var _stats: Label

var _position_scale := 1.0
var _camera_yaw := 0.62
var _camera_pitch := 0.44
var _camera_distance := 185.0
var _dragging := false

var _player: AsterraPlayer
var _restore_player_input := true
var _restore_mouse_capture := false
var _stats_accumulator := 0.0

func _ready() -> void:
	layer = 200
	process_mode = Node.PROCESS_MODE_ALWAYS
	_position_scale = ORBIT_VIEW_RADIUS / CelestialSystem.ASTERRA_SEMI_MAJOR_AXIS_M
	_build_ui()
	_build_3d_scene()
	visible = false
	set_process_input(true)
	set_process(true)

func _build_ui() -> void:
	_overlay = Control.new()
	_overlay.name = "CelestialCameraOverlay"
	_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_overlay.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_overlay)

	_viewport_container = SubViewportContainer.new()
	_viewport_container.name = "CelestialViewportContainer"
	_viewport_container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_overlay.add_child(_viewport_container)

	_subviewport = SubViewport.new()
	_subviewport.name = "CelestialViewport"
	_subviewport.disable_3d = false
	_subviewport.own_world_3d = true
	_subviewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_subviewport.size = Vector2i(1280, 720)
	_viewport_container.add_child(_subviewport)

	var info_panel := PanelContainer.new()
	info_panel.name = "InfoPanel"
	info_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	info_panel.offset_left = 18.0
	info_panel.offset_top = 18.0
	info_panel.offset_right = 438.0
	info_panel.offset_bottom = 250.0
	_overlay.add_child(info_panel)

	_stats = Label.new()
	_stats.name = "Stats"
	_stats.add_theme_font_size_override("font_size", 16)
	_stats.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_stats.text = "CELESTIAL BODY CAMERA\nWaiting for N-body state…"
	info_panel.add_child(_stats)

	var help := Label.new()
	help.name = "Help"
	help.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	help.offset_left = 18.0
	help.offset_top = -54.0
	help.offset_right = 760.0
	help.offset_bottom = -16.0
	help.add_theme_font_size_override("font_size", 15)
	help.text = "\" toggle/close   •   drag LMB to orbit camera   •   wheel to zoom   •   Esc close\nBody sizes exaggerated for visibility; barycentric positions and orbital distances are to scale."
	_overlay.add_child(help)

func _build_3d_scene() -> void:
	_scene_root = Node3D.new()
	_scene_root.name = "CelestialScene"
	_subviewport.add_child(_scene_root)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0015, 0.0025, 0.0075)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.08, 0.10, 0.18)
	environment.ambient_light_energy = 0.45
	world_environment.environment = environment
	_scene_root.add_child(world_environment)

	_camera = Camera3D.new()
	_camera.name = "SystemCamera"
	_camera.near = 0.05
	_camera.far = 1200.0
	_camera.fov = 52.0
	_scene_root.add_child(_camera)
	_camera.current = true

	var light := DirectionalLight3D.new()
	light.name = "SystemLight"
	light.light_energy = 1.35
	light.rotation_degrees = Vector3(-38.0, -32.0, 0.0)
	light.shadow_enabled = false
	_scene_root.add_child(light)

	_helion_mesh = MeshInstance3D.new()
	_helion_mesh.name = "Helion"
	var helion_sphere := SphereMesh.new()
	helion_sphere.radius = 1.85
	helion_sphere.height = 3.70
	helion_sphere.radial_segments = 32
	helion_sphere.rings = 16
	_helion_mesh.mesh = helion_sphere
	var helion_material := StandardMaterial3D.new()
	helion_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	helion_material.albedo_color = Color(1.0, 0.969, 0.945)
	helion_material.emission_enabled = true
	helion_material.emission = Color(1.0, 0.949, 0.910)
	helion_material.emission_energy_multiplier = 2.2
	_helion_mesh.material_override = helion_material
	_scene_root.add_child(_helion_mesh)

	_asterra_mesh = MeshInstance3D.new()
	_asterra_mesh.name = "Asterra"
	var asterra_sphere := SphereMesh.new()
	asterra_sphere.radius = 0.78
	asterra_sphere.height = 1.56
	asterra_sphere.radial_segments = 32
	asterra_sphere.rings = 16
	_asterra_mesh.mesh = asterra_sphere
	var asterra_material := StandardMaterial3D.new()
	asterra_material.albedo_color = Color(0.31, 0.47, 0.76)
	asterra_material.roughness = 0.72
	asterra_material.metallic = 0.02
	_asterra_mesh.material_override = asterra_material
	_scene_root.add_child(_asterra_mesh)

	_scene_root.add_child(_make_orbit_path())
	_asterra_axis = _make_asterra_axis()
	_scene_root.add_child(_asterra_axis)
	_update_camera_transform()
	_update_body_transforms()

func _make_orbit_path() -> MeshInstance3D:
	var orbit_mesh := ImmediateMesh.new()
	var orbit_material := StandardMaterial3D.new()
	orbit_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	orbit_material.albedo_color = Color(0.42, 0.50, 0.68)

	var total_mass := CelestialSystem.HELION_MASS_KG + CelestialSystem.ASTERRA_MASS_KG
	var asterra_fraction := CelestialSystem.HELION_MASS_KG / total_mass
	var a := CelestialSystem.ASTERRA_SEMI_MAJOR_AXIS_M
	var e := CelestialSystem.ASTERRA_ECCENTRICITY

	orbit_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP, orbit_material)
	for i in range(257):
		var nu := TAU * float(i) / 256.0
		var radius := a * (1.0 - e * e) / (1.0 + e * cos(nu))
		var barycentric_radius := radius * asterra_fraction * _position_scale
		orbit_mesh.surface_add_vertex(Vector3(
			barycentric_radius * cos(nu),
			0.0,
			-barycentric_radius * sin(nu)
		))
	orbit_mesh.surface_end()

	var instance := MeshInstance3D.new()
	instance.name = "AsterraOrbit"
	instance.mesh = orbit_mesh
	return instance

func _make_asterra_axis() -> MeshInstance3D:
	var tilt := deg_to_rad(CelestialSystem.ASTERRA_AXIAL_TILT_DEG)
	var axis := Vector3(0.0, cos(tilt), sin(tilt)).normalized()
	var axis_mesh := ImmediateMesh.new()
	var axis_material := StandardMaterial3D.new()
	axis_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	axis_material.albedo_color = Color(0.78, 0.63, 0.86)
	axis_mesh.surface_begin(Mesh.PRIMITIVE_LINES, axis_material)
	axis_mesh.surface_add_vertex(-axis * 2.15)
	axis_mesh.surface_add_vertex(axis * 2.15)
	axis_mesh.surface_end()

	var instance := MeshInstance3D.new()
	instance.name = "AsterraSpinAxis"
	instance.mesh = axis_mesh
	return instance

func _process(delta: float) -> void:
	if not visible:
		return
	_stats_accumulator += delta
	_update_body_transforms()
	_update_viewport_size()
	if _stats_accumulator >= 0.10:
		_stats_accumulator = 0.0
		_update_stats()

func _update_viewport_size() -> void:
	if _overlay == null or _subviewport == null:
		return
	var size := _overlay.size
	if size.x < 1.0 or size.y < 1.0:
		return
	var wanted := Vector2i(maxi(1, int(size.x)), maxi(1, int(size.y)))
	if _subviewport.size != wanted:
		_subviewport.size = wanted

func _update_body_transforms() -> void:
	if _helion_mesh == null or _asterra_mesh == null:
		return
	var helion := CelestialSystem.body_state("Helion")
	var asterra := CelestialSystem.body_state("Asterra")
	if helion.is_empty() or asterra.is_empty():
		return

	_helion_mesh.position = _scaled_position(helion["position_m"])
	_asterra_mesh.position = _scaled_position(asterra["position_m"])
	_asterra_axis.position = _asterra_mesh.position

func _scaled_position(p: Vec3D) -> Vector3:
	return Vector3(
		float(p.x * _position_scale),
		float(p.y * _position_scale),
		float(p.z * _position_scale)
	)

func _update_stats() -> void:
	var helion := CelestialSystem.body_state("Helion")
	var asterra := CelestialSystem.body_state("Asterra")
	if helion.is_empty() or asterra.is_empty():
		_stats.text = "CELESTIAL BODY CAMERA\nN-body state unavailable"
		return

	var distance_au := CelestialSystem.asterra_helion_distance_m() / CelestialSystem.AU_M
	var irradiance := CelestialSystem.asterra_irradiance_w_m2()
	var relative_velocity: Vec3D = (asterra["velocity_mps"] as Vec3D).sub(helion["velocity_mps"] as Vec3D)
	var relative_speed_km_s := relative_velocity.length() / 1000.0
	var sim_days := CelestialSystem.simulation_seconds / 86400.0
	var angular_diameter := CelestialSystem.helion_angular_diameter_deg()
	var declination := CelestialSystem.helion_declination_deg()

	_stats.text = (
		"CELESTIAL BODY CAMERA — N-BODY BARYCENTRIC\n"
		+ "Simulation time     %10.3f Earth d\n" % sim_days
		+ "Time scale          %10.1f×\n" % CelestialSystem.time_scale
		+ "Helion distance     %10.6f AU\n" % distance_au
		+ "Relative speed      %10.3f km/s\n" % relative_speed_km_s
		+ "Irradiance          %10.1f W/m²\n" % irradiance
		+ "Helion diameter     %10.4f°\n" % angular_diameter
		+ "Solar declination   %+10.3f°\n" % declination
		+ "Asterra day         %10.3f h\n" % (CelestialSystem.ASTERRA_ROTATION_PERIOD_S / 3600.0)
		+ "Asterra year        %10.3f local d" % CelestialSystem.orbital_period_asterra_days()
	)

func _update_camera_transform() -> void:
	if _camera == null:
		return
	var cp := cos(_camera_pitch)
	var direction := Vector3(
		sin(_camera_yaw) * cp,
		sin(_camera_pitch),
		cos(_camera_yaw) * cp
	).normalized()
	_camera.position = direction * _camera_distance
	_camera.look_at(Vector3.ZERO, Vector3.UP)

func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.unicode == QUOTE_UNICODE:
			toggle()
			get_viewport().set_input_as_handled()
			return
		if visible and event.keycode == KEY_ESCAPE:
			close()
			get_viewport().set_input_as_handled()
			return

	if not visible:
		return

	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_dragging = event.pressed
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_camera_distance = maxf(MIN_CAMERA_DISTANCE, _camera_distance * 0.88)
			_update_camera_transform()
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_camera_distance = minf(MAX_CAMERA_DISTANCE, _camera_distance * 1.14)
			_update_camera_transform()
		get_viewport().set_input_as_handled()
	elif event is InputEventMouseMotion:
		if _dragging:
			_camera_yaw -= event.relative.x * CAMERA_DRAG_SENSITIVITY
			_camera_pitch = clampf(
				_camera_pitch - event.relative.y * CAMERA_DRAG_SENSITIVITY,
				-1.35,
				1.35
			)
			_update_camera_transform()
		get_viewport().set_input_as_handled()

func toggle() -> void:
	if visible:
		close()
	else:
		open()

func open() -> void:
	if visible:
		return
	visible = true
	_dragging = false
	_stats_accumulator = 1.0
	_player = _find_player(get_tree().current_scene)
	_restore_mouse_capture = Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
	if _player != null:
		_restore_player_input = _player.input_enabled
		_player.input_enabled = false
		_player.set_mouse_captured(false)
	else:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_update_body_transforms()
	_update_camera_transform()
	_update_stats()

func close() -> void:
	if not visible:
		return
	visible = false
	_dragging = false
	if _player != null and is_instance_valid(_player):
		_player.input_enabled = _restore_player_input
		_player.set_mouse_captured(_restore_mouse_capture)
	elif _restore_mouse_capture:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	else:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_player = null

func _find_player(root: Node) -> AsterraPlayer:
	if root == null:
		return null
	if root is AsterraPlayer:
		return root as AsterraPlayer
	for child in root.get_children():
		var found := _find_player(child)
		if found != null:
			return found
	return null
