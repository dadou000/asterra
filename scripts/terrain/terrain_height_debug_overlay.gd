extends Node
## Additional terrain-height diagnostics layered on top of TerrainDebug.
##
## This stays separate from the production terrain stack. It freezes the existing
## AGL/GPU/physics diagnostic samples without freezing the clipmap itself, and can
## draw a tangent magenta reference grid at the ground altitude implied by AGL.

const INSTALL_RETRY_S: float = 0.35
const FLOOR_SAMPLE_INTERVAL_S: float = 0.10
const FROZEN_HEIGHT_ACCUM_S: float = -1000000.0
const FLOOR_HALF_EXTENT_M: float = 12.0
const FLOOR_GRID_STEP_M: float = 1.0
const FLOOR_LIFT_M: float = 0.018
const FLOOR_COLOR := Color(1.0, 0.08, 0.72, 0.82)
const TERRAIN_DEBUG_SCRIPT_PATH := "res://scripts/terrain/terrain_debug.gd"

var _terrain_debug: Node
var _controls_box: VBoxContainer
var _controls_root: VBoxContainer
var _status_label: Label
var _freeze_button: CheckButton
var _floor_button: CheckButton

var _install_accum_s: float = INSTALL_RETRY_S
var _floor_sample_accum_s: float = FLOOR_SAMPLE_INTERVAL_S
var _points_frozen: bool = false
var _agl_floor_enabled: bool = false

var _frozen_agl_sample: Dictionary = {}
var _frozen_gpu_sample: Dictionary = {}
var _frozen_physics_sample: Dictionary = {}
var _agl_floor_sample: Dictionary = {}

var _agl_floor: MeshInstance3D
var _floor_scene_id: int = 0


func _ready() -> void:
	process_priority = 30


func _process(delta: float) -> void:
	_install_accum_s += delta
	if _terrain_debug == null or not is_instance_valid(_terrain_debug) \
			or _controls_box == null or not is_instance_valid(_controls_box):
		if _install_accum_s >= INSTALL_RETRY_S:
			_install_accum_s = 0.0
			_try_install()

	if _points_frozen:
		_hold_frozen_cursor_samples()

	if _agl_floor_enabled:
		_floor_sample_accum_s += delta
		if not _points_frozen and _floor_sample_accum_s >= FLOOR_SAMPLE_INTERVAL_S:
			_floor_sample_accum_s = fmod(_floor_sample_accum_s, FLOOR_SAMPLE_INTERVAL_S)
			_sample_live_agl_floor()
		_sync_agl_floor()
	elif _agl_floor != null and is_instance_valid(_agl_floor):
		_agl_floor.visible = false


func _try_install() -> void:
	var current_scene: Node = get_tree().current_scene
	if current_scene == null:
		return
	var terrain_debug: Node = _find_terrain_debug(current_scene)
	if terrain_debug == null:
		return
	var installed_value: Variant = terrain_debug.get("_gpu_controls_installed")
	if not (installed_value is bool) or not bool(installed_value):
		return
	var controls_value: Node = current_scene.find_child("TerrainControls", true, false)
	if not (controls_value is VBoxContainer):
		return
	var box: VBoxContainer = controls_value as VBoxContainer
	var existing: Node = box.get_node_or_null("HeightDiagnosticHoldControls")
	if existing is VBoxContainer:
		_terrain_debug = terrain_debug
		_controls_box = box
		_controls_root = existing as VBoxContainer
		return

	_terrain_debug = terrain_debug
	_controls_box = box
	_controls_root = VBoxContainer.new()
	_controls_root.name = "HeightDiagnosticHoldControls"
	box.add_child(_controls_root)
	_controls_root.add_child(HSeparator.new())

	var title := Label.new()
	title.text = "Height comparison hold / reference floor"
	title.add_theme_font_size_override("font_size", 14)
	title.add_theme_color_override("font_color", Color(0.86, 0.91, 1.0))
	_controls_root.add_child(title)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.add_theme_font_size_override("font_size", 11)
	_status_label.add_theme_color_override("font_color", Color(0.74, 0.82, 0.92))
	_controls_root.add_child(_status_label)

	_freeze_button = _make_toggle(
		"Freeze diagnostic points", _points_frozen,
		"Holds the current cyan/yellow/magenta samples in planet coordinates. The terrain and floating origin continue normally.")
	_freeze_button.toggled.connect(set_points_frozen)
	_controls_root.add_child(_freeze_button)

	_floor_button = _make_toggle(
		"MAGENTA — fake AGL wireframe floor", _agl_floor_enabled,
		"Draws a 24 m × 24 m tangent grid at the ground MSL implied by player AGL. It is a reference plane, not terrain geometry.")
	_floor_button.toggled.connect(set_agl_floor_enabled)
	_controls_root.add_child(_floor_button)

	var note := Label.new()
	note.text = "     Freeze also holds the AGL floor so you can move the camera and compare the real terrain against a fixed reference."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.add_theme_font_size_override("font_size", 11)
	note.add_theme_color_override("font_color", Color(0.58, 0.65, 0.76))
	_controls_root.add_child(note)
	_update_status_label()


