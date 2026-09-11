extends Node
## Private newline-JSON transport used by tools/planet_studio_mcp/server.py.
## Only the editor subtree and explicitly supported operations are accessible.
##
## This is a boot autoload (see project.godot) rather than a child of the
## Planet Studio editor Control it used to be -- it now listens from the very
## first frame, independent of which scene/menu is showing, specifically so an
## MCP automation client can drive the start menu (studio_input) and poll
## readiness (studio_status) *before* Planet Studio has ever been opened, then
## keep using the same connection once it is. `_editor` is therefore resolved
## fresh on every dispatch (see _resolve_editor) instead of cached from
## get_parent(), and is null for that entire pre-Planet-Studio window -- every
## handler below must stay safe against that, not just against a stale id.

const MAX_BYTES := 8 * 1024 * 1024
const MAX_CLIENTS := 8
var _server := TCPServer.new()
var _clients: Array[Dictionary] = []
var _editor: Control
var _token: String
var _dispatching: bool = false
var _vision: Node

func _ready() -> void:
	if OS.get_environment("ASTERRA_MCP_ENABLED") != "1":
		set_process(false)
		return
	_vision = preload("res://scripts/world_authoring/mcp/planet_studio_vision.gd").new()
	add_child(_vision)
	_token = OS.get_environment("ASTERRA_MCP_TOKEN")
	if _token.length() < 16:
		push_error("Planet Studio MCP requires ASTERRA_MCP_TOKEN (at least 16 characters).")
		set_process(false)
		return
	var port_text := OS.get_environment("ASTERRA_MCP_PORT")
	var port := 9876 if port_text.is_empty() else port_text.to_int()
	if port < 1024 or port > 65535 or _server.listen(port, "127.0.0.1") != OK:
		push_error("Planet Studio MCP could not listen on the configured loopback port.")
		set_process(false)

## Finds the live Planet Studio editor Control wherever it currently exists --
## the standalone res://scenes/world_authoring/PlanetStudio.tscn test scene, or
## the "PlanetStudioLive" overlay world_authoring_runtime_host.gd creates once
## the player opens Planet Studio from the start menu. Duck-typed (rather than
## matched by node name) so both launch paths resolve the same way. Returns
## null before Planet Studio has been opened.
func _resolve_editor() -> Control:
	return _find_editor_control(get_tree().root)

func _find_editor_control(node: Node) -> Control:
	if node is Control and node.has_method("_show_category") and node.has_method("_refresh_all"):
		return node as Control
	for child: Node in node.get_children():
		var found := _find_editor_control(child)
		if found != null:
			return found
	return null

func _exit_tree() -> void:
	_server.stop()
	for client: Dictionary in _clients:
		client.peer.disconnect_from_host()

func _process(_delta: float) -> void:
	while _server.is_connection_available():
		var peer := _server.take_connection()
		if _clients.size() >= MAX_CLIENTS:
			peer.disconnect_from_host()
		else:
			_clients.append({"peer": peer, "data": PackedByteArray(), "busy": false, "started": Time.get_ticks_msec()})
	for client: Dictionary in _clients.duplicate():
		var peer: StreamPeerTCP = client.peer
		peer.poll()
		if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED or Time.get_ticks_msec() - int(client.started) > 120000:
			peer.disconnect_from_host()
			_clients.erase(client)
			continue
		if client.busy or _dispatching:
			continue
		var available := peer.get_available_bytes()
		if available > 0:
			if client.data.size() + available > MAX_BYTES:
				peer.disconnect_from_host()
				_clients.erase(client)
				continue
			var chunk := peer.get_data(available)
			if chunk[0] != OK:
				peer.disconnect_from_host()
				continue
			client.data.append_array(chunk[1])
			if client.data.find(10) >= 0:
				client.busy = true
				_dispatching = true
				_handle(client)

func _handle(client: Dictionary) -> void:
	var bytes: PackedByteArray = client.data
	var request: Variant = JSON.parse_string(bytes.slice(0, bytes.find(10)).get_string_from_utf8())
	var result: Dictionary
	if not request is Dictionary or request.get("token", "") != _token:
		result = {"error": "Invalid request or authentication token."}
	elif not request.get("arguments", {}) is Dictionary:
		result = {"error": "arguments must be an object."}
	else:
		result = await dispatch(String(request.get("tool", "")), request.get("arguments", {}))
	var peer: StreamPeerTCP = client.peer
	peer.put_data((JSON.stringify(result) + "\n").to_utf8_buffer())
	peer.disconnect_from_host()
	_clients.erase(client)
	_dispatching = false

