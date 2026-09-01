extends SceneTree
## Rendered launch harness for the real Planet Studio path. Unlike the isolated
## UI smoke scene, this loads Main with its production terrain and renderer.

const MAIN_SCENE := "res://scenes/Main.tscn"
const SUCCESS_AFTER_SECONDS := 20.0


func _initialize() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/full_launch_recovery_%d.tres" % OS.get_process_id())
	var mode := "planet_studio"
	var user_args := OS.get_cmdline_user_args()
	if not user_args.is_empty():
		mode = user_args[0]
	set_meta("launch_mode", mode)
	set_meta("validate_authored_graph", user_args.has("active_graph"))
	var error := change_scene_to_file(MAIN_SCENE)
	if error != OK:
		push_error("Could not launch Planet Studio Main scene: %s" % error_string(error))
		quit(1)
		return
	_run_until_stable.call_deferred()


func _run_until_stable() -> void:
	if bool(get_meta("validate_authored_graph", false)):
		if not await _seed_authored_graph():
			quit(1)
			return
	await create_timer(SUCCESS_AFTER_SECONDS).timeout
	var editor := root.find_child("PlanetStudioLive", true, false)
	var mode := String(get_meta("launch_mode", "planet_studio"))
	if mode == "planet_studio" and (editor == null or not editor.is_inside_tree()):
		push_error("Planet Studio editor was not alive after rendered launch.")
		quit(1)
		return
	print("PLANET_STUDIO_FULL_LAUNCH_OK: production renderer remained stable")
	quit(0)


func _seed_authored_graph() -> bool:
	var editor: Node = null
	for _frame: int in 600:
		editor = root.find_child("PlanetStudioLive", true, false)
		if editor != null:
			break
		await process_frame
	if editor == null:
		push_error("Planet Studio editor never became available for authored graph test.")
		return false
	var session: Object = editor.get("_session") as Object
	if session == null or not session.has_method("active_terrain_profile"):
		push_error("Planet Studio session was unavailable for authored graph test.")
		return false
	var terrain: Resource = session.call("active_terrain_profile") as Resource
	var slot: Resource = terrain.call("create_shader_slot", 0,
		"Rendered launch displacement") as Resource
	if slot == null:
		push_error("Could not create rendered launch displacement graph.")
		return false
	slot.set(&"clipmap_level_mask", 1)
	if not _make_live_terrain_flow(slot):
		push_error("Could not build rendered noise/erosion/sediment terrain flow.")
		return false
	var material_slot: Resource = terrain.call("create_shader_slot", 1,
		"Rendered launch material") as Resource
	if material_slot == null:
		push_error("Could not create rendered launch material graph.")
		return false
	material_slot.set(&"clipmap_level_mask", 1)
	editor.call("_phase28_make_material_passthrough", material_slot)
	# Exercise the real rendered composer and the now-prominent combined production
	# displacement/material source editor, not only the shader runtime in isolation.
	editor.set("_phase28_show_advanced", true)
	var source_expanded: Dictionary = editor.get("_live_shader_source_expanded") as Dictionary
	source_expanded["terrain_ground"] = true
	editor.set("_live_shader_source_expanded", source_expanded)
	editor.call("_show_category", "SHADERS")
	for _frame: int in 5:
		await process_frame
	var graph_edits: Array[Node] = editor.find_children("*", "GraphEdit", true, false)
	var source_editors: Array[Node] = editor.find_children("*", "CodeEdit", true, false)
	if graph_edits.is_empty() or source_editors.is_empty():
		push_error("Rendered Shader Composer did not expose both authored graphs and production source.")
		return false
	print("PLANET_STUDIO_FULL_LAUNCH_GRAPH_AND_SOURCE_ACTIVE")
	return true


func _make_live_terrain_flow(slot: Resource) -> bool:
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return false
	var output_id: String = ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		if String(node_data.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = String(node_data.get("id", ""))
			break
	if output_id.is_empty():
		return false
	var previous_id: String = ""
	var operations: Array[String] = [
		"NOISE_LAYER", "RIDGED_MOUNTAINS", "EROSION_CHANNELS", "SEDIMENT_DEPOSIT"]
	for index: int in operations.size():
		var operation_id: String = String(graph.call("add_node", operations[index],
			Vector2(120.0 + index * 240.0, 180.0), {
				"scale": 5.0 + index * 2.0,
				"amount": 18.0 + index * 6.0,
				"passes": 3,
				"seed": 4200 + index * 31,
			}))
		if not previous_id.is_empty():
			graph.call("connect_nodes", previous_id, 0, operation_id, 0)
		previous_id = operation_id
	return bool(graph.call("connect_nodes", previous_id, 0, output_id, 0))