func _find_terrain_debug(root: Node) -> Node:
	var script_value: Variant = root.get_script()
	if script_value is Script:
		var script: Script = script_value as Script
		if script.resource_path == TERRAIN_DEBUG_SCRIPT_PATH:
			return root
	for child: Node in root.get_children():
		var found: Node = _find_terrain_debug(child)
		if found != null:
			return found
	return null


func _make_toggle(text: String, initial: bool, tooltip: String) -> CheckButton:
	var button := CheckButton.new()
	button.text = text
	button.button_pressed = initial
	button.tooltip_text = tooltip
	button.custom_minimum_size = Vector2(0.0, 34.0)
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.add_theme_font_size_override("font_size", 13)
	return button


func set_points_frozen(value: bool) -> void:
	if _points_frozen == value:
		return
	_points_frozen = value
	if value:
		_capture_cursor_samples()
		if _agl_floor_enabled:
			_sample_live_agl_floor()
		_hold_frozen_cursor_samples()
	else:
		_release_cursor_samples()
	_floor_sample_accum_s = FLOOR_SAMPLE_INTERVAL_S
	_update_status_label()


func set_agl_floor_enabled(value: bool) -> void:
	_agl_floor_enabled = value
	_floor_sample_accum_s = FLOOR_SAMPLE_INTERVAL_S
	if value:
		if _agl_floor_sample.is_empty() or not _points_frozen:
			_sample_live_agl_floor()
		_ensure_agl_floor()
		_sync_agl_floor()
	elif _agl_floor != null and is_instance_valid(_agl_floor):
		_agl_floor.visible = false
	_update_status_label()


func _capture_cursor_samples() -> void:
	if _terrain_debug == null or not is_instance_valid(_terrain_debug):
		return
	# Force one synchronous controller refresh before taking the snapshot. The GPU
	# value can still legitimately be pending if its asynchronous readback has not
	# completed yet; in that case the frozen yellow point remains absent.
	if _terrain_debug.has_method("_refresh_height_diagnostics"):
		_terrain_debug.call("_refresh_height_diagnostics")
	_frozen_agl_sample = _read_debug_sample("_agl_sample")
	_frozen_gpu_sample = _read_debug_sample("_gpu_sample")
	_frozen_physics_sample = _read_debug_sample("_physics_sample")


func _read_debug_sample(property_name: StringName) -> Dictionary:
	if _terrain_debug == null or not is_instance_valid(_terrain_debug):
		return {}
	var value: Variant = _terrain_debug.get(property_name)
	if value is Dictionary:
		return (value as Dictionary).duplicate(true)
	return {}


func _hold_frozen_cursor_samples() -> void:
	if _terrain_debug == null or not is_instance_valid(_terrain_debug):
		return
	# TerrainDebug increments this accumulator before requesting new samples. Holding
	# it far below zero leaves its normal per-frame marker/world-origin sync running,
	# while preventing any new AGL/GPU/physics point from replacing the snapshot.
	_terrain_debug.set("_height_debug_accum", FROZEN_HEIGHT_ACCUM_S)
	_restore_debug_sample("_agl_sample", _frozen_agl_sample)
	_restore_debug_sample("_gpu_sample", _frozen_gpu_sample)
	_restore_debug_sample("_physics_sample", _frozen_physics_sample)


func _restore_debug_sample(property_name: StringName, frozen_sample: Dictionary) -> void:
	var current_value: Variant = _terrain_debug.get(property_name)
	if current_value is Dictionary and (current_value as Dictionary) == frozen_sample:
		return
	_terrain_debug.set(property_name, frozen_sample.duplicate(true))


func _release_cursor_samples() -> void:
	if _terrain_debug != null and is_instance_valid(_terrain_debug):
		# Trigger a fresh set on the next TerrainDebug process tick.
		_terrain_debug.set("_height_debug_accum", 1.0)
		if _terrain_debug.has_method("_refresh_height_diagnostics"):
			_terrain_debug.call_deferred("_refresh_height_diagnostics")
	_frozen_agl_sample.clear()
	_frozen_gpu_sample.clear()
	_frozen_physics_sample.clear()