const EDITOR_ONLY_TOOLS: PackedStringArray = [
	"studio_category", "studio_control", "studio_session", "studio_inspect", "studio_texture_stack",
]
const TEXTURE_BAND_COLOR_KEYS: PackedStringArray = ["color", "color_b", "emission_color"]

func dispatch(tool: String, args: Dictionary) -> Dictionary:
	_editor = _resolve_editor()
	_vision.set("editor", _editor)
	if _editor == null and tool in EDITOR_ONLY_TOOLS:
		return {"error": "Planet Studio is not open. Use studio_status to check editor_open, or studio_input to navigate the start menu into it."}
	var result: Dictionary
	match tool:
		"studio_status": result = _status()
		"studio_ui":
			var rows: Array = []
			var scope := String(args.get("scope", "editor"))
			if scope not in ["editor", "debug"]: return {"error": "Requested UI scope is unavailable."}
			var ui_root: Node = _editor if scope == "editor" else _vision.call("debug_menu")
			if ui_root == null: return {"error": "Requested UI scope is unavailable."}
			_walk(ui_root, rows, bool(args.get("include_hidden", false)), ui_root)
			result = {"controls": rows, "category": _editor.get("_category") if _editor != null else null}
		"studio_category":
			var category := String(args.get("category", ""))
			if category not in ["PLANET", "TERRAIN", "WATER", "ATMOSPHERIC", "CELESTIALS"]:
				return {"error": "Unknown editor category."}
			_editor.call("_show_category", category)
			result = {"ok": true}
		"studio_control": result = _control(args)
		"studio_session": result = _session_action(args)
		"studio_inspect": result = _inspect(args)
		"studio_texture_stack": result = _texture_stack(args)
		"studio_input": result = await _input_events(args)
		"studio_camera": result = await _vision.camera_command(args)
		"studio_time": result = _vision.time_command(args)
		"studio_view": result = _vision.view_command(args)
		"studio_captures": result = _vision.captures(args)
		"studio_screenshot":
			return await _vision.screenshot(args)
		_: return {"error": "Unknown tool."}
	# UI callbacks often queue_free/rebuild controls. Let those changes settle.
	await get_tree().process_frame
	await get_tree().process_frame
	return result

func _status() -> Dictionary:
	if _editor == null:
		var scene: Node = get_tree().current_scene
		return {"editor_open": false, "scene": scene.name if scene != null else "",
			"viewport_size": _encode(get_viewport().get_visible_rect().size, 0),
			"status": "Planet Studio is not open. Use studio_input to navigate there (e.g. press its start-menu button)."}
	var session: RefCounted = _editor.get("_session")
	var label: Label = _editor.get("_status_label")
	return {"editor_open": true, "category": _editor.get("_category"), "dirty": session.get("dirty"),
		"apply_scope": session.get("apply_scope"), "can_undo": session.call("can_undo"),
		"can_redo": session.call("can_redo"), "active_body_id": session.get("staged_system").get("active_body_id"),
		"status": label.text if label != null else "", "viewport_size": _encode(_editor.get_viewport_rect().size, 0),
		"live_world": _editor.get("_world_host") != null if "_world_host" in _editor else false}

