extends "res://scripts/world_authoring/world_authoring_editor_live_phase40.gd"
## Phase 41 Planet Studio activation.
##
## The only UI delta is the Phase 41 graph editor. It exposes deterministic Latitude
## Mask on generic authored displacement flows while retaining the exact Phase 40
## native-stage editor and its constant-only resident lowering contract.

const PHASE41_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase41.gd")


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := PHASE41_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "SIMPLE and DETAILED edit the same exact terrain as NODE GRAPH. Base Terrain native stages keep the safe Phase 40 Add / Scale / constant Blend contract. Additional authored displacement flows can now use LATITUDE MASK: 0° is the equator, ±90° are the poles, with a smooth feather. Rendering and contact/physics evaluate the same bytecode opcode. PREVIEW never replaces the applied world unless the candidate is valid."
	else:
		hint.text = "The existing production surface is exposed through PBR outputs plus classifier, classifier thresholds, palette/materials, microrelief, anti-tiling, geology rock PBR, scanned PBR, exact scan textures, raw world fields and graph math/texture nodes. Reset Flow restores the current production defaults."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
