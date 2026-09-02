extends Node
## Phase 44 UI regression through the actual PlanetStudio.tscn Surface path.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const GRAPH_EDITOR_PATH := "res://scripts/world_authoring/terrain_graph_editor_phase44.gd"
const PRODUCTION_SURFACE_SLOT_ID := "production-terrain-surface"

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase44_surface_ui_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	add_child(world)
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
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "SHADERS")
	await _frames(6)

	var surface_tab: Button = _find_button(editor, "SURFACE")
	if surface_tab == null:
		_fail("default Simple Terrain page has no Surface tab")
		return
	surface_tab.emit_signal("pressed")
	await _frames(6)

	var graph_editor: Node = _find_script_node(editor, GRAPH_EDITOR_PATH)
	if graph_editor == null:
		_fail("Surface tab did not open the Phase 44 graph editor")
		return
	var guided: Control = _find_named(graph_editor, "GuidedTerrainSurfaceEditor") as Control
	if guided == null or not guided.visible:
		_fail("production Surface did not default to guided Simple editing")
		return
	var mode_bar: Node = _find_named(graph_editor, "SurfaceEditingModeBar")
	var mode_picker: OptionButton = _first_option(mode_bar)
	if mode_picker == null or mode_picker.item_count != 3 \
			or mode_picker.get_item_text(0) != "Simple" \
			or mode_picker.get_item_text(1) != "Detailed" \
			or mode_picker.get_item_text(2) != "Node Graph":
		_fail("Surface Simple / Detailed / Node Graph modes are missing")
		return

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Planet Studio has no active terrain profile")
		return
	var surface_slot: Resource = terrain.call("find_shader_slot", PRODUCTION_SURFACE_SLOT_ID) as Resource
	var graph: Resource = surface_slot.get(&"graph") as Resource if surface_slot != null else null
	if graph == null:
		_fail("production Surface graph is missing")
		return

	# Drive a real Simple material-balance control.
	var rock_amount: SpinBox = _spin_for_label(guided, "Rock amount")
	var color_richness: SpinBox = _spin_for_label(guided, "Color richness")
	var scan_influence: SpinBox = _spin_for_label(guided, "Scanned texture influence")
	if rock_amount == null or color_richness == null or scan_influence == null:
		_fail("Surface Simple mode is missing material balance or visual controls")
		return
	rock_amount.emit_signal("value_changed", 1.65)
	color_richness.emit_signal("value_changed", 1.40)
	scan_influence.emit_signal("value_changed", 0.82)
	await _frames(2)
	if absf(float(_parameter(graph, "PRODUCTION_CLASSIFIER_SETTINGS", "rock_scale", 0.0)) - 1.65) > 0.001 \
			or absf(float(_parameter(graph, "PRODUCTION_CLASSIFIER_SETTINGS", "albedo_chroma", 0.0)) - 1.40) > 0.001 \
			or absf(float(_parameter(graph, "PRODUCTION_SCAN_PBR_SETTINGS", "transfer_strength", 0.0)) - 0.82) > 0.001:
		_fail("Surface Simple numeric controls did not update the production graph")
		return

	var microrelief: CheckButton = _find_check(guided, "Microrelief")
	var scanned_pbr: CheckButton = _find_check(guided, "Scanned ground PBR")
	if microrelief == null or scanned_pbr == null:
		_fail("Surface Simple mode is missing production PBR toggles")
		return
	microrelief.emit_signal("toggled", false)
	scanned_pbr.emit_signal("toggled", false)
	await _frames(2)
	if bool(_parameter(graph, "PRODUCTION_MICRORELIEF_SETTINGS", "enabled", true)) \
			or bool(_parameter(graph, "PRODUCTION_SCAN_PBR_SETTINGS", "enabled", true)):
		_fail("Surface Simple toggles did not update production settings nodes")
		return

	var grass_color: ColorPickerButton = _color_for_label(guided, "Grass")
	if grass_color == null:
		_fail("Surface Simple mode has no key Grass palette control")
		return
	var next_grass := Color(0.12, 0.31, 0.07)
	grass_color.emit_signal("color_changed", next_grass)
	await _frames(2)
	var actual_grass: Color = Color(_parameter(graph, "PRODUCTION_SURFACE_PALETTE", "vegetation_grass", Color.BLACK))
	if not actual_grass.is_equal_approx(next_grass):
		_fail("Surface palette color did not update the production graph")
		return

	# Detailed is a superset, not a separate document.
	mode_picker.select(1)
	mode_picker.emit_signal("item_selected", 1)
	await _frames(3)
	guided = _find_named(graph_editor, "GuidedTerrainSurfaceEditor") as Control
	var rock_slope: SpinBox = _spin_for_label(guided, "Rock starts on slope")
	var ground_size: SpinBox = _spin_for_label(guided, "Ground texture size")
	if rock_slope == null or ground_size == null \
			or rock_slope.suffix.strip_edges() != "°" \
			or ground_size.suffix.strip_edges() != "m":
		_fail("Surface Detailed mode is missing physical classifier/texture controls")
		return
	rock_slope.emit_signal("value_changed", 47.0)
	ground_size.emit_signal("value_changed", 3.5)
	await _frames(2)
	if absf(float(_parameter(graph, "PRODUCTION_CLASSIFIER_THRESHOLDS", "rock_slope_start_deg", 0.0)) - 47.0) > 0.001 \
			or absf(float(_parameter(graph, "PRODUCTION_SCAN_PBR_SETTINGS", "ground_metres", 0.0)) - 3.5) > 0.001:
		_fail("Surface Detailed controls did not edit the same production graph")
		return

	# Node Graph must reveal the exact same graph without conversion or reset.
	mode_picker.select(2)
	mode_picker.emit_signal("item_selected", 2)
	await _frames(3)
	var graph_edit: GraphEdit = _first_graph_edit(graph_editor)
	guided = _find_named(graph_editor, "GuidedTerrainSurfaceEditor") as Control
	if graph_edit == null or not graph_edit.visible or guided == null or guided.visible:
		_fail("Surface Node Graph mode did not reveal the inherited GraphEdit")
		return
	if absf(float(_parameter(graph, "PRODUCTION_CLASSIFIER_SETTINGS", "rock_scale", 0.0)) - 1.65) > 0.001 \
			or not Color(_parameter(graph, "PRODUCTION_SURFACE_PALETTE", "vegetation_grass", Color.BLACK)).is_equal_approx(next_grass):
		_fail("switching Surface views changed production graph values")
		return

	# Existing technical composer remains reachable from the top-level mode switch.
	var advanced: Button = _find_button(editor, "Advanced / Node System")
	if advanced == null:
		_fail("Surface Simple workflow has no Advanced escape hatch")
		return
	advanced.emit_signal("pressed")
	await _frames(6)
	if _find_label(editor, "Detail") == null:
		_fail("Advanced mode did not restore the existing LOD/scope composer")
		return

	print("SIMPLIFIED_SURFACE_PHASE44_UI_OK: Simple/Detailed Surface controls edit production classifier, palette and PBR nodes and round-trip to Node Graph")
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _parameter(graph: Resource, node_type: String, key: String, fallback: Variant) -> Variant:
	if graph == null:
		return fallback
	for value: Variant in graph.get(&"nodes") as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		if String(node.get("type", "")) != node_type:
			continue
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		return parameters.get(key, fallback)
	return fallback


