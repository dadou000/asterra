extends "res://scripts/world_authoring/world_authoring_editor_live_phase31.gd"
## Phase 40 Planet Studio activation.
##
## Keep the mature live authoring/migration chain intact and replace only the graph
## editor factory with the Phase 40 presentation. Runtime execution remains owned by
## the authoritative terrain renderer and TerrainDisplacementRuntime.

const PHASE40_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase40.gd")


func _phase29_build_graph_editor(slot: Resource) -> Control:
	var editor: Control = PHASE40_GRAPH_EDITOR.new() as Control
	editor.call("setup", _session, slot, Callable(self, "_rebuild_editor"))
	editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
	return editor
