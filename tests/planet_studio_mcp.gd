extends SceneTree
## Exercises the real PlanetStudio scene, without starting the production world.

var _failures := 0

func _initialize() -> void:
	OS.set_environment("ASTERRA_MCP_ENABLED", "1")
	OS.set_environment("ASTERRA_MCP_TOKEN", "planet-studio-test-secret")
	OS.set_environment("ASTERRA_MCP_PORT", "19876")
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH", "user://world_authoring/tests/mcp_%d.tres" % OS.get_process_id())
	_run.call_deferred()

func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures += 1
		push_error("MCP TEST: " + message)

func _press_text(bridge: Node, text: String) -> bool:
	var ui: Dictionary = await bridge.dispatch("studio_ui", {})
	for row: Dictionary in ui.controls:
		if row.get("text", "") == text and row.type == "Button":
			var result: Dictionary = await bridge.dispatch("studio_control", {"id": row.id, "action": "press"})
			return result.get("ok", false)
	return false

func _run() -> void:
	var scene: PackedScene = load("res://scenes/world_authoring/PlanetStudio.tscn")
	var editor: Control = scene.instantiate()
	root.add_child(editor)
	await process_frame
	var bridge: Node = editor.get_node("PlanetStudioMCP")
	var status: Dictionary = await bridge.dispatch("studio_status", {})
	_check(status.has("dirty"), "status reports session")
	for category: String in ["PLANET", "TERRAIN", "WATER", "ATMOSPHERIC", "CELESTIALS"]:
		var category_result: Dictionary = await bridge.dispatch("studio_category", {"category": category})
		_check(category_result.get("ok", false), "category opens: " + category)
		var ui: Dictionary = await bridge.dispatch("studio_ui", {})
		_check(ui.get("controls", []).size() > 0, "category exposes controls: " + category)
	var invalid: Dictionary = await bridge.dispatch("studio_control", {"id": "0", "action": "press"})
	_check(invalid.has("error"), "reject invalid control")
	invalid = await bridge.dispatch("studio_category", {"category": "INVALID"})
	_check(invalid.has("error"), "reject unknown category")
	invalid = await bridge.dispatch("studio_session", {"action": "save_preset", "path": "res://bad.tres"})
	_check(invalid.has("error"), "confine preset paths")
	var inspected: Dictionary = await bridge.dispatch("studio_inspect", {"path": ["bodies"], "depth": 1})
	_check(inspected.get("value") is Array, "inspect staged bodies")
	invalid = await bridge.dispatch("studio_inspect", {"path": ["script"]})
	_check(invalid.has("error"), "no engine properties exposed")
	# A real transactional text field on the Planet page.
	await bridge.dispatch("studio_category", {"category": "PLANET"})
	var ui: Dictionary = await bridge.dispatch("studio_ui", {})
	var session: RefCounted = editor.get("_session")
	var original_name: String = session.call("active_body").get("display_name")
	var name_id := ""
	for row: Dictionary in ui.controls:
		if row.type == "LineEdit" and row.get("text", "") == original_name:
			name_id = row.id
			break
	_check(not name_id.is_empty(), "body name control discovered")
	if not name_id.is_empty():
		var changed: Dictionary = await bridge.dispatch("studio_control", {"id": name_id, "action": "set", "value": "MCP Test Planet"})
		_check(changed.get("ok", false), "name update succeeds")
		_check(session.call("active_body").get("display_name") == "MCP Test Planet", "UI change reaches staged resource")
		await bridge.dispatch("studio_session", {"action": "undo"})
		_check(session.call("active_body").get("display_name") == original_name, "undo restores model")
		await bridge.dispatch("studio_session", {"action": "redo"})
		_check(session.call("active_body").get("display_name") == "MCP Test Planet", "redo restores edit")
		invalid = await bridge.dispatch("studio_control", {"id": name_id, "action": "set", "value": "stale"})
		_check(invalid.has("error"), "rebuilt controls reject stale ids")
		await bridge.dispatch("studio_session", {"action": "revert"})
	await bridge.dispatch("studio_category", {"category": "TERRAIN"})
	_check(await _press_text(bridge, "BIOME TERRAIN"), "biome terrain tab opens")
	_check(await _press_text(bridge, "+ Add terrain layer"), "create biome terrain layer through MCP")
	ui = await bridge.dispatch("studio_ui", {})
	var has_layer_type := false
	for row: Dictionary in ui.controls:
		if row.get("text", "") == "Layer type": has_layer_type = true
	_check(has_layer_type, "new terrain layer controls exposed")
	_check(await _press_text(bridge, "BIOME TEXTURE"), "biome texture tab opens")
	_check(await _press_text(bridge, "+ Add texture band"), "create texture band through MCP")
	ui = await bridge.dispatch("studio_ui", {})
	var color_id := ""
	for row: Dictionary in ui.controls:
		if row.type == "ColorPickerButton":
			color_id = row.id
			break
	_check(not color_id.is_empty(), "texture band color exposed")
	if not color_id.is_empty():
		var colored: Dictionary = await bridge.dispatch("studio_control", {"id": color_id, "action": "color", "value": [0.2, 0.4, 0.6, 1.0]})
		_check(colored.get("ok", false), "edit texture band color")
	# Import through the actual dialog callback, not a direct model assignment.
	var image_path := "user://world_authoring/tests/mcp_texture_%d.png" % OS.get_process_id()
	var texture := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	texture.fill(Color(0.2, 0.4, 0.6))
	_check(texture.save_png(image_path) == OK, "test texture written")
	_check(await _press_text(bridge, "+ Import texture..."), "texture import dialog opens")
	ui = await bridge.dispatch("studio_ui", {})
	var dialog_id := ""
	for row: Dictionary in ui.controls:
		if row.type == "FileDialog" and row.get("title", "") == "Import albedo texture": dialog_id = row.id
	_check(not dialog_id.is_empty(), "open file dialog discoverable")
	if not dialog_id.is_empty():
		var imported: Dictionary = await bridge.dispatch("studio_control", {"id": dialog_id, "action": "file", "value": image_path})
		_check(imported.get("ok", false), "texture import callback succeeds")
		ui = await bridge.dispatch("studio_ui", {})
		var found_texture := false
		for row: Dictionary in ui.controls:
			if row.get("text", "") == image_path.get_file().get_basename(): found_texture = true
		_check(found_texture, "imported texture appears in editor")
	DirAccess.remove_absolute(image_path)
	await bridge.dispatch("studio_session", {"action": "revert"})
	# Custom response-curve control uses the same signal as pointer editing.
	var curve: Control = load("res://scripts/world_authoring/curve_field_control.gd").new()
	editor.add_child(curve)
	var points := [0.0, 0.0, 0.5, 0.8, 1.0, 1.0]
	var curved: Dictionary = await bridge.dispatch("studio_control", {"id": str(curve.get_instance_id()), "action": "curve", "value": points})
	_check(curved.get("ok", false), "curve edit succeeds")
	_check(curve.call("get_points").size() == 6, "curve points updated")
	invalid = await bridge.dispatch("studio_control", {"id": str(curve.get_instance_id()), "action": "curve", "value": [0, 0, 0, 1]})
	_check(invalid.has("error"), "reject degenerate curve")
	invalid = await bridge.dispatch("studio_input", {"events": [{"type": "motion", "position": [-100, -100]}]})
	_check(invalid.has("error"), "reject out-of-viewport input")
	if DisplayServer.get_name() == "headless":
		invalid = await bridge.dispatch("studio_screenshot", {})
		_check(invalid.has("error"), "headless screenshot fails cleanly")
	# Private transport authentication, driven from the same scene tree.
	var peer := StreamPeerTCP.new()
	peer.connect_to_host("127.0.0.1", 19876)
	for i: int in 120:
		peer.poll()
		if peer.get_status() == StreamPeerTCP.STATUS_CONNECTED: break
		await process_frame
	peer.put_data('{"token":"wrong","tool":"studio_status"}\n'.to_utf8_buffer())
	var response := PackedByteArray()
	for i: int in 240:
		peer.poll()
		var count := peer.get_available_bytes()
		if count > 0: response.append_array(peer.get_data(count)[1])
		if response.find(10) >= 0: break
		await process_frame
	var decoded: Variant = JSON.parse_string(response.get_string_from_utf8())
	_check(decoded is Dictionary and decoded.has("error"), "transport rejects wrong token")
	peer.disconnect_from_host()
	var recovery_path := OS.get_environment("ASTERRA_AUTHORING_RECOVERY_PATH")
	if FileAccess.file_exists(recovery_path): DirAccess.remove_absolute(recovery_path)
	editor.queue_free()
	await process_frame
	print("Planet Studio MCP: %d failures" % _failures)
	quit(0 if _failures == 0 else 1)
