extends Node3D
## Interactive validation harness for TerrainDeformation.
##
## F10 toggles the harness while the Main world is running. The sphere uses a
## simple radial rigid-body integrator against the finite-strength terrain solver.
## The bucket is a kinematic hydraulic-tool proxy whose teeth submit cutting
## contacts. Both modify the same persistent Deltas seen by gameplay and rendering.

const SPHERE_RADIUS_M := 1.25
const TUNGSTEN_DENSITY_KG_M3 := 19250.0
const GRAVITY_MPS2 := 9.81
const SPHERE_SUBSTEPS := 4
const BUCKET_MOVE_MPS := 2.0
const BUCKET_FAST_MULTIPLIER := 4.0
const BUCKET_PITCH_RAD_S := 0.85
const BUCKET_TOOTH_COUNT := 5

var active := false
var material_id := 0

var _player: Node3D
var _camera: Camera3D
var _world_ready := false
var _center_dir := Vector3(1.0, 0.0, 0.0)
var _right := Vector3(0.0, 0.0, -1.0)
var _forward := Vector3(0.0, 1.0, 0.0)

var _sphere_node: MeshInstance3D
var _sphere_dir := Vector3(1.0, 0.0, 0.0)
var _sphere_altitude_msl := 0.0
var _sphere_velocity_mps := 0.0
var _sphere_running := false
var _sphere_last_contact: Dictionary = {}
var _sphere_mass_kg := 0.0

var _bucket_root: Node3D
var _bucket_offset_m := Vector2.ZERO
var _bucket_altitude_msl := 0.0
var _bucket_pitch_rad := -0.28
var _bucket_total_reaction_n := 0.0
var _bucket_last_moved_volume_m3 := 0.0

var _hud_layer: CanvasLayer
var _hud_panel: PanelContainer
var _hud_label: Label
var _hud_timer := 0.0


func _ready() -> void:
	process_priority = 5
	var volume_m3: float = 4.0 / 3.0 * PI * pow(SPHERE_RADIUS_M, 3.0)
	_sphere_mass_kg = volume_m3 * TUNGSTEN_DENSITY_KG_M3
	_build_visuals()
	_build_hud()
	_set_active_visuals(false)


func _input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key_event := event as InputEventKey
	if not key_event.pressed or key_event.echo:
		return
	if key_event.keycode == KEY_F10:
		_toggle()
		get_viewport().set_input_as_handled()
		return
	if not active:
		return
	match key_event.keycode:
		KEY_R:
			_place_sphere(true)
			get_viewport().set_input_as_handled()
		KEY_P:
			_sphere_running = not _sphere_running
			get_viewport().set_input_as_handled()
		KEY_BACKSPACE:
			_place_sphere(false)
			get_viewport().set_input_as_handled()
		KEY_B:
			_reset_bucket()
			get_viewport().set_input_as_handled()
		KEY_1:
			material_id = 0
			get_viewport().set_input_as_handled()
		KEY_2:
			material_id = 1
			get_viewport().set_input_as_handled()
		KEY_3:
			material_id = 2
			get_viewport().set_input_as_handled()
		KEY_4:
			material_id = 3
			get_viewport().set_input_as_handled()
		KEY_DELETE:
			Deltas.clear()
			TerrainDeformation.clear_state()
			_place_sphere(false)
			_reset_bucket()
			get_viewport().set_input_as_handled()


func _physics_process(dt: float) -> void:
	if not active:
		return
	if not _world_ready:
		_try_bind_world()
		return
	if _player == null or not is_instance_valid(_player) or not Planet.ready_state:
		_world_ready = false
		return
	_camera = get_viewport().get_camera_3d()
	if _camera == null:
		return
	_update_bucket(dt)
	if _sphere_running:
		var sub_dt: float = dt / float(SPHERE_SUBSTEPS)
		for _substep in SPHERE_SUBSTEPS:
			_step_sphere(sub_dt)
	_update_visual_transforms()
	_hud_timer += dt
	if _hud_timer >= 0.08:
		_hud_timer = 0.0
		_update_hud()


func _toggle() -> void:
	if active:
		active = false
		_world_ready = false
		_sphere_running = false
		_set_active_visuals(false)
		return
	var scene: Node = get_tree().current_scene
	if scene == null or scene.name != "Main":
		return
	active = true
	_set_active_visuals(true)
	_try_bind_world()


func _try_bind_world() -> void:
	if not active or not Planet.ready_state:
		return
	var scene: Node = get_tree().current_scene
	if scene == null or scene.name != "Main":
		return
	var player_value: Variant = scene.get("player")
	if not (player_value is Node3D):
		return
	_player = player_value as Node3D
	_camera = get_viewport().get_camera_3d()
	if _camera == null:
		return
	var up_value: Variant = _player.call("up_dir")
	if not (up_value is Vector3):
		return
	_center_dir = (up_value as Vector3).normalized()
	_rebuild_local_axes()
	_world_ready = true
	_place_sphere(false)
	_reset_bucket()
	_update_visual_transforms()
	_update_hud()


