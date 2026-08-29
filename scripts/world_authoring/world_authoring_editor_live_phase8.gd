class_name WorldAuthoringLiveEditorPhase8
extends "res://scripts/world_authoring/world_authoring_editor_live_phase7.gd"
## Phase 8 keeps Phase 7's final-height Smooth/Flatten math but publishes each
## absolute sparse-delta stamp through one Deltas mutex acquisition. Large shaping
## brushes therefore scale with the samples they actually evaluate instead of
## paying two lock/unlock pairs per lattice point.


func _apply_absolute_delta_writes(writes: Array[Dictionary], center_dir: Vector3,
		planet_radius: float) -> int:
	if writes.is_empty():
		return 0
	var changed: int = Deltas.set_offsets_batch(
		writes, SCULPT_MIN_OFFSET_M, SCULPT_MAX_OFFSET_M)
	if changed > 0:
		var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
		Deltas.notify_changed(
			center_dir.normalized(),
			_sculpt_radius_m + spacing_m * 4.0)
	return changed
