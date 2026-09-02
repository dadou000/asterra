extends "res://scripts/world_authoring/world_authoring_editor_live_phase31.gd"
## Phase 40 Planet Studio activation.
##
## Keep the mature live authoring/migration chain intact and replace only the graph
## editor factory with the Phase 40 presentation. Runtime execution remains owned by
## the authoritative terrain renderer and TerrainDisplacementRuntime.

const PHASE40_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase40.gd")


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := PHASE40_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "SIMPLE and DETAILED edit the same exact terrain as NODE GRAPH. In NODE GRAPH, production stages can be disabled, scaled, added or blended with constant factors while the resident production shader remains authoritative. Multi-stage Add/Blend results use CUSTOM GROUP on Native Detail Merge. PREVIEW never replaces the applied world unless the candidate is valid."
	else:
		hint.text = "The existing production surface is exposed through PBR outputs plus classifier, classifier thresholds, palette/materials, microrelief, anti-tiling, geology rock PBR, scanned PBR, exact scan textures, raw world fields and graph math/texture nodes. Reset Flow restores the current production defaults."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
