extends Node
## CPU/headless gate for HydroRiverCollapsePolicy.

var _failures: Array[String] = []


func _ready() -> void:
	var base := {
		"state": HydroTilePool.TileState.SETTLING,
		"quiet_time_s": 25.0,
		"max_velocity_mps": 0.004,
		"max_outgoing_flux_m3s": 0.003,
		"disturbance_energy": 1.0e-5,
	}
	_expect(HydroRiverCollapsePolicy.eligible_record(base),
		"quiet settling river should be eligible")

	var active := base.duplicate(true)
	active["state"] = HydroTilePool.TileState.ACTIVE
	_expect(not HydroRiverCollapsePolicy.eligible_record(active),
		"ACTIVE river must not collapse")

	var young := base.duplicate(true)
	young["quiet_time_s"] = 5.0
	_expect(not HydroRiverCollapsePolicy.eligible_record(young),
		"insufficient quiet time was accepted")

	var fast := base.duplicate(true)
	fast["max_velocity_mps"] = 0.03
	_expect(not HydroRiverCollapsePolicy.eligible_record(fast),
		"fast river was accepted")

	var fluxing := base.duplicate(true)
	fluxing["max_outgoing_flux_m3s"] = 0.05
	_expect(not HydroRiverCollapsePolicy.eligible_record(fluxing),
		"high-flux river was accepted")

	var disturbed := base.duplicate(true)
	disturbed["disturbance_energy"] = 1.0e-3
	_expect(not HydroRiverCollapsePolicy.eligible_record(disturbed),
		"disturbed river was accepted")

	var frozen := base.duplicate(true)
	frozen["state"] = HydroTilePool.TileState.FROZEN_WATER
	_expect(HydroRiverCollapsePolicy.eligible_record(frozen),
		"quiet frozen river should be eligible")

	_expect(HydroRiverCollapsePolicy.eligible_record(active,
		20.0, 0.01, 0.01, 5.0e-5, false),
		"explicit policy override did not allow ACTIVE state")

	if _failures.is_empty():
		print("HYDRO_RIVER_COLLAPSE_POLICY: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_RIVER_COLLAPSE_POLICY: " + failure)
		get_tree().quit(1)


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