func _walk(node: Node, rows: Array, include_hidden: bool, root: Node = null) -> void:
	if node == self or node.is_queued_for_deletion():
		return
	if node is Window and not node.visible and not include_hidden:
		return
	if node is CanvasLayer and not node.visible and not include_hidden:
		return
	if node is Control:
		var control := node as Control
		if include_hidden or control.is_visible_in_tree():
			var row := {"id": str(node.get_instance_id()), "path": str(root.get_path_to(node)) if root != null else str(node.get_path()),
				"type": node.get_class(), "visible": control.is_visible_in_tree(), "tooltip": control.tooltip_text,
				"rect": [control.global_position.x, control.global_position.y, control.size.x, control.size.y]}
			if node is Label or node is Button or node is LineEdit or node is TextEdit:
				row.text = node.text
			if node is BaseButton:
				row.disabled = node.disabled
				row.toggle = node.toggle_mode
				row.value = node.button_pressed
			if node is Range:
				row.merge({"value": node.value, "min": node.min_value, "max": node.max_value, "step": node.step})
			if node is LineEdit or node is TextEdit:
				row.editable = node.editable
			if node is OptionButton or node is ItemList:
				var items: Array = []
				for i: int in node.item_count:
					items.append({"index": i, "text": node.get_item_text(i), "disabled": node.is_item_disabled(i)})
				row.items = items
			if node is MenuButton:
				var popup: PopupMenu = node.get_popup()
				var items: Array = []
				for i: int in popup.item_count:
					items.append({"index": i, "text": popup.get_item_text(i), "disabled": popup.is_item_disabled(i) or popup.is_item_separator(i)})
				row.items = items
			if node is TabContainer:
				var items: Array = []
				for i: int in node.get_tab_count():
					items.append({"index": i, "text": node.get_tab_title(i), "disabled": node.is_tab_disabled(i)})
				row.items = items
				row.value = node.current_tab
			if node is ColorPickerButton:
				row.value = _encode(node.color, 0)
			if node.has_signal("curve_changed") and node.has_method("get_points"):
				row.type = "CurveFieldControl"
				row.value = Array(node.call("get_points"))
			rows.append(row)
	if node is FileDialog and (include_hidden or node.visible):
		rows.append({"id": str(node.get_instance_id()), "type": "FileDialog", "title": node.title,
			"visible": node.visible, "file_mode": node.file_mode, "access": node.access, "filters": Array(node.filters)})
	for child: Node in node.get_children():
		_walk(child, rows, include_hidden, root)

func _control(args: Dictionary) -> Dictionary:
	var id_text := String(args.get("id", ""))
	if not id_text.is_valid_int():
		return {"error": "Use a control id from studio_ui."}
	var object: Object = instance_from_id(id_text.to_int())
	var debug_root: Node = _vision.call("debug_menu")
	if not object is Node or object.is_queued_for_deletion():
		return {"error": "Stale or invalid control id. Refresh studio_ui."}
	var node := object as Node
	var control_root: Node = _editor if _editor != null and _editor.is_ancestor_of(node) else debug_root
	if control_root == null or not control_root.is_ancestor_of(node): return {"error": "Control is outside the editor/debug menu."}
	var ancestor: Node = node
	while ancestor != control_root.get_parent():
		if (ancestor is Window or ancestor is CanvasLayer) and not ancestor.visible:
			return {"error": "Control belongs to a hidden window."}
		ancestor = ancestor.get_parent()
	if node is Control and not node.is_visible_in_tree():
		return {"error": "Control is hidden. Open its category or tab first."}
	if node is BaseButton and node.disabled:
		return {"error": "Control is disabled."}
	var action := String(args.get("action", ""))
	var value: Variant = args.get("value")
	match action:
		"press":
			if not node is BaseButton or node is OptionButton or node is MenuButton:
				return {"error": "press requires a button; use select for options."}
			if node.toggle_mode:
				node.button_pressed = not node.button_pressed
			node.pressed.emit()
		"set":
			if node is Range:
				if not _number(value) or value < node.min_value or value > node.max_value:
					return {"error": "Number is outside the control range."}
				if node is SpinBox and not node.editable:
					return {"error": "Control is read-only."}
				node.value = value
			elif node is BaseButton and node.toggle_mode and value is bool:
				node.button_pressed = value
			elif node is LineEdit and value is String and node.editable:
				node.text = value
				node.text_changed.emit(value)
				node.text_submitted.emit(value)
			elif node is TextEdit and value is String and node.editable:
				node.text = value
				node.text_changed.emit()
			else:
				return {"error": "Unsupported or read-only control/value."}
		"select":
			if node is TabContainer:
				if not _number(value) or int(value) != value or value < 0 or value >= node.get_tab_count() or node.is_tab_disabled(int(value)):
					return {"error": "Invalid or disabled tab index."}
				node.current_tab = int(value)
				return {"ok": true}
			if node is MenuButton:
				var popup: PopupMenu = node.get_popup()
				if not _number(value) or int(value) != value or value < 0 or value >= popup.item_count:
					return {"error": "Invalid menu index."}
				var menu_index := int(value)
				if popup.is_item_disabled(menu_index) or popup.is_item_separator(menu_index):
					return {"error": "Menu item is disabled or a separator."}
				popup.id_pressed.emit(popup.get_item_id(menu_index))
				return {"ok": true}
			if not (node is OptionButton or node is ItemList) or not _number(value):
				return {"error": "select requires an option/list and integer index."}
			var index := int(value)
			if index != value or index < 0 or index >= node.item_count or node.is_item_disabled(index):
				return {"error": "Invalid or disabled item index."}
			node.select(index)
			node.item_selected.emit(index)
		"color":
			if not node is ColorPickerButton or not _numbers(value, 4):
				return {"error": "color requires a ColorPickerButton and [r,g,b,a]."}
			var color := Color(value[0], value[1], value[2], value[3])
			node.color = color
			node.color_changed.emit(color)
		"curve":
			if not node.has_signal("curve_changed") or not value is Array or value.size() < 4 or value.size() > 16 or value.size() % 2 != 0:
				return {"error": "curve requires 2 to 8 flattened [x,y] points."}
			for i: int in value.size():
				if not _number(value[i]) or value[i] < 0 or value[i] > 1:
					return {"error": "Curve coordinates must be finite and in [0,1]."}
				if i % 2 == 0 and i > 0 and value[i] <= value[i - 2]:
					return {"error": "Curve x coordinates must strictly increase."}
			if value[0] != 0 or value[-2] != 1:
				return {"error": "Curve endpoints must have x=0 and x=1."}
			var points := PackedFloat32Array(value)
			node.call("set_points", points)
			node.emit_signal("curve_changed", points)
		"file":
			if not node is FileDialog or not node.visible or not value is String:
				return {"error": "file requires an open FileDialog and path."}
			if node.access == FileDialog.ACCESS_USERDATA and not value.begins_with("user://"):
				return {"error": "This dialog requires a user:// path."}
			if node.access == FileDialog.ACCESS_RESOURCES and not value.begins_with("res://"):
				return {"error": "This dialog requires a res:// path."}
			if node.file_mode == FileDialog.FILE_MODE_OPEN_FILE and not FileAccess.file_exists(value):
				return {"error": "File does not exist."}
			node.hide()
			node.file_selected.emit(value)
		"scroll":
			if not node is ScrollContainer or not _numbers(value, 2):
				return {"error": "scroll requires a ScrollContainer and [horizontal,vertical]."}
			node.scroll_horizontal = int(value[0])
			node.scroll_vertical = int(value[1])
		_: return {"error": "Unknown control action."}
	return {"ok": true, "note": "Re-query studio_ui after changes; controls may have been rebuilt."}

