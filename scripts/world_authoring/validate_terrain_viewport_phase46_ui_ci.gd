extends Node
## Phase 46 UI regression through the actual PlanetStudio scene and live terrain pick.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const PRESETS := preload("res://scripts/world_authoring/model/terrain_feature_preset_catalog.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const EXPECTED_EDITOR_PATH := "res://scripts/world_authoring/world_authoring_editor_live_phase46.gd"
const PLACE_MODE: int = 100
const EDIT_MODE: int = 101

class DummyPlayer extends Node3D:
	var camera: Camera3D = null
	var input_enabled: bool = false
	func set_mouse_captured(_enabled: bool) -> void:
		pass

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase46_viewport_ui_recovery_%d.tres" % OS.get_process_id())
	if Planet.has_method("configure_for_sculpt_ci"):
		Planet.call("configure_for_sculpt_ci", 1000000.0)
	Frames.set_planet_radius(1000000.0)

	var world := DummyWorld.new()
	world.name = "Phase46ViewportWorld"
	add_child(world)
	var player := DummyPlayer.new()
	player.name = "Player"
	world.add_child(player)
	world.player = player
	var camera := Camera3D.new()
	camera.name = "Camera"
	camera.fov = 70.0
	camera.current = true
	player.add_child(camera)
	player.camera = camera
	camera.global_position = Vector3(1100000.0, 0.0, 0.0)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)

	var editor: Control = PLANET_STUDIO_SCENE.instantiate() as Control
	if editor == null:
		_fail("PlanetStudio.tscn failed to instantiate")
		return
	var script: Script = editor.get_script() as Script
	if script == null or script.resource_path != EXPECTED_EDITOR_PATH:
		_fail("PlanetStudio.tscn is not using the Phase 46 viewport editor wrapper")
		return
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(5)
	editor.call("_show_category", "SHADERS")
	await _frames(6)
	var local_tab: Button = _find_button(editor, "LOCAL FEATURES")
	if local_tab == null:
		_fail("Local Features tab is missing")
		return
	local_tab.emit_signal("pressed")
	await _frames(6)

	var shelf: Node = _find_named(editor, "TerrainPresetShelf")
	var crater_button: Button = _find_named(shelf, "Preset_%s" % PRESETS.CRATER) as Button
	if crater_button == null:
		_fail("Impact Crater preset is missing")
		return
	crater_button.emit_signal("pressed")
	await _frames(8)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Planet Studio has no active terrain profile")
		return
	var features: Array[Resource] = _features(terrain)
	if features.size() != 2:
		_fail("Impact Crater did not create its two ordinary guided features")
		return

	# Phase 46 should transiently group exactly the newly-created ordinary slots.
	# Phase 45's dedicated UI regression owns the exact display-name contract; this
	# regression resolves parts from the placement IDs and their authoritative graph
	# semantics so it tests Phase 46 rather than duplicating Phase 45 assertions.
	if int(editor.get("_placement_mode")) != PLACE_MODE:
		_fail("multi-part crater was not automatically armed for viewport placement")
		return
	var place_ids: Array = editor.get("_phase46_place_slot_ids") as Array
	if place_ids.size() != 2:
		_fail("crater viewport placement group does not contain exactly two ordinary slots")
		return
	var basin: Resource = null
	var rim: Resource = null
	for id_value: Variant in place_ids:
		var grouped_slot: Resource = terrain.call("find_shader_slot", String(id_value)) as Resource
		if grouped_slot == null or not features.has(grouped_slot):
			_fail("crater placement group contains a slot outside the newly-created feature stack")
			return
		var grouped_graph: Resource = grouped_slot.get(&"graph") as Resource
		if not GUIDED.is_guided_graph(grouped_graph):
			_fail("crater placement group contains a non-guided graph")
			return
		var grouped_config: Dictionary = GUIDED.config_from_graph(grouped_graph)
		match String(grouped_config.get("area_kind", "")):
			GUIDED.AREA_RADIAL:
				basin = grouped_slot
			GUIDED.AREA_RING:
				rim = grouped_slot
	if basin == null or rim == null:
		_fail("crater placement group is not the expected Radial basin + Ring rim")
		return
	if String(editor.get("_phase43_selected_feature_id")) != String(basin.get(&"slot_id")):
		_fail("Impact Crater did not leave its first Radial part selected for editing")
		return
	var place_button: Button = _find_named(editor, "PlaceFeatureInViewport") as Button
	if place_button == null or place_button.disabled or not place_button.text.contains("PLACING"):
		_fail("selected crater part does not expose the active Place on Planet control")
		return

	# Exercise the real Phase 0 live terrain picker from the visible right-hand
	# viewport strip. The chosen direction then goes through Phase 46 placement.
	var screen_point := Vector2(1480.0, 450.0)
	var hit: Dictionary = editor.call("_screen_aim", screen_point) as Dictionary
	if hit.is_empty() or Vector3(hit.get("dir", Vector3.ZERO)).length_squared() < 0.99:
		_fail("live viewport terrain pick did not return an authoritative surface direction")
		return
	var target_direction := Vector3(hit.get("dir", Vector3.ZERO)).normalized()
	editor.set("_last_hit", hit)
	editor.call("_place_current_hit", false)
	await _frames(6)
	if int(editor.get("_placement_mode")) != 0:
		_fail("one-click feature placement did not disarm after committing")
		return

	var target_lat_lon: Vector2 = editor.call("_phase46_lat_lon_from_direction", target_direction)
	var basin_config: Dictionary = GUIDED.config_from_graph(basin.get(&"graph") as Resource)
	var rim_config: Dictionary = GUIDED.config_from_graph(rim.get(&"graph") as Resource)
	for config: Dictionary in [basin_config, rim_config]:
		if absf(float(config.get("center_latitude_deg", 999.0)) - target_lat_lon.x) > 0.01 \
				or _longitude_error(float(config.get("center_longitude_deg", 999.0)), target_lat_lon.y) > 0.01:
			_fail("multi-part crater did not move both ordinary graphs to the picked point")
			return
	if not is_equal_approx(float(basin_config.get("radius_deg", 0.0)), 5.0) \
			or not is_equal_approx(float(rim_config.get("inner_radius_deg", 0.0)), 5.2) \
			or not is_equal_approx(float(rim_config.get("outer_radius_deg", 0.0)), 7.0):
		_fail("viewport placement changed crater size instead of only moving it")
		return

	# Enable handles through the real button. Boundary/handle geometry should appear
	# in the existing live ImmediateMesh rather than a disconnected overlay system.
	var edit_button: Button = _find_named(editor, "EditFeatureViewportHandles") as Button
	if edit_button == null or edit_button.disabled:
		_fail("viewport handle editor button is missing or disabled")
		return
	edit_button.emit_signal("pressed")
	await _frames(5)
	if int(editor.get("_placement_mode")) != EDIT_MODE \
			or String(editor.get("_phase46_edit_slot_id")) != String(basin.get(&"slot_id")):
		_fail("Edit Viewport Handles did not arm the selected guided feature")
		return
	var preview: ImmediateMesh = editor.get("_preview_mesh") as ImmediateMesh
	if preview == null or preview.get_surface_count() < 3:
		_fail("radial feature boundary/handles were not drawn into the live preview mesh")
		return

	var handles: Dictionary = editor.call("_phase46_handle_directions", basin_config) as Dictionary
	if not handles.has("center") or not handles.has("radius"):
		_fail("radial feature is missing center/radius viewport handles")
		return
	var projected_radius: Vector2 = editor.call("_phase46_project_direction", Vector3(handles["radius"]))
	if not is_finite(projected_radius.x) or not is_finite(projected_radius.y):
		_fail("visible radial handle could not project into viewport coordinates")
		return
	if String(editor.call("_phase46_pick_handle", projected_radius)) != "radius":
		_fail("screen-space handle hit test did not resolve the radial radius handle")
		return

	# Simulate the actual drag mutation at a new great-circle radius. The same guided
	# graph should change and remain compilable by all existing runtime paths.
	editor.set("_phase46_drag_handle", "radius")
	var radius_target: Vector3 = editor.call("_phase46_offset_direction", target_direction, 9.0, 0.0)
	editor.call("_phase46_apply_drag_handle", radius_target, true)
	await _frames(3)
	basin_config = GUIDED.config_from_graph(basin.get(&"graph") as Resource)
	if absf(float(basin_config.get("radius_deg", 0.0)) - 9.0) > 0.01:
		_fail("viewport radius drag did not update the ordinary guided graph")
		return

	print("TERRAIN_VIEWPORT_PHASE46_UI_OK: live pick places multi-part presets together; on-world center/radius handles draw, hit-test and edit the same guided graph")
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _features(terrain: Resource) -> Array[Resource]:
	var out: Array[Resource] = []
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = value as Resource
		if slot != null and String(slot.get(&"slot_id")) != NATIVE.PRODUCTION_SHAPE_SLOT_ID:
			out.append(slot)
	return out


func _find_named(root: Node, node_name: String) -> Node:
	if root == null:
		return null
	if String(root.name) == node_name:
		return root
	for child: Node in root.get_children():
		var found: Node = _find_named(child, node_name)
		if found != null:
			return found
	return null


func _find_button(root: Node, text: String) -> Button:
	if root == null:
		return null
	if root is Button and (root as Button).text == text:
		return root as Button
	for child: Node in root.get_children():
		var found: Button = _find_button(child, text)
		if found != null:
			return found
	return null


func _longitude_error(a: float, b: float) -> float:
	return absf(fposmod(a - b + 180.0, 360.0) - 180.0)


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("TERRAIN_VIEWPORT_PHASE46_UI_FAILED: %s" % message)
	get_tree().quit(1)
