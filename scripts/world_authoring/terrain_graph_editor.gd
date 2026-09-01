class_name TerrainGraphEditor
extends VBoxContainer
## Runtime Planet Studio graph editor. The graph Resource remains renderer-agnostic;
## this control only edits serialized nodes/links through the transactional session.

const GRAPH_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const QUICK_OPERATION_TYPES: Array[String] = [
	"NOISE_LAYER",
	"RIDGED_MOUNTAINS",
	"EROSION_CHANNELS",
	"SEDIMENT_DEPOSIT",
]
const PRODUCTION_BASE_NODE := "__production_base__"

var _session: RefCounted
var _slot: Resource
var _graph: Resource
var _graph_edit: GraphEdit
var _node_type_picker: OptionButton
var _status: Label
var _rebuild_requested: Callable

func setup(session: RefCounted, slot: Resource, rebuild_requested: Callable = Callable()) -> void:
	_session = session
	_slot = slot
	_rebuild_requested = rebuild_requested
	_graph = slot.get(&"graph") as Resource if slot != null else null
	if _graph != null:
		_graph.call("ensure_valid")
	_build_ui()

func _build_ui() -> void:
	for child: Node in get_children():
		remove_child(child)
		child.queue_free()
	if _graph == null:
		var missing := Label.new()
		missing.text = "This slot has no graph."
		add_child(missing)
		return

	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 6)
	add_child(toolbar)
	var title := Label.new()
	title.text = "LIVE TERRAIN FLOW"
	title.custom_minimum_size.x = 190.0
	title.add_theme_font_size_override("font_size", 16)
	toolbar.add_child(title)
	var hint := Label.new()
	hint.text = "Build left → right. New terrain operations connect themselves."
	hint.modulate = Color(0.62, 0.73, 0.82)
	hint.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(hint)
	var reset_button := Button.new()
	reset_button.text = "Reset Flow"
	reset_button.pressed.connect(_on_reset_graph)
	toolbar.add_child(reset_button)
	var frame_button := Button.new()
	frame_button.text = "Frame All"
	frame_button.tooltip_text = "Center every existing node in the canvas"
	frame_button.pressed.connect(_frame_graph_content)
	toolbar.add_child(frame_button)
	var revision := Label.new()
	revision.text = "live · r%d" % int(_graph.get(&"revision"))
	revision.modulate = Color(0.55, 0.65, 0.74)
	toolbar.add_child(revision)

	if int(_graph.get(&"domain")) == 0:
		_build_quick_operation_shelf()

	var more_toolbar := HBoxContainer.new()
	more_toolbar.add_theme_constant_override("separation", 6)
	add_child(more_toolbar)
	var more_label := Label.new()
	more_label.text = "More nodes"
	more_label.custom_minimum_size.x = 120.0
	more_toolbar.add_child(more_label)
	_node_type_picker = OptionButton.new()
	_node_type_picker.custom_minimum_size.x = 190.0
	for node_type: String in GRAPH_SCRIPT.NODE_TYPES:
		if node_type.begins_with("OUTPUT_") or QUICK_OPERATION_TYPES.has(node_type):
			continue
		_node_type_picker.add_item(_pretty(node_type))
		_node_type_picker.set_item_metadata(_node_type_picker.item_count - 1, node_type)
	more_toolbar.add_child(_node_type_picker)
	var add_button := Button.new()
	add_button.text = "Add Node"
	add_button.pressed.connect(_on_add_node)
	more_toolbar.add_child(add_button)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	more_toolbar.add_child(spacer)
	var help := Label.new()
	help.text = "Drag ports for custom logic · Delete removes selected nodes"
	help.modulate = Color(0.52, 0.63, 0.72)
	more_toolbar.add_child(help)

	_graph_edit = GraphEdit.new()
	_graph_edit.custom_minimum_size = Vector2(860.0, 520.0)
	_graph_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_graph_edit.right_disconnects = true
	_graph_edit.minimap_enabled = true
	_graph_edit.snapping_enabled = true
	_graph_edit.connection_request.connect(_on_connection_requested)
	_graph_edit.disconnection_request.connect(_on_disconnection_requested)
	_graph_edit.delete_nodes_request.connect(_on_delete_nodes_requested)
	_graph_edit.end_node_move.connect(_on_node_move_ended)
	add_child(_graph_edit)

	for node_data: Dictionary in _graph.get(&"nodes"):
		_create_graph_node(node_data)
	_create_production_base_node()
	for link: Dictionary in _graph.get(&"links"):
		var from_id := StringName(String(link.get("from", "")))
		var to_id := StringName(String(link.get("to", "")))
		if _graph_edit.has_node(NodePath(String(from_id))) and _graph_edit.has_node(NodePath(String(to_id))):
			_graph_edit.connect_node(from_id, int(link.get("from_port", 0)), to_id, int(link.get("to_port", 0)))
	_connect_production_base()

	_status = Label.new()
	_status.text = "Live: parameter and connection changes immediately recompile the rendered terrain and its matching contact/physics displacement."
	_status.modulate = Color(0.58, 0.67, 0.75)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_status)
	call_deferred("_frame_graph_content")