func _rebuild_local_axes() -> void:
	if _camera != null:
		var camera_forward: Vector3 = -_camera.global_basis.z
		camera_forward -= _center_dir * camera_forward.dot(_center_dir)
		if camera_forward.length_squared() > 1e-8:
			_forward = camera_forward.normalized()
			_right = _forward.cross(_center_dir).normalized()
			return
	var basis: Array = CubeSphere.tangent_basis(_center_dir)
	_right = (basis[0] as Vector3).normalized()
	_forward = (basis[1] as Vector3).normalized()


func _place_sphere(drop_immediately: bool) -> void:
	if not _world_ready:
		return
	var target_dir: Vector3 = _direction_from_offset(Vector2(0.0, 9.0))
	var aim_value: Variant = _player.call("aim")
	if aim_value is Dictionary:
		var aim: Dictionary = aim_value
		if aim.has("dir"):
			var dir_value: Variant = aim["dir"]
			if dir_value is Vector3:
				target_dir = (dir_value as Vector3).normalized()
	_sphere_dir = target_dir
	var ground_height_m: float = TerrainContactSampler.height(_sphere_dir)
	_sphere_altitude_msl = ground_height_m + SPHERE_RADIUS_M + 10.0
	_sphere_velocity_mps = 0.0
	_sphere_running = drop_immediately
	_sphere_last_contact.clear()
	_update_visual_transforms()


func _reset_bucket() -> void:
	if not _world_ready:
		return
	_bucket_offset_m = Vector2(3.5, 7.0)
	var bucket_dir: Vector3 = _direction_from_offset(_bucket_offset_m)
	_bucket_altitude_msl = TerrainContactSampler.height(bucket_dir) + 2.4
	_bucket_pitch_rad = -0.32
	_bucket_total_reaction_n = 0.0
	_bucket_last_moved_volume_m3 = 0.0


func _step_sphere(dt: float) -> void:
	var ground_height_m: float = TerrainContactSampler.height(_sphere_dir)
	var bottom_altitude_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	var penetration_m: float = ground_height_m - bottom_altitude_m
	var acceleration_mps2 := -GRAVITY_MPS2
	_sphere_last_contact.clear()
	if penetration_m > 0.0:
		var capped_penetration_m: float = minf(penetration_m, SPHERE_RADIUS_M)
		var contact_radius_sq: float = 2.0 * SPHERE_RADIUS_M * capped_penetration_m
		contact_radius_sq -= capped_penetration_m * capped_penetration_m
		var contact_radius_m: float = sqrt(maxf(contact_radius_sq, 0.05))
		contact_radius_m = clampf(contact_radius_m, 0.24, SPHERE_RADIUS_M)
		var inward_speed_mps: float = maxf(-_sphere_velocity_mps, 0.0)
		var stopping_distance_m: float = maxf(0.25, minf(SPHERE_RADIUS_M, penetration_m + 0.30))
		var impact_force_n: float = _sphere_mass_kg * inward_speed_mps * inward_speed_mps
		impact_force_n /= 2.0 * stopping_distance_m
		var load_n: float = _sphere_mass_kg * GRAVITY_MPS2 + impact_force_n
		_sphere_last_contact = TerrainDeformation.apply_contact(
			_sphere_dir, contact_radius_m, load_n, penetration_m,
			_sphere_velocity_mps, 0.0, dt, 0.0, material_id)
		var support_force_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
		acceleration_mps2 += support_force_n / maxf(_sphere_mass_kg, 1.0)
		acceleration_mps2 = clampf(acceleration_mps2, -180.0, 260.0)
	_sphere_velocity_mps += acceleration_mps2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt
	if penetration_m > 0.0 and absf(_sphere_velocity_mps) < 0.015:
		var support_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
		var weight_n: float = _sphere_mass_kg * GRAVITY_MPS2
		if support_n >= weight_n * 0.98:
			_sphere_velocity_mps = 0.0


