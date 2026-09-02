extends Node
## CPU/headless policy gate for quiet automatic surface collapse.

var _failures: Array[String] = []


func _ready() -> void:
	_test_activity_gate()
	_test_neighbor_gate()
	if _failures.is_empty():
		print("HYDRO_AUTOMATIC_SURFACE_DEMOTION: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_AUTOMATIC_SURFACE_DEMOTION: " + failure)
		get_tree().quit(1)


func _test_activity_gate() -> void:
	var base := {
		"state": HydroTilePool.TileState.SETTLING,
		"quiet_time_s": 30.0,
		"max_depth_m": 0.08,
		"max_velocity_mps": 0.002,
		"max_outgoing_flux_m3s": 0.001,
		"disturbance_energy": 1.0e-5,
	}
	_expect(HydroAutomaticSurfaceDemotionPolicy.quiet_surface_record_eligible(
		base, 20.0, 0.15, 0.004, 0.002, 2.5e-5),
		"quiet settling surface tile was rejected")

	var frozen := base.duplicate(true)
	frozen["state"] = HydroTilePool.TileState.FROZEN_WATER
	_expect(HydroAutomaticSurfaceDemotionPolicy.quiet_surface_record_eligible(
		frozen, 20.0, 0.15, 0.004, 0.002, 2.5e-5),
		"quiet frozen surface tile was rejected")

	var active := base.duplicate(true)
	active["state"] = HydroTilePool.TileState.ACTIVE
	_expect(not HydroAutomaticSurfaceDemotionPolicy.quiet_surface_record_eligible(
		active, 20.0, 0.15, 0.004, 0.002, 2.5e-5),
		"ACTIVE tile was accepted for automatic demotion")

	for mutation in [
		["quiet_time_s", 19.9, "insufficient quiet time"],
		["max_depth_m", 0.151, "deep surface water"],
		["max_velocity_mps", 0.0041, "excess velocity"],
		["max_outgoing_flux_m3s", 0.0021, "excess outgoing flux"],
		["disturbance_energy", 2.6e-5, "excess disturbance"],
	]:
		var record := base.duplicate(true)
		record[mutation[0]] = mutation[1]
		_expect(not HydroAutomaticSurfaceDemotionPolicy.quiet_surface_record_eligible(
			record, 20.0, 0.15, 0.004, 0.002, 2.5e-5),
			"%s did not suppress automatic demotion" % String(mutation[2]))


func _test_neighbor_gate() -> void:
	var pool := HydroTilePool.new(4)
	# Pick an east-edge tile so the east neighbor crosses a cube-face seam.
	var key := HydroTileKey.new(0, 2, 3, 1)
	var slot := pool.allocate(key)
	_expect(slot >= 0, "failed to allocate centre tile")
	pool.set_state(key, HydroTilePool.TileState.SETTLING, "test")
	_expect(not HydroAutomaticSurfaceDemotionPolicy.has_resident_cardinal_neighbor(
		key, pool), "isolated tile reported a resident neighbor")

	var link := HydroTileTopology.neighbor(key, HydroTileTopology.DIR_EAST)
	_expect(not link.is_empty(), "cross-face east neighbor did not resolve")
	if link.is_empty():
		return
	var neighbor: HydroTileKey = link.get("key", null)
	_expect(neighbor != null, "cross-face neighbor key missing")
	if neighbor == null:
		return
	_expect(neighbor.face != key.face,
		"edge fixture did not actually cross a cube face")
	_expect(pool.allocate(neighbor) >= 0, "failed to allocate cross-face neighbor")
	_expect(HydroAutomaticSurfaceDemotionPolicy.has_resident_cardinal_neighbor(
		key, pool), "resident cross-face neighbor was not detected")
	pool.release(neighbor)
	_expect(not HydroAutomaticSurfaceDemotionPolicy.has_resident_cardinal_neighbor(
		key, pool), "released neighbor still suppressed collapse")


func _expect(condition: bool, message: String) -> void:
	if not condition and not _failures.has(message):
		_failures.append(message)