func _create_graph_node(node_data: Dictionary) -> void:
	var node_id := String(node_data.get("id", ""))
	var node_type := String(node_data.get("type", "CONSTANT_FLOAT"))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _graph_node_title(node_type, node_data)
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)
	match node_type:
		"GAME_INPUT":
			_add_game_input_row(graph_node, node_id, node_data)
		"CONSTANT_FLOAT":
			_add_float_constant_row(graph_node, node_id, node_data)
		"CONSTANT_COLOR":
			_add_color_constant_row(graph_node, node_id, node_data)
		"TEXTURE_2D":
			_add_texture_row(graph_node, node_id, node_data)
		"NOISE":
			_add_noise_row(graph_node, node_id, node_data)
		"NOISE_LAYER", "RIDGED_MOUNTAINS", "EROSION_CHANNELS", "SEDIMENT_DEPOSIT":
			_add_terrain_operation_rows(graph_node, node_id, node_type, node_data)
		"OUTPUT_DISPLACEMENT":
			_add_port_row(graph_node, "Height (m)", true, false)
		"OUTPUT_MATERIAL":
			_add_port_row(graph_node, "Albedo", true, false)
			_add_port_row(graph_node, "Normal", true, false)
			_add_port_row(graph_node, "Roughness", true, false)
			_add_port_row(graph_node, "Metallic", true, false)
			_add_port_row(graph_node, "AO", true, false)
		"MIX":
			_add_port_row(graph_node, "A", true, true)
			_add_port_row(graph_node, "B", true, false)
			_add_port_row(graph_node, "Factor", true, false)
		"SMOOTHSTEP":
			_add_port_row(graph_node, "Value", true, true)
			_add_port_row(graph_node, "Edge 0", true, false)
			_add_port_row(graph_node, "Edge 1", true, false)
		"TRIPLANAR":
			_add_port_row(graph_node, "Texture", true, true)
			_add_port_row(graph_node, "Scale", true, false)
		"CLAMP", "REMAP":
			_add_port_row(graph_node, "Value", true, true)
			_add_port_row(graph_node, "Min", true, false)
			_add_port_row(graph_node, "Max", true, false)
		"ABS":
			_add_port_row(graph_node, "Value", true, true)
		_:
			_add_port_row(graph_node, "A", true, true)
			_add_port_row(graph_node, "B", true, false)
	if not node_type.begins_with("OUTPUT_"):
		_add_node_action_row(graph_node, node_id)


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	if node_type == "OUTPUT_DISPLACEMENT":
		return "FINAL TERRAIN"
	if node_type == "OUTPUT_MATERIAL":
		return "FINAL TERRAIN MATERIAL"
	if node_type == "GAME_INPUT":
		var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
		var source: String = String(parameters.get("source", ""))
		if source.begins_with("base_"):
			return "PRODUCTION %s" % _pretty(source.trim_prefix("base_"))
	return _pretty(node_type)