func _update_bucket(dt: float) -> void:
	var speed_multiplier := 1.0
	if Input.is_key_pressed(KEY_SHIFT):
		speed_multiplier = BUCKET_FAST_MULTIPLIER
	var tangent_velocity := Vector2.ZERO
	if Input.is_key_pressed(KEY_J):
		tangent_velocity.x -= 1.0
	if Input.is_key_pressed(KEY_L):
		tangent_velocity.x += 1.0
	if Input.is_key_pressed(KEY_I):
		tangent_velocity.y += 1.0
	if Input.is_key_pressed(KEY_K):
		tangent_velocity.y -= 1.0
	if tangent_velocity.length_squared() > 1.0:
		tangent_velocity = tangent_velocity.normalized()
	tangent_velocity *= BUCKET_MOVE_MPS * speed_multiplier
	var vertical_speed_mps := 0.0
	if Input.is_key_pressed(KEY_U):
		vertical_speed_mps += BUCKET_MOVE_MPS * speed_multiplier
	if Input.is_key_pressed(KEY_O):
		vertical_speed_mps -= BUCKET_MOVE_MPS * speed_multiplier
	if Input.is_key_pressed(KEY_Z):
		_bucket_pitch_rad += BUCKET_PITCH_RAD_S * speed_multiplier * dt
	if Input.is_key_pressed(KEY_X):
		_bucket_pitch_rad -= BUCKET_PITCH_RAD_S * speed_multiplier * dt
	_bucket_pitch_rad = clampf(_bucket_pitch_rad, -1.7, 0.8)
	_bucket_offset_m += tangent_velocity * dt
	_bucket_altitude_msl += vertical_speed_mps * dt
	_update_bucket_transform()
	_bucket_total_reaction_n = 0.0
	_bucket_last_moved_volume_m3 = 0.0
	var tangential_speed_mps: float = tangent_velocity.length()
	for tooth_index in BUCKET_TOOTH_COUNT:
		var tooth_x: float = lerpf(-0.95, 0.95, float(tooth_index) / float(BUCKET_TOOTH_COUNT - 1))
		var local_tooth := Vector3(tooth_x, -0.50, -0.95)
		var render_point: Vector3 = _bucket_root.to_global(local_tooth)
		var world_point: Vec3D = Frames.to_world(render_point)
		if world_point.length_sq() <= 1.0:
			continue
		var tooth_dir: Vector3 = world_point.normalized().to_v3()
		var tooth_altitude_m: float = world_point.length() - Planet.cfg.planet_radius
		var ground_height_m: float = TerrainContactSampler.height(tooth_dir)
		var penetration_m: float = ground_height_m - tooth_altitude_m
		if penetration_m <= 0.0:
			continue
		var actuator_force_n: float = 110000.0 + penetration_m * 760000.0
		actuator_force_n += maxf(-vertical_speed_mps, 0.0) * 180000.0
		actuator_force_n = clampf(actuator_force_n, 0.0, 950000.0)
		var response: Dictionary = TerrainDeformation.apply_contact(
			tooth_dir, 0.34, actuator_force_n, penetration_m,
			vertical_speed_mps, tangential_speed_mps, dt, 1.0, material_id)
		_bucket_total_reaction_n += float(response.get("support_force_n", 0.0))
		_bucket_last_moved_volume_m3 += float(response.get("moved_volume_m3", 0.0))


func _update_visual_transforms() -> void:
	if not _world_ready:
		return
	var sphere_world: Vec3D = Frames.dir_altitude_to_world(_sphere_dir, _sphere_altitude_msl)
	_sphere_node.global_position = Frames.to_render(sphere_world)
	_update_bucket_transform()


func _update_bucket_transform() -> void:
	if not _world_ready or _bucket_root == null:
		return
	var bucket_dir: Vector3 = _direction_from_offset(_bucket_offset_m)
	var tangent_forward: Vector3 = _forward - bucket_dir * _forward.dot(bucket_dir)
	if tangent_forward.length_squared() <= 1e-8:
		tangent_forward = _right.cross(bucket_dir)
	tangent_forward = tangent_forward.normalized()
	var tangent_right: Vector3 = tangent_forward.cross(bucket_dir).normalized()
	var base_basis := Basis(tangent_right, bucket_dir, -tangent_forward)
	var pitch_basis := Basis(Vector3.RIGHT, _bucket_pitch_rad)
	var world: Vec3D = Frames.dir_altitude_to_world(bucket_dir, _bucket_altitude_msl)
	_bucket_root.global_transform = Transform3D(base_basis * pitch_basis, Frames.to_render(world))


func _direction_from_offset(offset_m: Vector2) -> Vector3:
	var planet_radius: float = maxf(Planet.cfg.planet_radius, 1.0)
	return (_center_dir + _right * (offset_m.x / planet_radius) +
		_forward * (offset_m.y / planet_radius)).normalized()


