extends "res://scripts/world_authoring/terrain_graph_editor_phase37.gd"
## Phase 40 graph presentation for exact resident linear contribution composition.
##
## Serialization stays generic (`ADD`, `MULTIPLY`, `MIX`, `CONSTANT_FLOAT`) so saved
## graphs and the generic compiler remain compatible. Inside Base Terrain Shape those
## same nodes are presented in terrain language: Add Contributions, Scale
## Contribution, Blend Contributions and their constant factors. Runtime acceptance
## remains exclusively owned by terrain_production_geomorph_lowering_phase40.gd.

const PHASE40_LOWERING := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_lowering_phase40.gd")


func _build_ui() -> void:
	super._build_ui()
	_relabel_phase40_node_picker()


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	if _is_native_branch_graph():
		var node_id: String = String(node_data.get("id", ""))
		if node_type == "ADD" and not _is_final_production_add(node_id):
			return "ADD CONTRIBUTIONS"
		if node_type == "MULTIPLY":
			return "SCALE CONTRIBUTION"
		if node_type == "MIX":
			return "BLEND CONTRIBUTIONS"
		if node_type == "CONSTANT_FLOAT":
			match _factor_role(node_id):
				"scale": return "SCALE FACTOR"
				"blend": return "BLEND AMOUNT"
				_: return "NUMBER / FACTOR"
	return super._graph_node_title(node_type, node_data)


func _create_graph_node(node_data: Dictionary) -> void:
	var node_type: String = String(node_data.get("type", ""))
	var node_id: String = String(node_data.get("id", ""))
	if _is_native_branch_graph() and not node_id.is_empty():
		if node_type == "ADD" and not _is_final_production_add(node_id):
			_create_linear_math_node(node_data, "add")
			return
		if node_type == "MULTIPLY":
			_create_linear_math_node(node_data, "scale")
			return
		if node_type == "MIX":
			_create_linear_math_node(node_data, "blend")
			return
		if node_type == "CONSTANT_FLOAT" and not _factor_role(node_id).is_empty():
			_create_factor_node(node_data, _factor_role(node_id))
			return
	super._create_graph_node(node_data)


func _create_merge_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _graph_node_title(NATIVE_GRAPH.MERGE_TYPE, node_data)
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	# Dedicated sockets preserve stage provenance for direct and single-stage scale
	# paths. Their port order must remain identical to NATIVE.merge_port_for_stage().
	for stage_id: String in NATIVE_GRAPH.contribution_stage_ids():
		var stage_title: String = NATIVE_GRAPH.stage_title_for_node_type(
			NATIVE_GRAPH.stage_node_type(stage_id))
		_add_port_row(graph_node, stage_title + " contribution", true, false)

	# Multi-stage linear expressions have no single native-stage identity. Give them
	# explicit sockets rather than allowing a combined result to masquerade as one
	# production stage. The runtime validates these exact port indices.
	for index: int in PHASE40_LOWERING.group_merge_port_count():
		_add_port_row(graph_node, "Custom Group %d" % (index + 1), true, false)

	_add_port_row(graph_node, "Merged native detail", false, true)
	_add_settings_note(graph_node,
		"Direct stage wires and single-stage Scale paths return to their matching named socket. "
		+ "When Add or Blend combines two or more stages, connect the result to any Custom Group socket. "
		+ "Invalid or incomplete wiring stays in preview only; the last valid world terrain remains active.")


func _create_linear_math_node(node_data: Dictionary, role: String) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _graph_node_title(String(node_data.get("type", "")), node_data)
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	match role:
		"add":
			_add_port_row(graph_node, "Terrain A  →  Combined", true, true)
			_add_port_row(graph_node, "Terrain B", true, false)
			_add_settings_note(graph_node,
				"Adds two different terrain contributions. If the result contains more than one production stage, connect it to a Custom Group socket on Native Detail Merge.")
		"scale":
			_add_port_row(graph_node, "Contribution  →  Scaled", true, true)
			_add_port_row(graph_node, "Scale Factor", true, false)
			_add_settings_note(graph_node,
				"Changes the strength of one contribution or grouped linear result. One input must be a Number / Factor. Safe scale range: 0 to 4. 0 disables it; 1 keeps the original strength.")
		"blend":
			_add_port_row(graph_node, "Terrain A  →  Blended", true, true)
			_add_port_row(graph_node, "Terrain B", true, false)
			_add_port_row(graph_node, "Blend Amount", true, false)
			_add_settings_note(graph_node,
				"Blends between two different terrain contributions using a constant amount. 0 uses Terrain A; 1 uses Terrain B. Dynamic masks are deliberately not enabled yet because physics and rendering must stay identical.")
	_add_node_action_row(graph_node, node_id)