func _sample_live_agl_floor() -> void:
	var player: Node = _player_node()
	if player == null or not player.has_method("altitude") \
			or not player.has_method("height_above_ground") \
			or not player.has_method("up_dir"):
		_agl_floor_sample.clear()
		return
	var altitude_msl: float = float(player.call("altitude"))
	var agl_m: float = float(player.call("height_above_ground"))
	var direction_value: Variant = player.call("up_dir")
	if not (direction_value is Vector3) or not is_finite(altitude_msl) \
			or not is_finite(agl_m):
		_agl_floor_sample.clear()
		return
	var direction: Vector3 = (direction_value as Vector3).normalized()
	_agl_floor_sample = {
		"dir": direction,
		"height": altitude_msl - agl_m,
		"agl": agl_m,
	}


func _player_node() -> Node:
	if _terrain_debug == null or not is_instance_valid(_terrain_debug):
		return null
	var main: Node = _terrain_debug.get_parent()
	if main == null:
		return null
	var player_value: Variant = main.get("player")
	return player_value as Node if player_value is Node else null


func _ensure_agl_floor() -> void:
	var current_scene: Node = get_tree().current_scene
	if current_scene == null or not (current_scene is Node3D):
		return
	var scene_id: int = current_scene.get_instance_id()
	if _agl_floor != null and is_instance_valid(_agl_floor) and _floor_scene_id == scene_id:
		return
	_agl_floor = MeshInstance3D.new()
	_agl_floor.name = "AGLReferenceWireframeFloor"
	_agl_floor.mesh = _build_floor_mesh()
	_agl_floor.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_agl_floor.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	_agl_floor.extra_cull_margin = FLOOR_HALF_EXTENT_M * 2.0
	_agl_floor.visible = false
	(current_scene as Node3D).add_child(_agl_floor)
	_floor_scene_id = scene_id


func _build_floor_mesh() -> ImmediateMesh:
	var mesh := ImmediateMesh.new()
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = FLOOR_COLOR
	material.emission_enabled = true
	material.emission = Color(FLOOR_COLOR.r, FLOOR_COLOR.g, FLOOR_COLOR.b)
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.surface_begin(Mesh.PRIMITIVE_LINES, material)
	var line_count: int = int(round(FLOOR_HALF_EXTENT_M / FLOOR_GRID_STEP_M))
	for index: int in range(-line_count, line_count + 1):
		var offset: float = float(index) * FLOOR_GRID_STEP_M
		mesh.surface_add_vertex(Vector3(-FLOOR_HALF_EXTENT_M, 0.0, offset))
		mesh.surface_add_vertex(Vector3(FLOOR_HALF_EXTENT_M, 0.0, offset))
		mesh.surface_add_vertex(Vector3(offset, 0.0, -FLOOR_HALF_EXTENT_M))
		mesh.surface_add_vertex(Vector3(offset, 0.0, FLOOR_HALF_EXTENT_M))
	mesh.surface_end()
	return mesh


func _sync_agl_floor() -> void:
	if not _agl_floor_enabled or _agl_floor_sample.is_empty():
		if _agl_floor != null and is_instance_valid(_agl_floor):
			_agl_floor.visible = false
		return
	_ensure_agl_floor()
	if _agl_floor == null or not is_instance_valid(_agl_floor):
		return
	var direction_value: Variant = _agl_floor_sample.get("dir", null)
	if not (direction_value is Vector3):
		_agl_floor.visible = false
		return
	var direction: Vector3 = (direction_value as Vector3).normalized()
	var height: float = float(_agl_floor_sample.get("height", NAN))
	if not is_finite(height):
		_agl_floor.visible = false
		return
	var render_value: Variant = _render_position(direction, height)
	if not (render_value is Vector3):
		_agl_floor.visible = false
		return
	var render_position: Vector3 = render_value as Vector3
	var reference: Vector3 = Vector3.UP if absf(direction.dot(Vector3.UP)) < 0.94 else Vector3.FORWARD
	var right: Vector3 = reference.cross(direction).normalized()
	var forward: Vector3 = right.cross(direction).normalized()
	var floor_basis := Basis(right, direction, forward)
	_agl_floor.global_transform = Transform3D(
		floor_basis, render_position + direction * FLOOR_LIFT_M)
	_agl_floor.visible = true


func _render_position(direction: Vector3, height: float) -> Variant:
	var frames: Node = get_node_or_null("/root/Frames")
	if frames == null or not frames.has_method("dir_altitude_to_world") \
			or not frames.has_method("to_render"):
		return null
	var world_value: Variant = frames.call("dir_altitude_to_world", direction, height)
	if world_value == null:
		return null
	return frames.call("to_render", world_value)


func _update_status_label() -> void:
	if _status_label == null:
		return
	var state: String = "FROZEN" if _points_frozen else "LIVE"
	var floor_text: String = "floor OFF"
	if _agl_floor_enabled:
		if _agl_floor_sample.is_empty():
			floor_text = "floor pending AGL"
		else:
			floor_text = "floor %.3f m MSL" % float(_agl_floor_sample.get("height", 0.0))
	_status_label.text = "     %s  •  %s" % [state, floor_text]
