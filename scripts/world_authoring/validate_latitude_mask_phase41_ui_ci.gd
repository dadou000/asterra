extends Node
## Phase 41 UI regression for the actual Planet Studio activation path.
##
## A generic authored displacement flow must expose Latitude Band, create the exact
## serialized LATITUDE_MASK node, and route its controls through the normal staged
## graph-edit path. Resident Base Terrain must deliberately keep the option hidden
## until spatial native-stage lowering exists. Phase 45 is the current top-level
## Planet Studio wrapper; raw displacement graphs must still use exact Phase 41.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const EXPECTED_EDITOR_PATH := "res://scripts/world_authoring/world_authoring_editor_live_phase45.gd"
const GRAPH_EDITOR_PATH := "res://scripts/world_authoring/terrain_graph_editor_phase41.gd"
const LATITUDE_MASK_TYPE := "LATITUDE_MASK"


class DummyWorld extends Node3D:
	var player: Node = null


class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase41_ui_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	world.name = "Phase41UISmokeWorld"
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)

	var editor: Control = PLANET_STUDIO_SCENE.instantiate() as Control
	if editor == null:
		_fail("Actual PlanetStudio.tscn failed to instantiate.")
		return
	var editor_script: Script = editor.get_script() as Script
	if editor_script == null or editor_script.resource_path != EXPECTED_EDITOR_PATH:
		_fail("PlanetStudio.tscn is not using the current Phase 45 live editor wrapper.")
		return
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "SHADERS")
	await _frames(5)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Phase 45 Planet Studio has no active terrain profile.")
		return
	var shape_slot: Resource = terrain.call("find_shader_slot",
		NATIVE.PRODUCTION_SHAPE_SLOT_ID) as Resource
	if shape_slot == null:
		_fail("Phase 45 Planet Studio has no resident Base Terrain Shape slot.")
		return

	# Create an ordinary authored displacement layer. This is the bytecode path where
	# spatial masks are exact on GPU and CPU/contact, so the UI may expose the node.
	var custom_slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Latitude UI") as Resource
	if custom_slot == null:
		_fail("Could not create a generic authored displacement layer.")
		return
	custom_slot.set(&"enabled", true)
	custom_slot.set(&"clipmap_level_mask", (1 << 15) - 1)
	custom_slot.set(&"biome_mask_mode", SLOT.BiomeMaskMode.ALL)
	custom_slot.set(&"biome_ids", PackedInt32Array())
	custom_slot.set(&"blend_mode", SLOT.BlendMode.ADD)

	editor.call("_phase28_focus_existing_slot", custom_slot)
	await _frames(5)
	var graph_editor: Node = _find_script_node(editor, GRAPH_EDITOR_PATH)
	if graph_editor == null:
		_fail("Generic displacement layer did not open the Phase 41 graph editor.")
		return
	var picker: OptionButton = graph_editor.get("_node_type_picker") as OptionButton
	var latitude_index: int = _picker_index(picker, LATITUDE_MASK_TYPE)
	if latitude_index < 0:
		_fail("Generic displacement picker is missing Mask · Latitude Band.")
		return
	if picker.get_item_text(latitude_index) != "Mask  ·  Latitude Band":
		_fail("Latitude Mask picker entry is not using the beginner-facing label.")
		return

	picker.select(latitude_index)
	graph_editor.call("_on_add_node")
	await _frames(5)
	var graph: Resource = custom_slot.get(&"graph") as Resource
	var latitude_id: String = _node_id_by_type(graph, LATITUDE_MASK_TYPE)
	if latitude_id.is_empty():
		_fail("Add Node did not serialize a LATITUDE_MASK node into the authored graph.")
		return
	var parameters: Dictionary = _node_parameters(graph, latitude_id)
	if not is_equal_approx(float(parameters.get("south_deg", 999.0)), -30.0) \
			or not is_equal_approx(float(parameters.get("north_deg", 999.0)), 30.0) \
			or not is_equal_approx(float(parameters.get("feather_deg", 999.0)), 5.0) \
			or bool(parameters.get("invert", true)):
		_fail("New Latitude Mask did not use the documented beginner-safe defaults.")
		return

	graph_editor = _find_script_node(editor, GRAPH_EDITOR_PATH)
	var mask_node: GraphNode = _find_graph_node(graph_editor, "LATITUDE MASK")
	if mask_node == null:
		_fail("Serialized Latitude Mask was not rendered as a graph node.")
		return
	var south_spin: SpinBox = _spin_for_label(mask_node, "South edge")
	var north_spin: SpinBox = _spin_for_label(mask_node, "North edge")
	var feather_spin: SpinBox = _spin_for_label(mask_node, "Feather")
	if south_spin == null or north_spin == null or feather_spin == null:
		_fail("Latitude Mask is missing one or more degree controls.")
		return
	if south_spin.suffix != "°" or north_spin.suffix != "°" or feather_spin.suffix != "°":
		_fail("Latitude Mask controls are not presented in degrees.")
		return

	# Exercise the actual UI signal path rather than mutating the graph Resource
	# directly. The normal staged action must update the same serialized node.
	south_spin.emit_signal("value_changed", -45.0)
	feather_spin.emit_signal("value_changed", 12.5)
	var invert: CheckButton = _find_invert_button(mask_node)
	if invert == null:
		_fail("Latitude Mask is missing its beginner-facing Invert control.")
		return
	invert.emit_signal("toggled", true)
	await _frames(2)
	parameters = _node_parameters(graph, latitude_id)
	if not is_equal_approx(float(parameters.get("south_deg", 0.0)), -45.0) \
			or not is_equal_approx(float(parameters.get("feather_deg", 0.0)), 12.5) \
			or not bool(parameters.get("invert", false)):
		_fail("Latitude Mask UI controls did not update the staged graph parameters.")
		return

	# Base Terrain remains resident coefficient-lowered. Until a spatial stage
	# provenance runtime exists, showing Latitude Band here would be a false promise.
	editor.call("_phase28_focus_existing_slot", shape_slot)
	await _frames(5)
	var base_editor: Node = _find_script_node(editor, GRAPH_EDITOR_PATH)
	if base_editor == null:
		_fail("Base Terrain did not reopen the Phase 41 graph editor.")
		return
	var base_picker: OptionButton = base_editor.get("_node_type_picker") as OptionButton
	if _picker_index(base_picker, LATITUDE_MASK_TYPE) >= 0:
		_fail("Resident Base Terrain incorrectly exposes Latitude Band before native spatial lowering exists.")
		return

	print("LATITUDE_MASK_PHASE41_UI_OK: Phase 45 routes generic displacement through exact Phase 41; Latitude Band edits correctly and resident Base Terrain keeps unsupported spatial masking hidden")
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


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