func _session_action(args: Dictionary) -> Dictionary:
	var session: RefCounted = _editor.get("_session")
	var action := String(args.get("action", ""))
	match action:
		"undo", "redo", "apply", "revert":
			_editor.call("_on_%s_pressed" % action)
		"select_body":
			var body_id := String(args.get("body_id", ""))
			var found := false
			for body: Resource in session.get("staged_system").get("bodies"):
				if body.get("body_id") == body_id:
					found = true
			if not found:
				return {"error": "Unknown body_id."}
			session.call("select_body", body_id)
			_editor.call("_refresh_all")
		"save_preset", "load_preset":
			var path := String(args.get("path", ""))
			if not path.begins_with("user://world_authoring/presets/") or ".." in path or not path.ends_with(".tres"):
				return {"error": "Use user://world_authoring/presets/<name>.tres."}
			var error: int = session.call(action, path)
			if error != OK:
				return {"error": error_string(error)}
			_editor.call("_refresh_all")
		_: return {"error": "Unknown session action. Use UI controls for creation and editing."}
	return _status()

func _inspect(args: Dictionary) -> Dictionary:
	var session: RefCounted = _editor.get("_session")
	var value: Variant = session.get("staged_system")
	var path: Variant = args.get("path", [])
	if not path is Array or path.size() > 32:
		return {"error": "path must be an array of at most 32 property names/indices."}
	for part: Variant in path:
		if value is Resource and part is String:
			var allowed := false
			for prop: Dictionary in value.get_property_list():
				if prop.name == part and int(prop.usage) & PROPERTY_USAGE_SCRIPT_VARIABLE and int(prop.usage) & PROPERTY_USAGE_STORAGE:
					allowed = true
			if not allowed:
				return {"error": "Unknown stored resource property."}
			value = value.get(part)
		elif value is Array and _number(part) and int(part) == part and part >= 0 and part < value.size():
			value = value[int(part)]
		elif value is Dictionary and value.has(part):
			value = value[part]
		else:
			return {"error": "Invalid inspection path."}
	return {"value": _encode(value, clampi(int(args.get("depth", 2)), 0, 6))}

