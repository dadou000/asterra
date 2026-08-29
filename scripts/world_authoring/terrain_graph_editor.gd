class_name TerrainGraphEditor
extends VBoxContainer
## Runtime Planet Studio graph editor. The graph Resource remains renderer-agnostic;
## this control only edits serialized nodes/links through the transactional session.

const GRAPH_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")

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
	title.text = "NODE GRAPH"
	title.custom_minimum_size.x = 120.0
	title.add_theme_font_size_override("font_size", 16)
	toolbar.add_child(title)
	_node_type_picker = OptionButton.new()
	_node_type_picker.custom_minimum_size.x = 190.0
	for node_type: String in GRAPH_SCRIPT.NODE_TYPES:
		if node_type.begins_with("OUTPUT_"):
			continue
		_node_type_picker.add_item(_pretty(node_type))
		_node_type_picker.set_item_metadata(_node_type_picker.item_count - 1, node_type)
	toolbar.add_child(_node_type_picker)
	var add_button := Button.new()
	add_button.text = "Add Node"
	add_button.pressed.connect(_on_add_node)
	toolbar.add_child(add_button)
	var reset_button := Button.new()
	reset_button.text = "Reset Graph"
	reset_button.pressed.connect(_on_reset_graph)
	toolbar.add_child(reset_button)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(spacer)
	var revision := Label.new()
	revision.text = "revision %d" % int(_graph.get(&"revision"))
	revision.modulate = Color(0.55, 0.65, 0.74)
	toolbar.add_child(revision)

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
	for link: Dictionary in _graph.get(&"links"):
		var from_id := StringName(String(link.get("from", "")))
		var to_id := StringName(String(link.get("to", "")))
		if _graph_edit.has_node(NodePath(String(from_id))) and _graph_edit.has_node(NodePath(String(to_id))):
			_graph_edit.connect_node(from_id, int(link.get("from_port", 0)), to_id, int(link.get("to_port", 0)))

	_status = Label.new()
	_status.text = "Graph model is live in the preset/undo system. GPU shader compilation is a separate integration stage."
	_status.modulate = Color(0.58, 0.67, 0.75)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_status)

func _create_graph_node(node_data: Dictionary) -> void:
	var node_id := String(node_data.get("id", ""))
	var node_type := String(node_data.get("type", "CONSTANT_FLOAT"))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _pretty(node_type)
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

func _set_node_parameter(node_id: String, key: String, value: Variant) -> void:
	if _session == null or _graph == null:
		return
	_session.call("stage_action", "Change graph node parameter", func() -> void:
		_graph.call("set_node_parameter", node_id, key, value)
	, 2)

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
	_graph_edit.connect_node(from_node, from_port, to_node, to_port)

func _on_disconnection_requested(from_node: StringName, from_port: int, to_node: StringName, to_port: int) -> void:
	_session.call("stage_action", "Disconnect graph nodes", func() -> void:
		_graph.call("disconnect_nodes", String(from_node), from_port, String(to_node), to_port)
	, 2)
	_graph_edit.disconnect_node(from_node, from_port, to_node, to_port)

func _on_delete_nodes_requested(node_names: Array[StringName]) -> void:
	var removable: Array[String] = []
	for node_name: StringName in node_names:
		var node_type := _graph_node_type(String(node_name))
		if not node_type.begins_with("OUTPUT_"):
			removable.append(String(node_name))
	if removable.is_empty():
		if _status != null:
			_status.text = "Output nodes are permanent; reset the graph instead."
		return
	_session.call("stage_action", "Delete graph nodes", func() -> void:
		for node_id: String in removable:
			_graph.call("remove_node", node_id)
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
	if _rebuild_requested.is_valid():
		_rebuild_requested.call()
	else:
		_build_ui()

func _pretty(value: String) -> String:
	return value.replace("_", " ").capitalize()
