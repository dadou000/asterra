extends Node
## Phase 43 UI regression through the actual PlanetStudio.tscn path.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const GRAPH_EDITOR_PATH := "res://scripts/world_authoring/terrain_graph_editor_phase43.gd"

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase43_simple_ui_recovery_%d.tres" % OS.get_process_id())
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

	# The default terrain page must be Simple and show the existing Base Landscape
	# beginner panel, not the technical L0/L1 scope composer.
	var mode_bar: Node = _find_named(editor, "TerrainAuthoringModeBar")
	if mode_bar == null:
		_fail("default Terrain page has no Simple / Advanced mode bar")
		return
	if _find_button(editor, "BASE LANDSCAPE") == null or _find_button(editor, "LOCAL FEATURES") == null:
		_fail("Simple Terrain page is missing Base Landscape / Local Features tabs")
		return
	var base_guided: Node = _find_named(editor, "GuidedTerrainEditor")
	if base_guided == null or not (base_guided as Control).visible:
		_fail("Base Landscape did not open the existing simplified geomorph editor")
		return
	if _find_label(editor, "Detail") != null:
		_fail("Simple Terrain page leaked the technical LOD target bar")
		return

	var local_tab: Button = _find_button(editor, "LOCAL FEATURES")
	local_tab.emit_signal("pressed")
	await _frames(6)
	if _find_named(editor, "SimpleTerrainAllDistanceNotice") == null \
			or _find_named(editor, "SimpleTerrainFeatureCapacity") == null:
		_fail("Simple Local Features page does not explain all-distance application and capacity")
		return
	var add_feature: Button = _find_button(editor, "+ Add Terrain Feature")
	if add_feature == null:
		_fail("Local Features page has no guided feature creation action")
		return
	add_feature.emit_signal("pressed")
	await _frames(7)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Planet Studio has no active terrain profile")
		return
	var feature: Resource = _first_feature(terrain)
	if feature == null:
		_fail("Simple Add Terrain Feature did not create a displacement slot")
		return
	if int(feature.get(&"clipmap_level_mask")) != ((1 << 15) - 1):
		_fail("new Simple feature does not cover all terrain LODs")
		return
	var graph: Resource = feature.get(&"graph") as Resource
	if not GUIDED.is_guided_graph(graph):
		_fail("new Simple feature was not serialized as an ordinary guided node graph")
		return
	var config: Dictionary = GUIDED.config_from_graph(graph)
	if String(config.get("effect_kind", "")) != GUIDED.EFFECT_HEIGHT \
			or String(config.get("area_kind", "")) != GUIDED.AREA_RADIAL:
		_fail("new Simple feature defaults changed")
		return

	var graph_editor: Node = _find_script_node(editor, GRAPH_EDITOR_PATH)
	if graph_editor == null:
		_fail("Local Feature did not open the Phase 43 graph editor")
		return
	var feature_panel: Control = _find_named(graph_editor, "GuidedTerrainFeatureEditor") as Control
	if feature_panel == null or not feature_panel.visible:
		_fail("guided feature did not default to Simple feature editing")
		return

	# Drive effect and area changes through actual OptionButton signals. The graph
	# must rebuild as ordinary nodes while the UI remains in Simple mode.
	var effect_picker: OptionButton = _option_for_label(feature_panel, "Effect")
	var area_picker: OptionButton = _option_for_label(feature_panel, "Area")
	if effect_picker == null or area_picker == null:
		_fail("guided feature is missing Effect or Area picker")
		return
	var mountain_index: int = _metadata_index(effect_picker, GUIDED.EFFECT_MOUNTAINS)
	if mountain_index < 0:
		_fail("Effect picker does not expose Ridged Mountains")
		return
	effect_picker.select(mountain_index)
	effect_picker.emit_signal("item_selected", mountain_index)
	await _frames(5)
	config = GUIDED.config_from_graph(graph)
	if String(config.get("effect_kind", "")) != GUIDED.EFFECT_MOUNTAINS:
		_fail("Simple Effect picker did not update the serialized graph")
		return

	graph_editor = _find_script_node(editor, GRAPH_EDITOR_PATH)
	feature_panel = _find_named(graph_editor, "GuidedTerrainFeatureEditor") as Control
	area_picker = _option_for_label(feature_panel, "Area")
	var ring_index: int = _metadata_index(area_picker, GUIDED.AREA_RING)
	if ring_index < 0:
		_fail("Area picker does not expose Ring Area")
		return
	area_picker.select(ring_index)
	area_picker.emit_signal("item_selected", ring_index)
	await _frames(5)
	config = GUIDED.config_from_graph(graph)
	if String(config.get("area_kind", "")) != GUIDED.AREA_RING:
		_fail("Simple Area picker did not update the serialized graph")
		return

	graph_editor = _find_script_node(editor, GRAPH_EDITOR_PATH)
	feature_panel = _find_named(graph_editor, "GuidedTerrainFeatureEditor") as Control
	var inner: SpinBox = _spin_for_label(feature_panel, "Inner radius")
	var outer: SpinBox = _spin_for_label(feature_panel, "Outer radius")
	if inner == null or outer == null or inner.suffix != "°" or outer.suffix != "°":
		_fail("Ring Area simplified controls are missing or lack degree units")
		return
	inner.emit_signal("value_changed", 12.0)
	outer.emit_signal("value_changed", 24.0)
	await _frames(2)
	config = GUIDED.config_from_graph(graph)
	if not is_equal_approx(float(config.get("inner_radius_deg", -1.0)), 12.0) \
			or not is_equal_approx(float(config.get("outer_radius_deg", -1.0)), 24.0):
		_fail("Ring Area simplified controls did not update the graph")
		return

	# Node Graph is a view switch, not a conversion. The guided marker and config
	# must survive when the exact generated topology is revealed.
	var feature_mode_bar: Node = _find_named(graph_editor, "GuidedFeatureModeBar")
	var feature_mode: OptionButton = _first_option(feature_mode_bar)
	if feature_mode == null:
		_fail("guided feature has no Simple / Node Graph switch")
		return
	feature_mode.select(1)
	feature_mode.emit_signal("item_selected", 1)
	await _frames(2)
	var graph_edit: GraphEdit = _first_graph_edit(graph_editor)
	feature_panel = _find_named(graph_editor, "GuidedTerrainFeatureEditor") as Control
	if graph_edit == null or not graph_edit.visible or feature_panel == null or feature_panel.visible:
		_fail("Node Graph switch did not reveal the exact guided topology")
		return
	if not GUIDED.is_guided_graph(graph) or String(GUIDED.config_from_graph(graph).get("area_kind", "")) != GUIDED.AREA_RING:
		_fail("switching to Node Graph altered guided feature data")
		return

	# The previous technical composer remains available rather than being deleted.
	var advanced: Button = _find_button(editor, "Advanced / Node System")
	if advanced == null:
		_fail("Simple Terrain page has no Advanced escape hatch")
		return
	advanced.emit_signal("pressed")
	await _frames(6)
	if _find_button(editor, "Simple") == null or _find_label(editor, "Detail") == null:
		_fail("Advanced mode did not restore the existing LOD/scope composer")
		return

	print("SIMPLIFIED_TERRAIN_PHASE43_UI_OK: default Simple page, Base Landscape, feature creation, effect/location controls, Ring Area and Node Graph round-trip match")
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _first_feature(terrain: Resource) -> Resource:
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = value as Resource
		if slot != null and String(slot.get(&"slot_id")) != NATIVE.PRODUCTION_SHAPE_SLOT_ID:
			return slot
	return null


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


func _option_for_label(root: Node, label_text: String) -> OptionButton:
	if root == null:
		return null
	for child: Node in root.get_children():
		if child is HBoxContainer:
			var matched: bool = false
			var option: OptionButton = null
			for row_child: Node in child.get_children():
				if row_child is Label and (row_child as Label).text == label_text:
					matched = true
				elif row_child is OptionButton:
					option = row_child as OptionButton
			if matched and option != null:
				return option
		var nested: OptionButton = _option_for_label(child, label_text)
		if nested != null:
			return nested
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


func _metadata_index(picker: OptionButton, value: String) -> int:
	if picker == null:
		return -1
	for index: int in picker.item_count:
		if String(picker.get_item_metadata(index)) == value:
			return index
	return -1


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
	print("SIMPLIFIED_TERRAIN_PHASE43_UI_FAILED: %s" % message)
	get_tree().quit(1)
