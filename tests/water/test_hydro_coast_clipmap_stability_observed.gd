extends "res://tests/water/test_hydro_coast_clipmap_stability.gd"
## Adds per-movement observation to the base playable-world harness. The base test
## intentionally keeps expensive invariant checks at milestones; this wrapper also
## checks the frame after every near-field player move so ordinary toroidal lattice
## snaps are counted rather than only the phase endpoints.

var _pending_move_check := false
var _pending_move_index := 0


func _process(delta: float) -> void:
	# We run at priority 20. By the next frame OceanSystem(10) and WaterSystem(11)
	# have already consumed the player move and published their updated frames.
	if _pending_move_check and not _finished:
		_check_invariants("lattice move %d" % _pending_move_index)
		_pending_move_check = false
		if _finished:
			return

	var before_snap := _snap_index
	super._process(delta)
	if not _finished and _snap_index > before_snap:
		_pending_move_index = _snap_index
		_pending_move_check = true
