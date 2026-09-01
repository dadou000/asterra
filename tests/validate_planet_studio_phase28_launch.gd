extends Node
## Runtime smoke test for the exact Phase 28 construction path that static parser
## validation cannot cover. This is launched through a .tscn so project autoload
## globals resolve exactly as they do in the game.

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase28.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	# Phase 25's live runtime-shader owner resolver reads this from the editor's
	# CanvasLayer parent host. Keep the smoke hierarchy identical to the game.
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase28_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	world.name = "Phase28SmokeWorld"
	add_child(world)

	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)

	var editor: Control = EDITOR_SCRIPT.new() as Control
	if editor == null:
		_fail("Phase 28 editor failed to instantiate.")
		return
	editor.name = "PlanetStudioLive"
	editor.call("bind_world", world)
	layer.add_child(editor)

	# Phase 21 deliberately defers full/category rebuilds. Give both _ready() and
	# those deferred calls time to drain before entering the shader lab.
	await get_tree().process_frame
	await get_tree().process_frame
	await get_tree().process_frame
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 28 editor left the tree during launch.")
		return

	# Seed graphs outside the editor's default Global/L0 target. The shader library
	# must make them discoverable and navigate to their exact scope before building
	# TerrainGraphEditor and its GraphEdit/GraphNodes.
	var session: Object = editor.get("_session") as Object
	if session == null or not session.has_method("active_terrain_profile"):
		_fail("Phase 28 editor has no authoring session.")
		return
	var terrain: Resource = session.call("active_terrain_profile") as Resource
	if terrain == null:
		_fail("Phase 28 smoke world has no terrain profile.")
		return
	var slot: Resource = terrain.call("create_shader_slot", 0, "Scoped smoke displacement") as Resource
	if slot == null:
		_fail("Phase 28 failed to create smoke displacement graph.")
		return
	slot.set(&"clipmap_level_mask", 1)
	slot.set(&"biome_mask_mode", 1)
	slot.set(&"biome_ids", PackedInt32Array([9]))
	var material_slot: Resource = terrain.call("create_shader_slot", 1, "Scoped smoke texture") as Resource
	if material_slot == null:
		_fail("Phase 28 failed to create smoke material graph.")
		return
	material_slot.set(&"clipmap_level_mask", 1 << 3)
	material_slot.set(&"biome_mask_mode", 1)
	material_slot.set(&"biome_ids", PackedInt32Array([4]))

	editor.call("_show_category", "SHADERS")
	await get_tree().process_frame
	await get_tree().process_frame
	await get_tree().process_frame
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 28 editor left the tree while opening SHADERS.")
		return
	if not _has_control_text_containing(editor, "Scoped smoke displacement") \
			or not _has_control_text_containing(editor, "Scoped smoke texture"):
		_fail("Phase 28 authored shader library omitted a graph outside the current target.")
		return

	editor.call("_phase28_focus_existing_slot", slot)
	await get_tree().process_frame
	await get_tree().process_frame
	var graph_edits: Array[Node] = editor.find_children("*", "GraphEdit", true, false)
	if graph_edits.is_empty():
		_fail("Phase 28 could not navigate to the scoped displacement GraphEdit.")
		return
	if int(editor.get("_phase28_biome_id")) != 9 or int(editor.get("_phase28_domain")) != 0:
		_fail("Phase 28 displacement library navigation selected the wrong target.")
		return
	var quick_operations: Array[Dictionary] = [
		{"button":"+ Noise Layer", "type":"NOISE_LAYER"},
		{"button":"+ Ridged Mountains", "type":"RIDGED_MOUNTAINS"},
		{"button":"+ Erosion Channels", "type":"EROSION_CHANNELS"},
		{"button":"+ Sediment Deposit", "type":"SEDIMENT_DEPOSIT"},
	]
	for operation: Dictionary in quick_operations:
		var operation_button: Button = _find_button(editor, String(operation["button"]))
		if operation_button == null:
			_fail("Phase 28 live terrain operation shelf is missing %s." % String(operation["button"]))
			return
		operation_button.pressed.emit()
		await get_tree().process_frame
		await get_tree().process_frame
	var operation_types: Dictionary = {}
	var displacement_graph: Resource = slot.get(&"graph") as Resource
	for node_value: Variant in displacement_graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		operation_types[String(node_data.get("type", ""))] = true
	for operation: Dictionary in quick_operations:
		if not operation_types.has(String(operation["type"])):
			_fail("Phase 28 one-click %s did not modify the live graph." % String(operation["button"]))
			return
	if (displacement_graph.get(&"links") as Array).size() != quick_operations.size():
		_fail("Phase 28 quick operations did not auto-connect into one terrain flow.")
		return
	if _find_graph_node(editor, "PRODUCTION TERRAIN (ALWAYS PRESENT)") == null:
		_fail("Phase 28 displacement canvas did not expose the production terrain base.")
		return
	var ridged_node: GraphNode = _find_graph_node(editor, "Ridged Mountains")
	var delete_button: Button = _find_button(ridged_node, "Delete this node") if ridged_node != null else null
	if delete_button == null:
		_fail("Phase 28 authored terrain node has no visible delete action.")
		return
	delete_button.pressed.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	operation_types.clear()
	for node_value: Variant in displacement_graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		operation_types[String(node_data.get("type", ""))] = true
	if operation_types.has("RIDGED_MOUNTAINS"):
		_fail("Phase 28 visible node delete action did not remove the selected operation.")
		return
	if (displacement_graph.get(&"links") as Array).size() != quick_operations.size() - 1:
		_fail("Phase 28 node deletion did not preserve the surrounding terrain flow.")
		return

	editor.call("_phase28_focus_existing_slot", material_slot)
	await get_tree().process_frame
	await get_tree().process_frame
	graph_edits = editor.find_children("*", "GraphEdit", true, false)
	if graph_edits.is_empty():
		_fail("Phase 28 could not navigate to the scoped texture/material GraphEdit.")
		return
	if int(editor.get("_shader_composer_level")) != 3 \
			or int(editor.get("_phase28_biome_id")) != 4 \
			or int(editor.get("_phase28_domain")) != 1:
		_fail("Phase 28 material library navigation selected the wrong target.")
		return
	if _find_graph_node(editor, "PRODUCTION TEXTURES / PBR (ALWAYS PRESENT)") == null:
		_fail("Phase 28 material canvas did not expose the production texture/PBR base.")
		return

	print("PLANET_STUDIO_PHASE28_LAUNCH_OK: editor and GraphEdit alive")
	editor.queue_free()
	layer.queue_free()
	host.queue_free()
	world.queue_free()
	await get_tree().process_frame
	await get_tree().process_frame
	get_tree().quit(0)


func _fail(message: String) -> void:
	push_error(message)
	print("PLANET_STUDIO_PHASE28_LAUNCH_FAILED: %s" % message)
	get_tree().quit(1)


func _has_control_text_containing(root: Node, text: String) -> bool:
	for button_value: Node in root.find_children("*", "Button", true, false):
		var button := button_value as Button
		if button != null and button.text.contains(text):
			return true
	for picker_value: Node in root.find_children("*", "OptionButton", true, false):
		var picker := picker_value as OptionButton
		if picker == null:
			continue
		for item_index: int in picker.item_count:
			if picker.get_item_text(item_index).contains(text):
				return true
	return false


func _find_button(root: Node, text: String) -> Button:
	for button_value: Node in root.find_children("*", "Button", true, false):
		var button := button_value as Button
		if button != null and button.text == text:
			return button
	return null


func _find_graph_node(root: Node, title: String) -> GraphNode:
	for node_value: Node in root.find_children("*", "GraphNode", true, false):
		var graph_node := node_value as GraphNode
		if graph_node != null and graph_node.title == title:
			return graph_node
	return null