## Structured biome-texture band read/write. Reads the already-friendly
## layer-dict shape the live BIOME TEXTURE tab itself builds and consumes
## (_phase47_texture_stack / _phase47_stage_biome_texture on the editor)
## instead of making a caller reconstruct it from raw studio_inspect graph
## nodes/links -- and writes an entire band stack in one staged action instead
## of one studio_control call per field.
func _texture_stack(args: Dictionary) -> Dictionary:
	var session: RefCounted = _editor.get("_session")
	var terrain: Resource = session.call("active_terrain_profile")
	if terrain == null:
		return {"error": "The selected body has no terrestrial terrain profile."}
	var biome_id := int(args.get("biome_id", -1))
	var biome_count: int = int(_editor.call("_phase47_biome_count"))
	if biome_id < 0 or biome_id >= biome_count:
		return {"error": "biome_id must be in [0, %d). See studio_ui's BiomeTexturePicker items for names." % biome_count}
	var action := String(args.get("action", "get"))
	match action:
		"get": return _texture_stack_get(terrain, biome_id)
		"set": return _texture_stack_set(terrain, biome_id, args)
		_: return {"error": "Unknown action. Use get or set."}


func _texture_stack_get(terrain: Resource, biome_id: int) -> Dictionary:
	var slot: Resource = _editor.call("_phase47_biome_texture_slot", terrain, biome_id) as Resource
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	var layers: Array = _editor.call("_phase47_texture_stack", graph) if graph != null else []
	var library_slot: Resource = _editor.call("_phase47_texture_library_slot", terrain) as Resource
	var custom_entries: Array = _editor.call("_phase47_custom_texture_stack",
		library_slot.get(&"graph") as Resource) if library_slot != null else []
	return {
		"biome_id": biome_id,
		"layers": _encode(layers, 6),
		"texture_choice_labels": ["Flat colour", "Ground", "Grass", "Mud", "Forest", "Imported"],
		"imported_textures": _encode(custom_entries, 4),
	}


func _texture_stack_set(terrain: Resource, biome_id: int, args: Dictionary) -> Dictionary:
	var raw_layers: Variant = args.get("layers")
	if not raw_layers is Array or (raw_layers as Array).size() > 8:
		return {"error": "layers must be an array of at most 8 band objects. Use action=get for the current shape and field names."}
	var layers: Array = []
	for raw_value: Variant in raw_layers as Array:
		if not raw_value is Dictionary:
			return {"error": "Each layer must be an object."}
		var layer: Dictionary = (raw_value as Dictionary).duplicate(true)
		for color_key: String in TEXTURE_BAND_COLOR_KEYS:
			var color_value: Variant = layer.get(color_key)
			if color_value is Array:
				if not _numbers(color_value, 3) and not _numbers(color_value, 4):
					return {"error": "%s must be [r,g,b] or [r,g,b,a]." % color_key}
				var c: Array = color_value
				layer[color_key] = Color(c[0], c[1], c[2], c[3] if c.size() > 3 else 1.0)
		var curve_value: Variant = layer.get("gradient_curve")
		if curve_value is Array:
			if curve_value.size() < 4 or curve_value.size() > 16 or curve_value.size() % 2 != 0:
				return {"error": "gradient_curve must be 2 to 8 flattened [x,y] points."}
			layer["gradient_curve"] = PackedFloat32Array(curve_value)
		layers.append(layer)
	var slot: Resource = _editor.call("_phase47_ensure_biome_texture_slot", terrain, biome_id) as Resource
	if slot == null:
		return {"error": "This body's terrain profile could not be provisioned."}
	var graph: Resource = slot.get(&"graph") as Resource
	_editor.call("_phase47_stage_biome_texture", graph, layers, "MCP: set biome texture stack")
	return {"ok": true, "biome_id": biome_id, "band_count": layers.size()}


func _encode(value: Variant, depth: int) -> Variant:
	if value is Resource:
		var result := {"type": value.get_class()}
		if depth < 0:
			return result
		for prop: Dictionary in value.get_property_list():
			if int(prop.usage) & PROPERTY_USAGE_SCRIPT_VARIABLE and int(prop.usage) & PROPERTY_USAGE_STORAGE:
				result[prop.name] = _encode(value.get(prop.name), depth - 1)
		return result
	if value is Array or value is Dictionary:
		if depth < 0:
			return {"size": value.size(), "truncated": true}
		if value is Dictionary:
			var result: Dictionary = {}
			for key: Variant in value:
				result[str(key)] = _encode(value[key], depth - 1)
			return result
		var result: Array = []
		for item: Variant in value:
			result.append(_encode(item, depth - 1))
		return result
	if value is Vector2: return [value.x, value.y]
	if value is Vector3: return [value.x, value.y, value.z]
	if value is Color: return [value.r, value.g, value.b, value.a]
	if value is PackedByteArray: return {"bytes": value.size()}
	if typeof(value) >= TYPE_PACKED_INT32_ARRAY:
		return _encode(Array(value), depth)
	if value is Object: return null
	return value

