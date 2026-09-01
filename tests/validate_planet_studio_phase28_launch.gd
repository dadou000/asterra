extends SceneTree
## Runtime smoke test for the exact Phase 28 construction path that static parser
## validation cannot cover. The editor is bound before entering the tree, matching
## WorldAuthoringRuntimeHostPhase25._open_live_editor().

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase28.gd")

class DummyWorld:
	extends Node3D
	var player: Node = null

var _failed: bool = false


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var world := DummyWorld.new()
	world.name = "Phase28SmokeWorld"
	root.add_child(world)

	var editor: Control = EDITOR_SCRIPT.new() as Control
	if editor == null:
		_fail("Phase 28 editor failed to instantiate.")
		return
	editor.name = "PlanetStudioLive"
	editor.call("bind_world", world)
	root.add_child(editor)

	# Phase 21 deliberately defers full/category rebuilds. Give both _ready() and
	# those deferred calls time to drain before entering the shader lab.
	await process_frame
	await process_frame
	await process_frame
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 28 editor left the tree during launch.")
		return

	editor.call("_show_category", "SHADERS")
	await process_frame
	await process_frame
	await process_frame
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 28 editor left the tree while opening SHADERS.")
		return

	var graph_edits: Array[Node] = editor.find_children("*", "GraphEdit", true, false)
	print("PLANET_STUDIO_PHASE28_LAUNCH_OK: editor alive; GraphEdit count=%d" % graph_edits.size())
	editor.queue_free()
	world.queue_free()
	await process_frame
	quit(0)


func _fail(message: String) -> void:
	_failed = true
	push_error(message)
	print("PLANET_STUDIO_PHASE28_LAUNCH_FAILED: %s" % message)
	quit(1)