func _create_factor_node(node_data: Dictionary, role: String) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _graph_node_title("CONSTANT_FLOAT", node_data)
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(220.0, 32.0)
	var label := Label.new()
	label.text = "Amount" if role == "blend" else "Strength"
	label.custom_minimum_size.x = 72.0
	row.add_child(label)
	var spin := SpinBox.new()
	spin.min_value = 0.0
	spin.max_value = 1.0 if role == "blend" else 4.0
	spin.step = 0.01
	spin.allow_lesser = false
	spin.allow_greater = false
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	spin.value = clampf(float(parameters.get("value", 0.0)), spin.min_value, spin.max_value)
	spin.value_changed.connect(func(value: float) -> void:
		_set_node_parameter(node_id, "value", value)
	)
	row.add_child(spin)
	graph_node.add_child(row)
	graph_node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))
	_add_settings_note(graph_node,
		"0 = Terrain A, 1 = Terrain B" if role == "blend" \
		else "0 = off, 1 = original strength, 2 = twice the contribution")
	_add_node_action_row(graph_node, node_id)


func _relabel_phase40_node_picker() -> void:
	if _node_type_picker == null or not _is_native_branch_graph():
		return
	for index: int in _node_type_picker.item_count:
		var node_type: String = String(_node_type_picker.get_item_metadata(index))
		match node_type:
			"ADD":
				_node_type_picker.set_item_text(index, "Math  ·  Add Contributions")
			"MULTIPLY":
				_node_type_picker.set_item_text(index, "Math  ·  Scale Contribution")
			"MIX":
				_node_type_picker.set_item_text(index, "Math  ·  Blend Contributions")
			"CONSTANT_FLOAT":
				_node_type_picker.set_item_text(index, "Utility  ·  Number / Factor")
	_node_type_picker.tooltip_text = "For Base Terrain: Add combines stages, Scale changes strength, Blend mixes two stages with a fixed amount. Multi-stage results go to Custom Group."


func _is_native_branch_graph() -> bool:
	return _graph != null and NATIVE_GRAPH.has_merge_node(_graph)


func _is_final_production_add(node_id: String) -> bool:
	if _graph == null or node_id.is_empty():
		return false
	var output_id: String = _find_node_id_by_type("OUTPUT_DISPLACEMENT")
	var compose_id: String = _find_node_id_by_type(NATIVE_GRAPH.COMPOSE_TYPE)
	var sculpt_id: String = _find_node_id_by_type(NATIVE_GRAPH.SCULPT_TYPE)
	if output_id.is_empty() or compose_id.is_empty() or sculpt_id.is_empty():
		return false
	return _has_exact_link(compose_id, 0, node_id, 0) \
		and _has_exact_link(sculpt_id, 0, node_id, 1) \
		and _has_exact_link(node_id, 0, output_id, 0)


func _factor_role(node_id: String) -> String:
	if _graph == null or node_id.is_empty():
		return ""
	for link_value: Variant in _graph.get(&"links") as Array:
		if not (link_value is Dictionary):
			continue
		var link: Dictionary = link_value as Dictionary
		if String(link.get("from", "")) != node_id or int(link.get("from_port", -1)) != 0:
			continue
		var target_id: String = String(link.get("to", ""))
		var target_type: String = _node_type_for_id(target_id)
		var target_port: int = int(link.get("to_port", -1))
		if target_type == "MULTIPLY" and (target_port == 0 or target_port == 1):
			return "scale"
		if target_type == "MIX" and target_port == 2:
			return "blend"
	return ""


func _find_node_id_by_type(node_type: String) -> String:
	if _graph == null:
		return ""
	for node_value: Variant in _graph.get(&"nodes") as Array:
		if node_value is Dictionary and String((node_value as Dictionary).get("type", "")) == node_type:
			return String((node_value as Dictionary).get("id", ""))
	return ""


func _node_type_for_id(node_id: String) -> String:
	if _graph == null:
		return ""
	for node_value: Variant in _graph.get(&"nodes") as Array:
		if node_value is Dictionary and String((node_value as Dictionary).get("id", "")) == node_id:
			return String((node_value as Dictionary).get("type", ""))
	return ""


func _has_exact_link(from_id: String, from_port: int, to_id: String, to_port: int) -> bool:
	if _graph == null:
		return false
	for link_value: Variant in _graph.get(&"links") as Array:
		if not (link_value is Dictionary):
			continue
		var link: Dictionary = link_value as Dictionary
		if String(link.get("from", "")) == from_id \
				and int(link.get("from_port", -1)) == from_port \
				and String(link.get("to", "")) == to_id \
				and int(link.get("to_port", -1)) == to_port:
			return true
	return false