func _create_production_base_node() -> void:
	if _graph_edit == null or _graph_edit.has_node(NodePath(PRODUCTION_BASE_NODE)):
		return
	if int(_graph.get(&"domain")) == 1 and _graph_has_material_base_nodes():
		return
	var base := GraphNode.new()
	base.name = PRODUCTION_BASE_NODE
	base.position_offset = Vector2(-80.0, 180.0)
	base.resizable = false
	base.ignore_invalid_connection_type = true
	base.title = "PRODUCTION TERRAIN (ALWAYS PRESENT)" if int(_graph.get(&"domain")) == 0 \
		else "PRODUCTION TEXTURES / PBR (ALWAYS PRESENT)"
	_graph_edit.add_child(base)
	if int(_graph.get(&"domain")) == 0:
		_add_port_row(base, "Generated terrain + sculpt", false, true)
	else:
		for output_name: String in ["Base albedo", "Base normal", "Base roughness", "Base metallic", "Base AO"]:
			_add_port_row(base, output_name, false, true)
	var note := Label.new()
	note.text = "Renderer-owned base · cannot be deleted"
	note.modulate = Color(0.56, 0.70, 0.80)
	base.add_child(note)


func _connect_production_base() -> void:
	if _graph_edit == null or not _graph_edit.has_node(NodePath(PRODUCTION_BASE_NODE)):
		return
	if int(_graph.get(&"domain")) == 1:
		var output_id: String = _find_output_node_id("OUTPUT_MATERIAL")
		if output_id.is_empty():
			return
		for port: int in 5:
			if not _graph_has_input_link(output_id, port):
				_graph_edit.connect_node(StringName(PRODUCTION_BASE_NODE), port,
					StringName(output_id), port)
		return
	var displacement_output: String = _find_output_node_id("OUTPUT_DISPLACEMENT")
	if displacement_output.is_empty():
		return
	var current: String = _graph_input_source(displacement_output, 0)
	if current.is_empty():
		_graph_edit.connect_node(StringName(PRODUCTION_BASE_NODE), 0,
			StringName(displacement_output), 0)
		return
	while true:
		var previous: String = _graph_input_source(current, 0)
		if previous.is_empty():
			break
		current = previous
	if QUICK_OPERATION_TYPES.has(_graph_node_type(current)):
		_graph_edit.connect_node(StringName(PRODUCTION_BASE_NODE), 0, StringName(current), 0)


func _graph_has_material_base_nodes() -> bool:
	for node_value: Variant in _graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		if String(node_data.get("type", "")) != "GAME_INPUT":
			continue
		var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
		if String(parameters.get("source", "")).begins_with("base_"):
			return true
	return false


func _find_output_node_id(node_type: String) -> String:
	for node_value: Variant in _graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		if String(node_data.get("type", "")) == node_type:
			return String(node_data.get("id", ""))
	return ""


func _graph_has_input_link(node_id: String, port: int) -> bool:
	return not _graph_input_source(node_id, port).is_empty()


func _graph_input_source(node_id: String, port: int) -> String:
	for link_value: Variant in _graph.get(&"links") as Array:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) == node_id and int(link.get("to_port", -1)) == port:
			return String(link.get("from", ""))
	return ""


func _add_node_action_row(node: GraphNode, node_id: String) -> void:
	var remove := Button.new()
	remove.text = "Delete this node"
	remove.tooltip_text = "Remove this node and its connections from the authored flow"
	remove.pressed.connect(_delete_node.bind(node_id))
	node.add_child(remove)

