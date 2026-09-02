extends "res://scripts/world_authoring/terrain_graph_editor_phase40.gd"
## Phase 41 graph presentation for deterministic planet-space band masks.
##
## Latitude/Longitude Bands are exposed only on generic authored displacement
## graphs. Base Terrain native stages are still resident shader contributions
## lowered to scalar coefficients, so presenting a spatial mask there would imply
## support that does not exist yet. Saved unsupported native topology remains
## visible but is rejected transactionally by the Phase 40 lowering/runtime.
##
## LONGITUDE_MASK is an editor-facing picker identity only. Serialized graphs keep
## the canonical LATITUDE_MASK node type and opt into longitude semantics with
## parameters.axis="longitude". Missing axis therefore remains byte-for-byte
## backward compatible with every Phase 41 latitude graph.

const LATITUDE_MASK_TYPE := "LATITUDE_MASK"
const LONGITUDE_MASK_PICKER_TYPE := "LONGITUDE_MASK"
const LATITUDE_MASK_PICKER_LABEL := "Mask  ·  Latitude Band"
const LONGITUDE_MASK_PICKER_LABEL := "Mask  ·  Longitude Band"


func _build_ui() -> void:
	super._build_ui()
	_sync_phase41_spatial_picker_entries()
	if _status != null and _spatial_mask_available_here():
		_status.text = "Live authored displacement: Latitude / Longitude Bands use normalized planet-space degrees and the same CPU/contact + GPU bytecode definition. Invalid edits keep the last valid terrain active."


func _create_graph_node(node_data: Dictionary) -> void:
	if String(node_data.get("type", "")) == LATITUDE_MASK_TYPE:
		var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
		if String(parameters.get("axis", "latitude")) == "longitude":
			_create_longitude_mask_node(node_data)
		else:
			_create_latitude_mask_node(node_data)
		return
	super._create_graph_node(node_data)


func _on_add_node() -> void:
	if _node_type_picker == null or _node_type_picker.item_count == 0:
		return
	var node_type: String = String(_node_type_picker.get_item_metadata(
		_node_type_picker.selected))
	if node_type != LATITUDE_MASK_TYPE and node_type != LONGITUDE_MASK_PICKER_TYPE:
		super._on_add_node()
		return
	if not _spatial_mask_available_here() or _session == null or _graph == null:
		if _status != null:
			_status.text = "Planet-space band masks are currently available for authored displacement flows, not resident Base Terrain stage composition."
		return
	var node_count: int = (_graph.get(&"nodes") as Array).size()
	var position := Vector2(
		180.0 + float(node_count % 4) * 180.0,
		120.0 + float(node_count / 4) * 120.0)
	if node_type == LONGITUDE_MASK_PICKER_TYPE:
		_session.call("stage_action", "Add Longitude Mask", func() -> void:
			_append_longitude_mask_node(position)
		, 2)
	else:
		_session.call("stage_action", "Add Latitude Mask", func() -> void:
			_append_latitude_mask_node(position)
		, 2)
	_request_rebuild()


func _sync_phase41_spatial_picker_entries() -> void:
	if _node_type_picker == null:
		return
	var latitude_index: int = _picker_index_for_metadata(LATITUDE_MASK_TYPE)
	var longitude_index: int = _picker_index_for_metadata(LONGITUDE_MASK_PICKER_TYPE)

	# LATITUDE_MASK is part of the canonical displacement schema. Longitude uses the
	# same serialized type with an axis parameter so old graph resources need no
	# migration. Resident Base Terrain deliberately stays one step behind until
	# native stage provenance can carry a spatial factor identically through render,
	# warm cache and physical/contact evaluation.
	if not _spatial_mask_available_here():
		if longitude_index >= 0:
			_node_type_picker.remove_item(longitude_index)
		latitude_index = _picker_index_for_metadata(LATITUDE_MASK_TYPE)
		if latitude_index >= 0:
			_node_type_picker.remove_item(latitude_index)
		return

	if latitude_index < 0:
		_node_type_picker.add_item(LATITUDE_MASK_PICKER_LABEL)
		latitude_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(latitude_index, LATITUDE_MASK_TYPE)
	else:
		_node_type_picker.set_item_text(latitude_index, LATITUDE_MASK_PICKER_LABEL)

	longitude_index = _picker_index_for_metadata(LONGITUDE_MASK_PICKER_TYPE)
	if longitude_index < 0:
		_node_type_picker.add_item(LONGITUDE_MASK_PICKER_LABEL)
		longitude_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(longitude_index, LONGITUDE_MASK_PICKER_TYPE)
	else:
		_node_type_picker.set_item_text(longitude_index, LONGITUDE_MASK_PICKER_LABEL)
	_node_type_picker.tooltip_text = "Latitude and Longitude Bands output 0 to 1 from normalized planet-space direction. Use them with Multiply or Mix in an authored displacement flow."


func _picker_index_for_metadata(metadata_value: String) -> int:
	if _node_type_picker == null:
		return -1
	for index: int in _node_type_picker.item_count:
		if String(_node_type_picker.get_item_metadata(index)) == metadata_value:
			return index
	return -1


func _spatial_mask_available_here() -> bool:
	return _graph != null and int(_graph.get(&"domain")) == GRAPH_SCRIPT.Domain.DISPLACEMENT \
		and not _is_native_branch_graph()


func _latitude_mask_available_here() -> bool:
	# Kept for Phase 41 regressions and downstream wrappers that used the original
	# helper name before longitude support was added.
	return _spatial_mask_available_here()


func _append_latitude_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {})


func _append_longitude_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	# Allocate through the canonical graph schema exactly like Latitude Mask. The
	# optional axis value is deliberately forward-compatible: existing graph
	# resources preserve arbitrary parameter dictionaries on clone/save/load.
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {
		"axis":"longitude",
		"south_deg":-45.0,
		"north_deg":45.0,
		"feather_deg":5.0,
		"invert":false,
	})


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

	_add_port_row(graph_node, "Mask 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_degree_parameter(graph_node, node_id, "South edge", "south_deg",
		float(parameters.get("south_deg", -30.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "North edge", "north_deg",
		float(parameters.get("north_deg", 30.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Feather", "feather_deg",
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


func _create_longitude_mask_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "LONGITUDE MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	_add_port_row(graph_node, "Mask 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	# The legacy south/north keys are intentionally reused inside the serialized
	# LATITUDE_MASK variant. This keeps the bytecode parameter ABI at one vec4.
	_add_degree_parameter(graph_node, node_id, "West edge", "south_deg",
		float(parameters.get("south_deg", -45.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "East edge", "north_deg",
		float(parameters.get("north_deg", 45.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 180.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the band"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 inside the longitude arc. On: 1 outside the arc."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "Longitude is measured around the planet with 0° on +Z. The band runs eastward from West edge to East edge; West > East crosses the ±180° seam. Feather is circular and seam-safe."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 300.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _add_latitude_parameter(node: GraphNode, node_id: String, label_text: String,
		key: String, value: float, minimum: float, maximum: float, step: float) -> void:
	# Backward-compatible helper retained for any downstream Phase 41 wrapper.
	_add_degree_parameter(node, node_id, label_text, key, value, minimum, maximum, step)


func _add_degree_parameter(node: GraphNode, node_id: String, label_text: String,
		key: String, value: float, minimum: float, maximum: float, step: float) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(300.0, 32.0)
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
