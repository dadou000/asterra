class_name WorldAuthoringLiveEditorPhase3
extends "res://scripts/world_authoring/world_authoring_editor_live_phase2.gd"
## Phase 3 live authoring feedback.
##
## Phase 2 deliberately batches a complete biome drag into one staged history
## action. This layer mirrors each newly pending stamp to the disposable runtime
## biome preview, so terrain material and ecology respond while the mouse is down
## without reintroducing per-motion preset snapshots/autosaves or repeatedly
## copying the whole growing drag history.

signal biome_preview_stroke_added(layer_id: String, stroke: Dictionary)
signal biome_preview_transient_cleared


func _place_biome_stroke(direction: Vector3, continuous: bool) -> void:
	var before_count: int = _pending_biome_strokes.size()
	super._place_biome_stroke(direction, continuous)
	if _pending_biome_strokes.size() == before_count:
		return
	var newest: Dictionary = _pending_biome_strokes[_pending_biome_strokes.size() - 1]
	biome_preview_stroke_added.emit(_selected_biome_layer_id, newest.duplicate(true))


func _commit_biome_transaction() -> void:
	super._commit_biome_transaction()
	biome_preview_transient_cleared.emit()


func _discard_interactive_transactions() -> void:
	super._discard_interactive_transactions()
	biome_preview_transient_cleared.emit()