func _build_visuals() -> void:
	_sphere_node = MeshInstance3D.new()
	_sphere_node.name = "TungstenDropSphere"
	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = SPHERE_RADIUS_M
	sphere_mesh.height = SPHERE_RADIUS_M * 2.0
	sphere_mesh.radial_segments = 48
	sphere_mesh.rings = 24
	_sphere_node.mesh = sphere_mesh
	var tungsten := StandardMaterial3D.new()
	tungsten.albedo_color = Color(0.24, 0.26, 0.29)
	tungsten.metallic = 0.93
	tungsten.roughness = 0.24
	_sphere_node.material_override = tungsten
	add_child(_sphere_node)

	_bucket_root = Node3D.new()
	_bucket_root.name = "ExcavatorBucketProxy"
	add_child(_bucket_root)
	var bucket_material := StandardMaterial3D.new()
	bucket_material.albedo_color = Color(0.72, 0.42, 0.08)
	bucket_material.metallic = 0.72
	bucket_material.roughness = 0.34
	_add_box(_bucket_root, Vector3(2.35, 0.16, 1.45), Vector3(0.0, 0.36, 0.0), bucket_material)
	_add_box(_bucket_root, Vector3(2.35, 0.86, 0.15), Vector3(0.0, 0.0, 0.64), bucket_material)
	_add_box(_bucket_root, Vector3(0.16, 0.82, 1.30), Vector3(-1.10, 0.0, 0.0), bucket_material)
	_add_box(_bucket_root, Vector3(0.16, 0.82, 1.30), Vector3(1.10, 0.0, 0.0), bucket_material)
	for tooth_index in BUCKET_TOOTH_COUNT:
		var tooth_x: float = lerpf(-0.95, 0.95, float(tooth_index) / float(BUCKET_TOOTH_COUNT - 1))
		_add_box(_bucket_root, Vector3(0.18, 0.16, 0.62), Vector3(tooth_x, -0.48, -0.82), bucket_material)


func _add_box(parent: Node3D, size: Vector3, local_position: Vector3,
		material: Material) -> void:
	var instance := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	instance.mesh = box
	instance.position = local_position
	instance.material_override = material
	parent.add_child(instance)


func _build_hud() -> void:
	_hud_layer = CanvasLayer.new()
	_hud_layer.layer = 90
	add_child(_hud_layer)
	_hud_panel = PanelContainer.new()
	_hud_panel.position = Vector2(18.0, 18.0)
	_hud_panel.custom_minimum_size = Vector2(470.0, 0.0)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.035, 0.045, 0.055, 0.92)
	style.border_color = Color(0.28, 0.36, 0.42, 0.95)
	style.set_border_width_all(1)
	style.corner_radius_top_left = 7
	style.corner_radius_top_right = 7
	style.corner_radius_bottom_left = 7
	style.corner_radius_bottom_right = 7
	style.content_margin_left = 14.0
	style.content_margin_right = 14.0
	style.content_margin_top = 12.0
	style.content_margin_bottom = 12.0
	_hud_panel.add_theme_stylebox_override("panel", style)
	_hud_layer.add_child(_hud_panel)
	_hud_label = Label.new()
	_hud_label.add_theme_font_size_override("font_size", 14)
	_hud_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_hud_panel.add_child(_hud_label)


func _set_active_visuals(enabled: bool) -> void:
	if _sphere_node != null:
		_sphere_node.visible = enabled
	if _bucket_root != null:
		_bucket_root.visible = enabled
	if _hud_panel != null:
		_hud_panel.visible = enabled


func _update_hud() -> void:
	if _hud_label == null:
		return
	if not _world_ready:
		_hud_label.text = "TERRAIN DEFORMATION EXPERIMENT\nWaiting for the generated world...\nF10: close experiment"
		return
	var ground_height_m: float = TerrainContactSampler.height(_sphere_dir)
	var clearance_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M - ground_height_m
	var bearing_ratio := 0.0
	var sink_rate := 0.0
	if not _sphere_last_contact.is_empty():
		bearing_ratio = float(_sphere_last_contact.get("bearing_ratio", 0.0))
		sink_rate = float(_sphere_last_contact.get("sink_rate_mps", 0.0))
	var state_stats: Dictionary = TerrainDeformation.stats()
	_hud_label.text = (
		"TERRAIN DEFORMATION EXPERIMENT\n" +
		"Material [1-4]: %s\n" % TerrainDeformation.material_name(material_id) +
		"Tungsten sphere: %.1f t | clearance %+6.2f m | v %+6.2f m/s | bearing %.2fx | sink %.3f m/s\n" % [
			_sphere_mass_kg / 1000.0, clearance_m, _sphere_velocity_mps, bearing_ratio, sink_rate] +
		"Bucket reaction: %.0f kN | moved this step %.4f m3 | state tiles %d\n\n" % [
			_bucket_total_reaction_n / 1000.0, _bucket_last_moved_volume_m3,
			int(state_stats.get("state_tiles", 0))] +
		"R place + DROP sphere at aim | P pause/resume | Backspace reset suspended\n" +
		"Bucket: I/K forward/back  J/L left/right  U/O up/down  Z/X pitch  Shift fast  B reset\n" +
		"1 topsoil  2 wet clay  3 gravel  4 rock | Delete clears terrain edits | F10 close"
	)