func _add_port_row(node: GraphNode, label_text: String, input_enabled: bool, output_enabled: bool) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(150.0, 28.0)
	var label := Label.new()
	label.text = label_text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	node.add_child(row)
	var slot_index := node.get_child_count() - 1
	node.set_slot(slot_index, input_enabled, 0, Color(0.65, 0.78, 0.95), output_enabled, 0, Color(0.93, 0.70, 0.35))

func _add_game_input_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(220.0, 32.0)
	var picker := OptionButton.new()
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var parameters: Dictionary = node_data.get("parameters", {})
	var current := String(parameters.get("source", "terrain_height_m"))
	for input_name: String in GRAPH_SCRIPT.GAME_INPUTS:
		picker.add_item(_pretty(input_name))
		picker.set_item_metadata(picker.item_count - 1, input_name)
		if input_name == current:
			picker.select(picker.item_count - 1)
	picker.item_selected.connect(func(index: int) -> void:
		_set_node_parameter(node_id, "source", String(picker.get_item_metadata(index)))
	)
	row.add_child(picker)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))

func _add_float_constant_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(180.0, 32.0)
	var spin := SpinBox.new()
	spin.min_value = -1000000000.0
	spin.max_value = 1000000000.0
	spin.step = 0.001
	var parameters: Dictionary = node_data.get("parameters", {})
	spin.value = float(parameters.get("value", 0.0))
	spin.value_changed.connect(func(value: float) -> void: _set_node_parameter(node_id, "value", value))
	row.add_child(spin)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))

func _add_color_constant_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(180.0, 32.0)
	var picker := ColorPickerButton.new()
	picker.custom_minimum_size.x = 150.0
	var parameters: Dictionary = node_data.get("parameters", {})
	picker.color = Color(parameters.get("value", Color.WHITE))
	picker.color_changed.connect(func(value: Color) -> void: _set_node_parameter(node_id, "value", value))
	row.add_child(picker)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))

func _add_texture_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(260.0, 32.0)
	var edit := LineEdit.new()
	edit.placeholder_text = "texture asset id / path"
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var parameters: Dictionary = node_data.get("parameters", {})
	edit.text = String(parameters.get("asset_id", ""))
	edit.text_submitted.connect(func(value: String) -> void: _set_node_parameter(node_id, "asset_id", value))
	row.add_child(edit)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))

func _add_noise_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(220.0, 32.0)
	var label := Label.new()
	label.text = "Scale"
	row.add_child(label)
	var spin := SpinBox.new()
	spin.min_value = 0.000001
	spin.max_value = 1000000.0
	spin.step = 0.01
	var parameters: Dictionary = node_data.get("parameters", {})
	spin.value = float(parameters.get("scale", 1.0))
	spin.value_changed.connect(func(value: float) -> void: _set_node_parameter(node_id, "scale", value))
	row.add_child(spin)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))


func _build_quick_operation_shelf() -> void:
	var shelf := HBoxContainer.new()
	shelf.add_theme_constant_override("separation", 6)
	add_child(shelf)
	var label := Label.new()
	label.text = "Add terrain operation"
	label.custom_minimum_size.x = 160.0
	shelf.add_child(label)
	for data: Dictionary in [
		{"label":"+ Noise Layer", "type":"NOISE_LAYER", "tip":"Layer fractal noise onto this L level."},
		{"label":"+ Ridged Mountains", "type":"RIDGED_MOUNTAINS", "tip":"Add sharp ridged mountain forms."},
		{"label":"+ Erosion Channels", "type":"EROSION_CHANNELS", "tip":"Carve procedural drainage-like channels."},
		{"label":"+ Sediment Deposit", "type":"SEDIMENT_DEPOSIT", "tip":"Deposit material in broad procedural basins."},
	]:
		var button := Button.new()
		button.text = String(data["label"])
		button.tooltip_text = String(data["tip"])
		button.pressed.connect(_on_add_quick_operation.bind(String(data["type"])))
		shelf.add_child(button)