func _number(value: Variant) -> bool:
	return (value is int or value is float) and is_finite(float(value))

func _numbers(value: Variant, count: int) -> bool:
	if not value is Array or value.size() != count:
		return false
	for item: Variant in value:
		if not _number(item): return false
	return true

func _input_events(args: Dictionary) -> Dictionary:
	var events: Variant = args.get("events", [])
	if not events is Array or events.is_empty() or events.size() > 120:
		return {"error": "Provide 1 to 120 input events."}
	# Validate the whole sequence before injecting any event.
	var prepared: Array[InputEvent] = []
	for data: Variant in events:
		if not data is Dictionary:
			return {"error": "Each event must be an object."}
		var event: InputEvent
		match data.get("type", ""):
			"key":
				var key := OS.find_keycode_from_string(String(data.get("key", "")))
				if key == KEY_NONE: return {"error": "Unknown key name."}
				var keyboard := InputEventKey.new()
				keyboard.keycode = key
				keyboard.physical_keycode = key
				keyboard.pressed = bool(data.get("pressed", true))
				event = keyboard
			"button", "motion":
				if not _numbers(data.get("position"), 2): return {"error": "Mouse position requires [x,y]."}
				var position := Vector2(data.position[0], data.position[1])
				# Use the root viewport, not _editor's -- this must keep working with
				# no editor open (main menu, or the live game before Planet Studio is
				# opened) so automation can click through to it in the first place.
				# Bounds check stays in LOGICAL space -- get_visible_rect() (and
				# studio_status.viewport_size, and every studio_ui control's `rect`)
				# report the base 1600x900 canvas_items-stretched space regardless of
				# the actual window resolution, so that is the contract callers code
				# against.
				if not get_viewport().get_visible_rect().has_point(position): return {"error": "Mouse position is outside the viewport."}
				# Input.warp_mouse() and the InputEvent position Godot actually
				# hit-tests Controls against both operate in real WINDOW pixels, not
				# the logical canvas -- at the project's default 1600x900 window
				# those coincide, but the moment the window is a different physical
				# size (any non-default --resolution, e.g. running at 4K) a logical
				# position lands nowhere near the intended control. Convert once here
				# so every caller keeps using the same logical coordinates regardless
				# of window size.
				var window_size := Vector2(DisplayServer.window_get_size())
				var logical_size: Vector2 = get_viewport().get_visible_rect().size
				var to_physical: Vector2 = window_size / Vector2(maxf(logical_size.x, 1.0), maxf(logical_size.y, 1.0))
				var physical_position := position * to_physical
				if data.type == "button":
					var button := int(data.get("button", 1))
					if button < 1 or button > 5: return {"error": "Mouse button must be 1 through 5."}
					var mouse := InputEventMouseButton.new()
					mouse.position = physical_position
					mouse.global_position = physical_position
					mouse.button_index = button
					mouse.pressed = bool(data.get("pressed", true))
					mouse.double_click = bool(data.get("double_click", false))
					event = mouse
				else:
					if not _numbers(data.get("relative", [0, 0]), 2): return {"error": "relative requires [x,y]."}
					var motion := InputEventMouseMotion.new()
					motion.position = physical_position
					motion.global_position = physical_position
					var relative: Array = data.get("relative", [0, 0])
					motion.relative = Vector2(relative[0], relative[1]) * to_physical
					motion.button_mask = int(data.get("button_mask", 0))
					event = motion
			_: return {"error": "Input type must be key, button or motion."}
		if event is InputEventWithModifiers:
			event.shift_pressed = bool(data.get("shift", false))
			event.ctrl_pressed = bool(data.get("ctrl", false))
			event.alt_pressed = bool(data.get("alt", false))
		prepared.append(event)
	for event: InputEvent in prepared:
		if event is InputEventMouse and DisplayServer.get_name() != "headless":
			Input.warp_mouse(event.position)
		Input.parse_input_event(event)
		await get_tree().process_frame
	return {"ok": true, "events": prepared.size()}
