extends Node
## Runtime smoke test for the exact Phase 28 construction path that static parser
## validation cannot cover. This is launched through a .tscn so project autoload
## globals resolve exactly as they do in the game.

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase28.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	# Phase 25's live runtime-shader owner resolver reads this from the editor's
	# CanvasLayer parent host. Keep the smoke hierarchy identical to the game.
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var world := DummyWorld.new()
	world.name = "Phase28SmokeWorld"
	add_child(world)

	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)

	var editor: Control = EDITOR_SCRIPT.new() as Control
	if editor == null:
		_fail("Phase 28 editor failed to instantiate.")
		return
	editor.name = "PlanetStudioLive"
	editor.call("bind_world", world)
	layer.add_child(editor)

	# Phase 21 deliberately defers full/category rebuilds. Give both _ready() and
	# those deferred calls time to drain before entering the shader lab.
	await get_tree().process_frame
	await get_tree().process_frame
	await get_tree().process_frame
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 28 editor left the tree during launch.")
		return

	# Seed one real graph so the SHADERS page must construct TerrainGraphEditor and
	# its GraphEdit/GraphNodes. This is the lifecycle path that previously built the
	# graph while detached from the scene tree.
	var session: Object = editor.get("_session") as Object
	if session == null or not session.has_method("active_terrain_profile"):
		_fail("Phase 28 editor has no authoring session.")
		return
	var terrain: Resource = session.call("active_terrain_profile") as Resource
	if terrain == null:
		_fail("Phase 28 smoke world has no terrain profile.")
		return
	var slot: Resource = terrain.call("create_shader_slot", 0, "Launch smoke displacement") as Resource
	if slot == null:
		_fail("Phase 28 failed to create smoke displacement graph.")
		return
	slot.set(&"clipmap_level_mask", 1)

	editor.call("_show_category", "SHADERS")
	await get_tree().process_frame
	await get_tree().process_frame
	await get_tree().process_frame
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 28 editor left the tree while opening SHADERS.")
		return
	var graph_edits: Array[Node] = editor.find_children("*", "GraphEdit", true, false)
	if graph_edits.is_empty():
		_fail("Phase 28 SHADERS page did not construct its GraphEdit.")
		return

	print("PLANET_STUDIO_PHASE28_LAUNCH_OK: editor and GraphEdit alive")
	editor.queue_free()
	layer.queue_free()
	host.queue_free()
	world.queue_free()
	await get_tree().process_frame
	await get_tree().process_frame
	get_tree().quit(0)


func _fail(message: String) -> void:
	push_error(message)
	print("PLANET_STUDIO_PHASE28_LAUNCH_FAILED: %s" % message)
	get_tree().quit(1)