func _add_terrain_operation_rows(node: GraphNode, node_id: String,
		node_type: String, node_data: Dictionary) -> void:
	_add_port_row(node, "Previous terrain  →  Result", true, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_operation_parameter(node, node_id, "Frequency", "scale",
		float(parameters.get("scale", 6.0)), 0.01, 10000.0, 0.05)
	var amount_label: String = "Height (m)"
	if node_type == "EROSION_CHANNELS":
		amount_label = "Cut depth (m)"
	elif node_type == "SEDIMENT_DEPOSIT":
		amount_label = "Deposit (m)"
	_add_operation_parameter(node, node_id, amount_label, "amount",
		float(parameters.get("amount", _operation_default_amount(node_type))),
		-100000.0, 100000.0, 1.0)
	_add_operation_parameter(node, node_id, "Detail passes", "passes",
		float(parameters.get("passes", 3)), 1.0, 4.0, 1.0)
	_add_operation_parameter(node, node_id, "Seed", "seed",
		float(parameters.get("seed", 0)), 0.0, 1048575.0, 1.0)


func _add_operation_parameter(node: GraphNode, node_id: String, label_text: String,
		key: String, value: float, minimum: float, maximum: float, step: float) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(245.0, 30.0)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 112.0
	row.add_child(label)
	var spin := SpinBox.new()
	spin.min_value = minimum
	spin.max_value = maximum
	spin.step = step
	spin.value = value
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spin.value_changed.connect(func(next_value: float) -> void:
		_set_node_parameter(node_id, key, int(next_value) if step >= 1.0 and key != "amount" else next_value)
	)
	row.add_child(spin)
	node.add_child(row)


func _operation_default_amount(node_type: String) -> float:
	match node_type:
		"RIDGED_MOUNTAINS": return 250.0
		"EROSION_CHANNELS": return 40.0
		"SEDIMENT_DEPOSIT": return 25.0
		_: return 100.0

func _set_node_parameter(node_id: String, key: String, value: Variant) -> void:
	if _session == null or _graph == null:
		return
	_session.call("stage_action", "Change graph node parameter", func() -> void:
		_graph.call("set_node_parameter", node_id, key, value)
	, 2)


func _delete_node(node_id: String) -> void:
	if node_id == PRODUCTION_BASE_NODE or _graph_node_type(node_id).begins_with("OUTPUT_"):
		return
	_session.call("stage_action", "Delete terrain flow node", func() -> void:
		_remove_node_preserving_flow(node_id)
	, 2)
	_request_rebuild()


func _remove_node_preserving_flow(node_id: String) -> void:
	# Modifier nodes are unary in the simple terrain flow. Preserve that flow when
	# removing a middle operation so Delete behaves like removing a visual block,
	# rather than silently leaving the downstream terrain disconnected.
	var incoming: Dictionary = {}
	var outgoing: Array[Dictionary] = []
	for link_value: Variant in _graph.get(&"links") as Array:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) == node_id and int(link.get("to_port", -1)) == 0:
			incoming = link.duplicate(true)
		elif String(link.get("from", "")) == node_id:
			outgoing.append(link.duplicate(true))
	_graph.call("remove_node", node_id)
	if incoming.is_empty():
		return
	for outgoing_link: Dictionary in outgoing:
		_graph.call("connect_nodes",
			String(incoming.get("from", "")), int(incoming.get("from_port", 0)),
			String(outgoing_link.get("to", "")), int(outgoing_link.get("to_port", 0)))


