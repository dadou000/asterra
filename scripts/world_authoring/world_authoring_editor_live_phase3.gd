class_name WorldAuthoringLiveEditorPhase3
extends "res://scripts/world_authoring/world_authoring_editor_live_phase2.gd"
## Phase 3 live authoring feedback.
##
## Phase 2 deliberately batches a complete biome drag into one staged history
## action. This layer mirrors the still-pending stamps to the disposable runtime
## biome preview, so terrain material and ecology respond while the mouse is down
## without reintroducing per-motion preset snapshots/autosaves.

signal biome_preview_strokes_changed(layer_id: String, strokes: Array)


func _place_biome_stroke(direction: Vector3, continuous: bool) -> void:
	var before_count: int = _pending_biome_strokes.size()
	super._place_biome_stroke(direction, continuous)
	if _pending_biome_strokes.size() == before_count:
		return
	biome_preview_strokes_changed.emit(
		_selected_biome_layer_id,
		_pending_biome_strokes.duplicate(true))


func _commit_biome_transaction() -> void:
	super._commit_biome_transaction()
	biome_preview_strokes_changed.emit("", [])


func _discard_interactive_transactions() -> void:
	super._discard_interactive_transactions()
	biome_preview_strokes_changed.emit("", [])