func _picker_index(picker: OptionButton, metadata_value: String) -> int:
	if picker == null:
		return -1
	for index: int in picker.item_count:
		if String(picker.get_item_metadata(index)) == metadata_value:
			return index
	return -1


func _node_id_by_type(graph: Resource, node_type: String) -> String:
	if graph == null:
		return ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""


func _node_parameters(graph: Resource, node_id: String) -> Dictionary:
	if graph == null:
		return {}
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("id", "")) == node_id:
			return (node.get("parameters", {}) as Dictionary).duplicate(true)
	return {}


func _find_graph_node(root: Node, title: String) -> GraphNode:
	if root == null:
		return null
	for node_value: Node in root.find_children("*", "GraphNode", true, false):
		var graph_node := node_value as GraphNode
		if graph_node != null and graph_node.title == title:
			return graph_node
	return null


func _spin_for_label(node: GraphNode, label_text: String) -> SpinBox:
	if node == null:
		return null
	for row_value: Node in node.get_children():
		var row := row_value as HBoxContainer
		if row == null:
			continue
		var matched: bool = false
		var spin: SpinBox = null
		for child: Node in row.get_children():
			if child is Label and (child as Label).text == label_text:
				matched = true
			elif child is SpinBox:
				spin = child as SpinBox
		if matched and spin != null:
			return spin
	return null


func _find_invert_button(node: GraphNode) -> CheckButton:
	if node == null:
		return null
	for child: Node in node.get_children():
		var button := child as CheckButton
		if button != null and button.text.begins_with("Invert"):
			return button
	return null


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("LATITUDE_MASK_PHASE41_UI_FAILED: %s" % message)
	get_tree().quit(1)
