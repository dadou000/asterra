extends SceneTree
## Headless camera/clock tests; omit --headless to also verify PNG persistence.
var failures := 0

class TestPlayer extends Node3D:
	signal moved(position: Vec3D)
	var world_pos := Vec3D.new(0, 0, 1200)
	var yaw := 0.0
	var pitch := -0.2
	var input_enabled := false
	var camera: Camera3D

func _initialize() -> void:
	OS.set_environment("ASTERRA_MCP_ENABLED", "1")
	OS.set_environment("ASTERRA_MCP_TOKEN", "vision-integration-test-token")
	OS.set_environment("ASTERRA_MCP_PORT", "19877")
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH", "user://world_authoring/tests/vision_%d.tres" % OS.get_process_id())
	_run.call_deferred()

func check(value: bool, message: String) -> void:
	if not value:
		failures += 1
		push_error("VISION TEST: " + message)

func _run() -> void:
	var editor: Control = load("res://scenes/world_authoring/PlanetStudio.tscn").instantiate()
	root.add_child(editor)
	editor.set_process(false)
	var bridge: Node = editor.get_node("PlanetStudioMCP")
	var vision: Node = bridge.get("_vision")
	var frames: Node = root.get_node("Frames")
	var player := TestPlayer.new()
	player.camera = Camera3D.new()
	player.add_child(player.camera)
	root.add_child(player)
	player.camera.current = true
	editor.set("_player", player)
	editor.set("_camera", player.camera)
	editor.call("set_camera_interest", Vec3D.new(), 1000.0, true)
	var original: Dictionary = await bridge.dispatch("studio_camera", {"action": "state"})
	check(not original.has("error"), "camera state available")
	var moved: Dictionary = await bridge.dispatch("studio_camera", {"action": "move", "space": "world", "offset_m": [10, 20, 30]})
	check(is_equal_approx(float(moved.position_m[0]), 10.0) and is_equal_approx(float(moved.position_m[2]), 1230.0), "exact world displacement")
	await bridge.dispatch("studio_camera", {"action": "rotate", "degrees": [20, 10, 15]})
	await bridge.dispatch("studio_camera", {"action": "zoom", "fov_deg": 35})
	check(is_equal_approx(player.camera.fov, 35), "optical zoom")
	await bridge.dispatch("studio_camera", {"action": "pose", "pose": original})
	var restored: Dictionary = await bridge.dispatch("studio_camera", {"action": "state"})
	check(_vec(restored.forward).dot(_vec(original.forward)) > 0.99999, "pose restores camera direction")
	check(player.world_pos.sub(Vec3D.new(0, 0, 1200)).length() < 0.001, "pose restores position")
	await bridge.dispatch("studio_camera", {"action": "slew", "position_m": [20, 30, 1300], "duration_s": 0.05})
	check(player.world_pos.sub(Vec3D.new(20, 30, 1300)).length() < 0.001, "animated slew reaches destination")
	check(not editor.get("_camera_command_active"), "slew releases keyboard gate")
	# Exercise actual editor synchronization across both poles and old axis-switch
	# latitudes. No mouse motion: adjacent orientations must remain continuous.
	for pole: float in [0.0, 180.0]:
		var previous := Vector3.ZERO
		for step: int in 161:
			var angle := deg_to_rad(pole - 20.0 + step * 0.25)
			player.world_pos = Vec3D.new(sin(angle) * 1200.0, cos(angle) * 1200.0, 0.0)
			editor.call("_resync_camera_basis")
			var forward := -player.camera.global_basis.z
			if previous.length_squared() > 0.5: check(previous.dot(forward) > 0.999, "continuous heading across pole")
			check(absf(player.camera.global_basis.determinant() - 1.0) < 0.001, "camera basis stays orthonormal")
			previous = forward
	# Negative time, scrubbing, and body-calendar date selection.
	await bridge.dispatch("studio_time", {"action": "seek", "seconds": 1000})
	var clock: Dictionary = await bridge.dispatch("studio_time", {"action": "advance", "seconds": -1500})
	check(is_equal_approx(clock.seconds, -500), "rewind across epoch")
	await bridge.dispatch("studio_time", {"action": "rate", "rate": -10, "playing": false})
	check(is_equal_approx(float(frames.get("time_scale")), -10), "negative timewarp rate")
	clock = await bridge.dispatch("studio_time", {"action": "date", "year": 2, "day": 3, "hour": 12})
	check(is_equal_approx(clock.seconds, 2 * float(frames.call("year_seconds")) + 3.5 * float(frames.get("day_seconds"))), "date selection uses planetary calendar")
	var invalid: Dictionary = await bridge.dispatch("studio_time", {"action": "date", "year": 0, "day": 1000000, "hour": 0})
	check(invalid.has("error"), "invalid date rejected")
	# Debug view enumeration and full-bright restoration.
	var views: Dictionary = await bridge.dispatch("studio_view", {"action": "state"})
	check(views.modes.has("wireframe") and views.modes.has("normal_buffer"), "all viewport modes discoverable")
	await bridge.dispatch("studio_view", {"action": "mode", "mode": "normal_buffer"})
	await bridge.dispatch("studio_view", {"action": "full_bright", "enabled": true})
	check(root.debug_draw == Viewport.DEBUG_DRAW_UNSHADED, "full-bright enabled")
	await bridge.dispatch("studio_view", {"action": "full_bright", "enabled": false})
	check(root.debug_draw == Viewport.DEBUG_DRAW_NORMAL_BUFFER, "full-bright restores prior mode")
	await bridge.dispatch("studio_view", {"action": "mode", "mode": "disabled"})
	invalid = await bridge.dispatch("studio_captures", {"action": "read", "id": "../escape"})
	check(invalid.has("error"), "capture paths reject traversal")
	# Test comparison on known images even with the headless renderer.
	var directory: String = vision.CAPTURE_DIR
	DirAccess.make_dir_recursive_absolute(directory)
	var ids: Array[String] = ["vision-test-%d-a" % OS.get_process_id(), "vision-test-%d-b" % OS.get_process_id()]
	var picture := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	for id: String in ids:
		picture.fill(Color.BLACK if id == ids[0] else Color.WHITE)
		picture.save_png(directory.path_join(id + ".png"))
		var metadata := FileAccess.open(directory.path_join(id + ".json"), FileAccess.WRITE)
		metadata.store_string(JSON.stringify({"id": id, "name": "vision-test", "intent": "Test known pixel difference"}))
		metadata.close()
	var comparison: Dictionary = await bridge.dispatch("studio_captures", {"action": "compare", "id": ids[1], "baseline": ids[0]})
	check(is_equal_approx(comparison.get("mean_absolute_rgb_error", -1), 1.0), "comparison measures known RGB error")
	check(is_equal_approx(comparison.get("changed_pixel_fraction", -1), 1.0), "comparison measures changed pixels")
	var tracked: Dictionary = await bridge.dispatch("studio_captures", {"action": "read", "id": ids[1]})
	check(tracked.get("comparisons", {}).has(ids[0]), "baseline relationship persists")
	if comparison.has("diff_path"): DirAccess.remove_absolute(comparison.diff_path)
	for id: String in ids:
		for extension: String in [".png", ".json", "-diff.png"]:
			var path := directory.path_join(id + extension)
			if FileAccess.file_exists(path): DirAccess.remove_absolute(path)
	if DisplayServer.get_name() != "headless":
		# Small rendered fixture, not the expensive production terrain. Tests actual
		# readback and persistence rather than assuming headless image APIs suffice.
		var mesh := MeshInstance3D.new()
		mesh.mesh = BoxMesh.new()
		var material := StandardMaterial3D.new()
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		material.albedo_color = Color(0.9, 0.2, 0.1)
		mesh.material_override = material
		root.add_child(mesh)
		await bridge.dispatch("studio_camera", {"action": "pose", "pose": original})
		mesh.global_position = player.camera.global_position - player.camera.global_basis.z * 4.0
		var capture: Dictionary = await bridge.dispatch("studio_screenshot", {"name": "vision-fixture", "intent": "Verify rendered PNG and pose metadata", "hide_editor": true, "tags": ["test"]})
		check(capture.has("image") and capture.has("capture"), "rendered screenshot returns image and metadata")
		if capture.has("capture"):
			var data: Dictionary = capture.capture
			check(FileAccess.file_exists(data.png_path) and FileAccess.file_exists(data.metadata_path), "named files persist")
			print("Rendered vision fixture: " + data.png_path)
			# Keep the rendered artifact for visual inspection after the test.
		mesh.queue_free()
	frames.set("playing", false)
	editor.queue_free()
	player.queue_free()
	await process_frame
	var recovery := OS.get_environment("ASTERRA_AUTHORING_RECOVERY_PATH")
	if FileAccess.file_exists(recovery): DirAccess.remove_absolute(recovery)
	print("Planet Studio vision: %d failures" % failures)
	quit(0 if failures == 0 else 1)

func _vec(value: Array) -> Vector3:
	return Vector3(value[0], value[1], value[2])
