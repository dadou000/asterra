extends "res://scripts/world_authoring/terrain_graph_editor_phase40.gd"
## Phase 41 graph presentation for deterministic planet-space latitude masks.
##
## Latitude Mask is exposed only on generic authored displacement graphs. Base
## Terrain native stages are still resident shader contributions lowered to scalar
## coefficients, so presenting a spatial mask there would imply support that does
## not exist yet. Saved unsupported native topology remains visible but is rejected
## transactionally by the Phase 40 lowering/runtime.

const LATITUDE_MASK_TYPE := "LATITUDE_MASK"


func _build_ui() -> void:
	super._build_ui()
	_sync_phase41_latitude_picker_entry()
	if _status != null and _latitude_mask_available_here():
		_status.text = "Live authored displacement: Latitude Mask uses planet-space degrees and the same CPU/contact + GPU bytecode definition. Invalid edits keep the last valid terrain active."


func _create_graph_node(node_data: Dictionary) -> void:
	if String(node_data.get("type", "")) == LATITUDE_MASK_TYPE:
		_create_latitude_mask_node(node_data)
		return
	super._create_graph_node(node_data)


func _on_add_node() -> void:
	if _node_type_picker == null or _node_type_picker.item_count == 0:
		return
	var node_type: String = String(_node_type_picker.get_item_metadata(
		_node_type_picker.selected))
	if node_type != LATITUDE_MASK_TYPE:
		super._on_add_node()
		return
	if not _latitude_mask_available_here() or _session == null or _graph == null:
		if _status != null:
			_status.text = "Latitude Mask is currently available for authored displacement flows, not resident Base Terrain stage composition."
		return
	var node_count: int = (_graph.get(&"nodes") as Array).size()
	var position := Vector2(
		180.0 + float(node_count % 4) * 180.0,
		120.0 + float(node_count / 4) * 120.0)
	_session.call("stage_action", "Add Latitude Mask", func() -> void:
		_append_latitude_mask_node(position)
	, 2)
	_request_rebuild()


func _sync_phase41_latitude_picker_entry() -> void:
	if _node_type_picker == null:
		return
	var existing_index: int = -1
	for index: int in _node_type_picker.item_count:
		if String(_node_type_picker.get_item_metadata(index)) == LATITUDE_MASK_TYPE:
			existing_index = index
			break

	# LATITUDE_MASK is now part of the canonical displacement schema, so inherited
	# picker construction can see it. Resident Base Terrain is deliberately one step
	# behind: remove the entry there until native stage provenance can carry a spatial
	# factor through render, warm cache and physical/contact evaluation identically.
	if not _latitude_mask_available_here():
		if existing_index >= 0:
			_node_type_picker.remove_item(existing_index)
		return

	if existing_index < 0:
		_node_type_picker.add_item("Masks  ·  Latitude Band")
		existing_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(existing_index, LATITUDE_MASK_TYPE)
	else:
		_node_type_picker.set_item_text(existing_index, "Masks  ·  Latitude Band")
	_node_type_picker.tooltip_text = "Latitude Band outputs 0 to 1 from normalized planet-space latitude. Use it with Multiply or Mix in an authored displacement flow."


func _latitude_mask_available_here() -> bool:
	return _graph != null and int(_graph.get(&"domain")) == GRAPH_SCRIPT.Domain.DISPLACEMENT \
		and not _is_native_branch_graph()


func _append_latitude_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	# Creation now goes through the same canonical schema path as every other node.
	# Defaults, IDs and revision accounting therefore survive clone/migration/catalog
	# operations without an editor-only raw-dictionary exception.
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {})


func _create_latitude_mask_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "LATITUDE MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	# Port zero is always the mask output. Parameter controls are ordinary children
	# below it and therefore cannot change the serialized graph port contract.
	_add_port_row(graph_node, "Mask 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_latitude_parameter(graph_node, node_id, "South edge", "south_deg",
		float(parameters.get("south_deg", -30.0)), -90.0, 90.0, 0.5)
	_add_latitude_parameter(graph_node, node_id, "North edge", "north_deg",
		float(parameters.get("north_deg", 30.0)), -90.0, 90.0, 0.5)
	_add_latitude_parameter(graph_node, node_id, "Feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 90.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the band"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 inside the latitude band. On: 1 outside the band."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "Latitude uses the planet itself: 0° equator, +90° north pole, −90° south pole. Feather fades smoothly outside each edge."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 275.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _add_latitude_parameter(node: GraphNode, node_id: String, label_text: String,
		key: String, value: float, minimum: float, maximum: float, step: float) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(275.0, 32.0)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 92.0
	row.add_child(label)
	var spin := SpinBox.new()
	spin.min_value = minimum
	spin.max_value = maximum
	spin.step = step
	spin.allow_lesser = false
	spin.allow_greater = false
	spin.suffix = "°"
	spin.value = clampf(value, minimum, maximum)
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spin.value_changed.connect(func(next_value: float) -> void:
		_set_node_parameter(node_id, key, next_value)
	)
	row.add_child(spin)
	node.add_child(row)
