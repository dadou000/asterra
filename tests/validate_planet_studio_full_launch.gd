extends SceneTree
## Rendered launch harness for the real Planet Studio path. Unlike the isolated
## UI smoke scene, this loads Main with its production terrain and renderer.

const MAIN_SCENE := "res://scenes/Main.tscn"
const SUCCESS_AFTER_SECONDS := 20.0
const CURRENT_EDITOR_PATH := "res://scripts/world_authoring/world_authoring_editor_live_phase46.gd"


func _initialize() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/full_launch_recovery_%d.tres" % OS.get_process_id())
	var mode := "planet_studio"
	var user_args := OS.get_cmdline_user_args()
	if not user_args.is_empty():
		mode = user_args[0]
	set_meta("launch_mode", mode)
	var error := change_scene_to_file(MAIN_SCENE)
	if error != OK:
		push_error("Could not launch Planet Studio Main scene: %s" % error_string(error))
		quit(1)
		return
	_run_until_stable.call_deferred()


func _run_until_stable() -> void:
	await create_timer(SUCCESS_AFTER_SECONDS).timeout
	var editor := root.find_child("PlanetStudioLive", true, false)
	var mode := String(get_meta("launch_mode", "planet_studio"))
	if mode == "planet_studio" and (editor == null or not editor.is_inside_tree()):
		push_error("Planet Studio editor was not alive after rendered launch.")
		quit(1)
		return
	if mode == "planet_studio":
		var editor_script: Script = editor.get_script() as Script
		if editor_script == null or editor_script.resource_path != CURRENT_EDITOR_PATH:
			push_error("Main.tscn did not launch the current simple Terrain editor.")
			quit(1)
			return
		editor.call("_show_category", "TERRAIN")
		await process_frame
		if editor.find_child("TerrainAllRingsNotice", true, false) == null:
			push_error("Live Terrain tab did not expose the simple all-rings editor.")
			quit(1)
			return
		for button: Node in editor.find_children("*", "Button", true, false):
			if (button as Button).text == "SHADERS":
				push_error("Live Terrain editor still exposes the retired SHADERS tab.")
				quit(1)
				return
	print("PLANET_STUDIO_FULL_LAUNCH_OK: production renderer remained stable")
	quit(0)