func _on_add_quick_operation(node_type: String) -> void:
	if _session == null or _graph == null or not QUICK_OPERATION_TYPES.has(node_type):
		return
	var output_id: String = ""
	var output_position := Vector2(520.0, 180.0)
	for node_value: Variant in _graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		if String(node_data.get("type", "")) != "OUTPUT_DISPLACEMENT":
			continue
		output_id = String(node_data.get("id", ""))
		output_position = Vector2(node_data.get("position", output_position))
		break
	if output_id.is_empty():
		return
	var previous_link: Dictionary = {}
	for link_value: Variant in _graph.get(&"links") as Array:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) == output_id and int(link.get("to_port", -1)) == 0:
			previous_link = link.duplicate(true)
			break
	var parameters: Dictionary = {
		"scale": 6.0,
		"amount": _operation_default_amount(node_type),
		"passes": 3,
		"seed": randi_range(0, 1048575),
	}
	if node_type == "RIDGED_MOUNTAINS":
		parameters["scale"] = 3.0
	elif node_type == "EROSION_CHANNELS":
		parameters["scale"] = 12.0
	elif node_type == "SEDIMENT_DEPOSIT":
		parameters["scale"] = 10.0
	var operation_position := output_position - Vector2(270.0, 0.0)
	_session.call("stage_action", "Add live terrain operation", func() -> void:
		if not previous_link.is_empty():
			_graph.call("disconnect_nodes",
				String(previous_link.get("from", "")), int(previous_link.get("from_port", 0)),
				output_id, 0)
		var operation_id: String = String(_graph.call("add_node",
			node_type, operation_position, parameters))
		if not previous_link.is_empty():
			_graph.call("connect_nodes", String(previous_link.get("from", "")),
				int(previous_link.get("from_port", 0)), operation_id, 0)
		_graph.call("connect_nodes", operation_id, 0, output_id, 0)
		_graph.call("set_node_position", output_id, output_position + Vector2(270.0, 0.0))
	, 2)
	_request_rebuild()

func _on_add_node() -> void:
	if _node_type_picker == null or _node_type_picker.item_count == 0:
		return
	var node_type := String(_node_type_picker.get_item_metadata(_node_type_picker.selected))
	var position := Vector2(180.0 + float((_graph.get(&"nodes") as Array).size() % 4) * 180.0, 120.0 + float((_graph.get(&"nodes") as Array).size() / 4) * 120.0)
	_session.call("stage_action", "Add graph node", func() -> void:
		var parameters := {}
		if node_type == "GAME_INPUT":
			parameters["source"] = "terrain_height_m"
		_graph.call("add_node", node_type, position, parameters)
	, 2)
	_request_rebuild()

func _on_reset_graph() -> void:
	var domain := int(_graph.get(&"domain"))
	_session.call("stage_action", "Reset terrain graph", func() -> void:
		_graph.call("create_default_graph", domain)
	, 2)
	_request_rebuild()

func _on_connection_requested(from_node: StringName, from_port: int, to_node: StringName, to_port: int) -> void:
	if String(from_node) == PRODUCTION_BASE_NODE or String(to_node) == PRODUCTION_BASE_NODE:
		if _status != null:
			_status.text = "The production base is always present and cannot be rewired. Add authored operations after it."
		return
	if _graph_edit.is_node_connected(from_node, from_port, to_node, to_port):
		return
	for link: Dictionary in _graph.get(&"links"):
		if String(link.get("to", "")) == String(to_node) and int(link.get("to_port", -1)) == to_port:
			if _status != null:
				_status.text = "Input already has a connection. Disconnect it before replacing it."
			return
	_session.call("stage_action", "Connect graph nodes", func() -> void:
		_graph.call("connect_nodes", String(from_node), from_port, String(to_node), to_port)
	, 2)
	if _graph_edit.is_node_connected(StringName(PRODUCTION_BASE_NODE), to_port, to_node, to_port):
		_graph_edit.disconnect_node(StringName(PRODUCTION_BASE_NODE), to_port, to_node, to_port)
	_graph_edit.connect_node(from_node, from_port, to_node, to_port)

