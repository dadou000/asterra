extends CanvasLayer
## Optional runtime readout for the camera-to-nearest-character distance.
##
## Character actors should normally join the `characters` group. The current
## Character Studio human is also recognised by its AsterraHuman node name so
## the debug works immediately without modifying the imported GLB.

const UPDATE_INTERVAL := 0.10

var _panel: PanelContainer
var _label: Label
var _elapsed := 0.0

func _ready() -> void:
	layer = 120
	_build_ui()
	AppSettings.debug_closest_character_distance_changed.connect(_on_setting_changed)
	_refresh_visibility()

func _process(delta: float) -> void:
	_elapsed += delta
	if _elapsed < UPDATE_INTERVAL:
		return
	_elapsed = 0.0
	_refresh_visibility()
	if not visible:
		return
	_update_readout()

func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.name = "ClosestCharacterDistancePanel"
	_panel.position = Vector2(390.0, 92.0)
	_panel.custom_minimum_size = Vector2(310.0, 38.0)

	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.025, 0.04, 0.055, 0.90)
	style.border_color = Color(0.22, 0.32, 0.40, 0.95)
	style.set_border_width_all(1)
	style.corner_radius_top_left = 6
	style.corner_radius_top_right = 6
	style.corner_radius_bottom_left = 6
	style.corner_radius_bottom_right = 6
	style.content_margin_left = 10.0
	style.content_margin_right = 10.0
	style.content_margin_top = 7.0
	style.content_margin_bottom = 7.0
	_panel.add_theme_stylebox_override("panel", style)
	add_child(_panel)

	_label = Label.new()
	_label.text = "Closest character: —"
	_label.add_theme_font_size_override("font_size", 14)
	_label.modulate = Color(0.82, 0.90, 0.96)
	_panel.add_child(_label)

func _on_setting_changed(_enabled: bool) -> void:
	_refresh_visibility()
	if visible:
		_update_readout()

func _refresh_visibility() -> void:
	var scene := get_tree().current_scene
	# Keep the launcher itself uncluttered; the toggle lives there but the debug
	# readout is intended for game/editor scenes.
	visible = AppSettings.debug_closest_character_distance \
		and scene != null \
		and str(scene.name) != "StartMenu"

func _update_readout() -> void:
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		_label.text = "Closest character: no active camera"
		return

	var closest: Node3D = null
	var closest_distance := INF
	var candidates := _collect_character_candidates()
	for candidate in candidates:
		if not is_instance_valid(candidate):
			continue
		# Never count the camera itself or one of its ancestors as a target.
		if candidate == camera or candidate.is_ancestor_of(camera):
			continue
		var distance := camera.global_position.distance_to(candidate.global_position)
		if distance < closest_distance:
			closest_distance = distance
			closest = candidate

	if closest == null:
		_label.text = "Closest character: none"
		return

	_label.text = "Closest character: %s  •  %s" % [
		str(closest.name),
		_format_distance(closest_distance)
	]

func _collect_character_candidates() -> Array[Node3D]:
	var result: Array[Node3D] = []
	var seen := {}

	for node in get_tree().get_nodes_in_group("characters"):
		if node is Node3D:
			var node3d := node as Node3D
			seen[node3d.get_instance_id()] = true
			result.append(node3d)

	var scene := get_tree().current_scene
	if scene != null:
		_collect_named_characters(scene, result, seen)
	return result

func _collect_named_characters(node: Node, result: Array[Node3D], seen: Dictionary) -> void:
	if node is Node3D:
		var node3d := node as Node3D
		var is_known_character := str(node3d.name) == "AsterraHuman" \
			or bool(node3d.get_meta("asterra_character", false))
		if is_known_character and not seen.has(node3d.get_instance_id()):
			seen[node3d.get_instance_id()] = true
			result.append(node3d)
	for child in node.get_children():
		_collect_named_characters(child, result, seen)

func _format_distance(distance_m: float) -> String:
	if distance_m < 10.0:
		return "%.2f m" % distance_m
	if distance_m < 1000.0:
		return "%.1f m" % distance_m
	return "%.3f km" % (distance_m / 1000.0)