func _find_script_node(root: Node, resource_path: String) -> Node:
	if root == null:
		return null
	var script: Script = root.get_script() as Script
	if script != null and script.resource_path == resource_path:
		return root
	for child: Node in root.get_children():
		var found: Node = _find_script_node(child, resource_path)
		if found != null:
			return found
	return null


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


func _find_label(root: Node, text: String) -> Label:
	if root == null:
		return null
	if root is Label and (root as Label).text == text:
		return root as Label
	for child: Node in root.get_children():
		var found: Label = _find_label(child, text)
		if found != null:
			return found
	return null


func _spin_for_label(root: Node, label_text: String) -> SpinBox:
	if root == null:
		return null
	for child: Node in root.get_children():
		if child is HBoxContainer:
			var matched: bool = false
			var spin: SpinBox = null
			for row_child: Node in child.get_children():
				if row_child is Label and (row_child as Label).text == label_text:
					matched = true
				elif row_child is SpinBox:
					spin = row_child as SpinBox
			if matched and spin != null:
				return spin
		var nested: SpinBox = _spin_for_label(child, label_text)
		if nested != null:
			return nested
	return null


func _color_for_label(root: Node, label_text: String) -> ColorPickerButton:
	if root == null:
		return null
	for child: Node in root.get_children():
		if child is HBoxContainer:
			var matched: bool = false
			var picker: ColorPickerButton = null
			for row_child: Node in child.get_children():
				if row_child is Label and (row_child as Label).text == label_text:
					matched = true
				elif row_child is ColorPickerButton:
					picker = row_child as ColorPickerButton
			if matched and picker != null:
				return picker
		var nested: ColorPickerButton = _color_for_label(child, label_text)
		if nested != null:
			return nested
	return null


func _find_check(root: Node, text: String) -> CheckButton:
	if root == null:
		return null
	if root is CheckButton and (root as CheckButton).text == text:
		return root as CheckButton
	for child: Node in root.get_children():
		var found: CheckButton = _find_check(child, text)
		if found != null:
			return found
	return null


func _first_option(root: Node) -> OptionButton:
	if root == null:
		return null
	if root is OptionButton:
		return root as OptionButton
	for child: Node in root.get_children():
		var found: OptionButton = _first_option(child)
		if found != null:
			return found
	return null


func _first_graph_edit(root: Node) -> GraphEdit:
	if root == null:
		return null
	if root is GraphEdit:
		return root as GraphEdit
	for child: Node in root.get_children():
		var found: GraphEdit = _first_graph_edit(child)
		if found != null:
			return found
	return null


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("SIMPLIFIED_SURFACE_PHASE44_UI_FAILED: %s" % message)
	get_tree().quit(1)
