extends Node
## Phase 41 UI regression for Geographic Region in the real Planet Studio path.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const GRAPH_EDITOR_PATH := "res://scripts/world_authoring/terrain_graph_editor_phase41.gd"
const REGION_PICKER_TYPE := "GEOGRAPHIC_REGION_MASK"

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase41_region_ui_recovery_%d.tres" % OS.get_process_id())
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
	await _frames(5)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Planet Studio has no active terrain profile")
		return
	var base_slot: Resource = terrain.call("find_shader_slot", NATIVE.PRODUCTION_SHAPE_SLOT_ID) as Resource
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Geographic Region UI") as Resource
	if slot == null or base_slot == null:
		_fail("required displacement slots are missing")
		return
	slot.set(&"enabled", true)
	editor.call("_phase28_focus_existing_slot", slot)
	await _frames(5)

	var graph_editor: Node = _find_script_node(editor, GRAPH_EDITOR_PATH)
	if graph_editor == null:
		_fail("generic displacement did not open Phase 41 graph editor")
		return
	var picker: OptionButton = graph_editor.get("_node_type_picker") as OptionButton
	var region_index: int = _picker_index(picker, REGION_PICKER_TYPE)
	if region_index < 0 or picker.get_item_text(region_index) != "Mask  ·  Geographic Region":
		_fail("generic displacement picker is missing Geographic Region")
		return
	picker.select(region_index)
	graph_editor.call("_on_add_node")
	await _frames(5)

	var graph: Resource = slot.get(&"graph") as Resource
	var region_id: String = _region_node_id(graph)
	if region_id.is_empty():
		_fail("Geographic Region did not serialize as LATITUDE_MASK axis=region")
		return
	var parameters: Dictionary = _node_parameters(graph, region_id)
	if not is_equal_approx(float(parameters.get("south_deg", 999.0)), -30.0) \
			or not is_equal_approx(float(parameters.get("north_deg", 999.0)), 30.0) \
			or not is_equal_approx(float(parameters.get("feather_deg", 999.0)), 5.0) \
			or not is_equal_approx(float(parameters.get("west_deg", 999.0)), -45.0) \
			or not is_equal_approx(float(parameters.get("east_deg", 999.0)), 45.0) \
			or not is_equal_approx(float(parameters.get("longitude_feather_deg", 999.0)), 5.0) \
			or bool(parameters.get("invert", true)):
		_fail("Geographic Region defaults changed")
		return

	graph_editor = _find_script_node(editor, GRAPH_EDITOR_PATH)
	var region_node: GraphNode = _find_graph_node(graph_editor, "GEOGRAPHIC REGION MASK")
	if region_node == null:
		_fail("serialized region variant was not rendered as GEOGRAPHIC REGION MASK")
		return
	var south: SpinBox = _spin_for_label(region_node, "South edge")
	var north: SpinBox = _spin_for_label(region_node, "North edge")
	var lat_feather: SpinBox = _spin_for_label(region_node, "Latitude feather")
	var west: SpinBox = _spin_for_label(region_node, "West edge")
	var east: SpinBox = _spin_for_label(region_node, "East edge")
	var lon_feather: SpinBox = _spin_for_label(region_node, "Longitude feather")
	if south == null or north == null or lat_feather == null \
			or west == null or east == null or lon_feather == null:
		_fail("Geographic Region is missing one or more degree controls")
		return
	for spin: SpinBox in [south, north, lat_feather, west, east, lon_feather]:
		if spin.suffix != "°":
			_fail("Geographic Region contains a control without degree units")
			return

	# Use a seam-crossing region through the actual staged UI signal path.
	south.emit_signal("value_changed", -20.0)
	north.emit_signal("value_changed", 25.0)
	lat_feather.emit_signal("value_changed", 7.5)
	west.emit_signal("value_changed", 170.0)
	east.emit_signal("value_changed", -170.0)
	lon_feather.emit_signal("value_changed", 12.5)
	var invert: CheckButton = _find_invert(region_node)
	if invert == null:
		_fail("Geographic Region invert control is missing")
		return
	invert.emit_signal("toggled", true)
	await _frames(2)
	parameters = _node_parameters(graph, region_id)
	if not is_equal_approx(float(parameters.get("south_deg", 0.0)), -20.0) \
			or not is_equal_approx(float(parameters.get("north_deg", 0.0)), 25.0) \
			or not is_equal_approx(float(parameters.get("feather_deg", 0.0)), 7.5) \
			or not is_equal_approx(float(parameters.get("west_deg", 0.0)), 170.0) \
			or not is_equal_approx(float(parameters.get("east_deg", 0.0)), -170.0) \
			or not is_equal_approx(float(parameters.get("longitude_feather_deg", 0.0)), 12.5) \
			or not bool(parameters.get("invert", false)):
		_fail("Geographic Region controls did not update the staged graph")
		return

	editor.call("_phase28_focus_existing_slot", base_slot)
	await _frames(5)
	var base_editor: Node = _find_script_node(editor, GRAPH_EDITOR_PATH)
	var base_picker: OptionButton = base_editor.get("_node_type_picker") as OptionButton if base_editor != null else null
	if _picker_index(base_picker, REGION_PICKER_TYPE) >= 0:
		_fail("resident Base Terrain incorrectly exposes Geographic Region")
		return

	print("GEOGRAPHIC_REGION_MASK_PHASE41_UI_OK: picker, canonical serialization, six staged degree controls, inversion and Base Terrain safety boundary match")
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


func _region_node_id(graph: Resource) -> String:
	if graph == null:
		return ""
	for value: Variant in graph.get(&"nodes") as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		if String(node.get("type", "")) == "LATITUDE_MASK" \
				and String(parameters.get("axis", "latitude")) == "region":
			return String(node.get("id", ""))
	return ""


func _node_parameters(graph: Resource, node_id: String) -> Dictionary:
	for value: Variant in graph.get(&"nodes") as Array:
		if value is Dictionary and String((value as Dictionary).get("id", "")) == node_id:
			return ((value as Dictionary).get("parameters", {}) as Dictionary).duplicate(true)
	return {}


func _find_graph_node(root: Node, title: String) -> GraphNode:
	if root == null:
		return null
	for value: Node in root.find_children("*", "GraphNode", true, false):
		var node := value as GraphNode
		if node != null and node.title == title:
			return node
	return null


func _spin_for_label(node: GraphNode, label_text: String) -> SpinBox:
	for row_value: Node in node.get_children():
		var row := row_value as HBoxContainer
		if row == null:
			continue
		var matched := false
		var spin: SpinBox = null
		for child: Node in row.get_children():
			if child is Label and (child as Label).text == label_text:
				matched = true
			elif child is SpinBox:
				spin = child as SpinBox
		if matched:
			return spin
	return null


func _find_invert(node: GraphNode) -> CheckButton:
	for child: Node in node.get_children():
		if child is CheckButton and (child as CheckButton).text.begins_with("Invert"):
			return child as CheckButton
	return null


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("GEOGRAPHIC_REGION_MASK_PHASE41_UI_FAILED: %s" % message)
	get_tree().quit(1)