func _on_disconnection_requested(from_node: StringName, from_port: int, to_node: StringName, to_port: int) -> void:
	if String(from_node) == PRODUCTION_BASE_NODE or String(to_node) == PRODUCTION_BASE_NODE:
		if _status != null:
			_status.text = "The production base connection is implicit and cannot be removed."
		return
	_session.call("stage_action", "Disconnect graph nodes", func() -> void:
		_graph.call("disconnect_nodes", String(from_node), from_port, String(to_node), to_port)
	, 2)
	_graph_edit.disconnect_node(from_node, from_port, to_node, to_port)

func _on_delete_nodes_requested(node_names: Array[StringName]) -> void:
	var removable: Array[String] = []
	for node_name: StringName in node_names:
		if String(node_name) == PRODUCTION_BASE_NODE:
			continue
		var node_type := _graph_node_type(String(node_name))
		if not node_type.begins_with("OUTPUT_"):
			removable.append(String(node_name))
	if removable.is_empty():
		if _status != null:
			_status.text = "Output nodes are permanent; reset the graph instead."
		return
	_session.call("stage_action", "Delete graph nodes", func() -> void:
		for node_id: String in removable:
			_remove_node_preserving_flow(node_id)
	, 2)
	_request_rebuild()

func _on_node_move_ended() -> void:
	var moved: Dictionary = {}
	for child: Node in _graph_edit.get_children():
		if child is GraphNode:
			var graph_node := child as GraphNode
			var node_id := String(graph_node.name)
			var old_position := _graph_node_position(node_id)
			if old_position.distance_squared_to(graph_node.position_offset) > 0.01:
				moved[node_id] = graph_node.position_offset
	if moved.is_empty():
		return
	_session.call("stage_action", "Move graph nodes", func() -> void:
		for node_id: String in moved:
			_graph.call("set_node_position", node_id, Vector2(moved[node_id]))
	, 2)

func _graph_node_type(node_id: String) -> String:
	for node_data: Dictionary in _graph.get(&"nodes"):
		if String(node_data.get("id", "")) == node_id:
			return String(node_data.get("type", ""))
	return ""

func _graph_node_position(node_id: String) -> Vector2:
	for node_data: Dictionary in _graph.get(&"nodes"):
		if String(node_data.get("id", "")) == node_id:
			return Vector2(node_data.get("position", Vector2.ZERO))
	return Vector2.ZERO

func _request_rebuild() -> void:
	# Rebuild only this canvas. Rebuilding the complete Shader Lab resets its outer
	# scroll position, which made newly added/existing nodes appear to disappear.
	call_deferred("_build_ui")


func _frame_graph_content() -> void:
	if _graph_edit == null or not is_instance_valid(_graph_edit) or not _graph_edit.is_inside_tree():
		return
	var minimum := Vector2(INF, INF)
	var maximum := Vector2(-INF, -INF)
	var found: bool = false
	for child: Node in _graph_edit.get_children():
		if not (child is GraphNode):
			continue
		var graph_node := child as GraphNode
		var node_size: Vector2 = graph_node.size
		if node_size.x <= 1.0 or node_size.y <= 1.0:
			node_size = Vector2(260.0, 180.0)
		minimum = minimum.min(graph_node.position_offset)
		maximum = maximum.max(graph_node.position_offset + node_size)
		found = true
	if not found:
		return
	var viewport_size: Vector2 = _graph_edit.size
	if viewport_size.x <= 1.0 or viewport_size.y <= 1.0:
		return
	var content_size: Vector2 = maximum - minimum + Vector2(160.0, 140.0)
	var target_zoom: float = minf(viewport_size.x / maxf(content_size.x, 1.0),
		viewport_size.y / maxf(content_size.y, 1.0))
	_graph_edit.zoom = clampf(target_zoom, _graph_edit.zoom_min, 1.0)
	var visible_graph_size: Vector2 = viewport_size / maxf(_graph_edit.zoom, 0.001)
	var center: Vector2 = (minimum + maximum) * 0.5
	_graph_edit.scroll_offset = (center - visible_graph_size * 0.5).max(Vector2.ZERO)

func _pretty(value: String) -> String:
	return value.replace("_", " ").capitalize()
