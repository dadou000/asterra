class_name WorldAuthoringLiveEditorPhase10
extends "res://scripts/world_authoring/world_authoring_editor_live_phase9.gd"
## Phase 10: non-persistent live sculpt performance telemetry.
##
## The authoring result is unchanged. Each completed synchronous brush stamp records
## how many sparse-lattice candidates passed the active radius/falloff, how many
## pristine generated-height evaluations were actually performed, and total CPU
## wall time. This gives Planet Studio real measurements before any large-brush
## sampling policy or asynchronous GPU patch path is introduced.

var _telemetry_candidate_count: int = 0
var _telemetry_pristine_evaluations: int = 0
var _telemetry_stamp_start_us: int = 0
var _telemetry_active_tool: String = ""
var _telemetry_last_ms: float = 0.0
var _telemetry_last_candidates: int = 0
var _telemetry_last_pristine: int = 0
var _telemetry_last_tool: String = ""


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Sculpt performance")
	var label := Label.new()
	if _telemetry_last_tool.is_empty():
		label.text = "No measured sculpt stamp yet."
	else:
		label.text = "%s • %.3f ms CPU • %d brush samples • %d pristine evaluations" % [
			_telemetry_last_tool,
			_telemetry_last_ms,
			_telemetry_last_candidates,
			_telemetry_last_pristine,
		]
	label.modulate = Color(0.64, 0.76, 0.86)
	_workspace.add_child(label)
	_add_note("Telemetry is runtime-only and never enters presets/history. It measures the complete synchronous stamp on this machine, including sample collection, generated-height evaluation, smoothing/flatten math and the batched sparse write. Use it to decide when a future large-brush GPU/worker path is justified.")


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	_begin_sculpt_telemetry("Raise/Lower")
	super._place_sculpt_stroke(direction, continuous, sign_value)
	_finish_sculpt_telemetry()


func _place_erase_stroke(direction: Vector3, continuous: bool) -> void:
	_begin_sculpt_telemetry("Erase")
	super._place_erase_stroke(direction, continuous)
	_finish_sculpt_telemetry()


func _place_smooth_stroke(direction: Vector3, continuous: bool) -> void:
	_begin_sculpt_telemetry("Smooth")
	super._place_smooth_stroke(direction, continuous)
	_finish_sculpt_telemetry()


func _place_flatten_stroke(direction: Vector3, continuous: bool) -> void:
	_begin_sculpt_telemetry("Flatten")
	super._place_flatten_stroke(direction, continuous)
	_finish_sculpt_telemetry()


func _collect_sculpt_samples(center_dir: Vector3, planet_radius: float) -> Array[Dictionary]:
	var samples: Array[Dictionary] = super._collect_sculpt_samples(center_dir, planet_radius)
	if not _telemetry_active_tool.is_empty():
		_telemetry_candidate_count = samples.size()
	return samples


func _generated_pristine_height(direction: Vector3) -> float:
	if not _telemetry_active_tool.is_empty():
		_telemetry_pristine_evaluations += 1
	return super._generated_pristine_height(direction)


func _begin_sculpt_telemetry(tool_name: String) -> void:
	_telemetry_active_tool = tool_name
	_telemetry_candidate_count = 0
	_telemetry_pristine_evaluations = 0
	_telemetry_stamp_start_us = Time.get_ticks_usec()


func _finish_sculpt_telemetry() -> void:
	if _telemetry_active_tool.is_empty():
		return
	var elapsed_us: int = maxi(0, Time.get_ticks_usec() - _telemetry_stamp_start_us)
	var tool_name: String = _telemetry_active_tool
	var candidate_count: int = _telemetry_candidate_count
	var pristine_count: int = _telemetry_pristine_evaluations
	_telemetry_active_tool = ""
	_telemetry_stamp_start_us = 0

	# A continuous drag can intentionally skip a stamp when the cursor has not moved
	# far enough. Do not overwrite useful telemetry with those zero-work events.
	if candidate_count <= 0:
		return
	_telemetry_last_tool = tool_name
	_telemetry_last_candidates = candidate_count
	_telemetry_last_pristine = pristine_count
	_telemetry_last_ms = float(elapsed_us) / 1000.0
	var suffix: String = "perf %.3f ms • %d samples" % [
		_telemetry_last_ms, candidate_count]
	if pristine_count > 0:
		suffix += " • %d pristine" % pristine_count
	if _status_label != null:
		_status_label.text = "%s • %s" % [_status_label.text, suffix]
