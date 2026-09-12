class_name HydroRiverCollapsePolicy
extends RefCounted
## Pure CPU eligibility rule for collapsing one refined sparse river tile back to 1D.


static func eligible_record(record: Dictionary,
		minimum_quiet_time_s: float = 20.0,
		maximum_velocity_mps: float = 0.01,
		maximum_outgoing_flux_m3s: float = 0.01,
		maximum_disturbance_energy: float = 5.0e-5,
		require_settling_state: bool = true) -> bool:
	if record.is_empty():
		return false
	if require_settling_state:
		var state := int(record.get("state", HydroTilePool.TileState.ACTIVE))
		if state not in [HydroTilePool.TileState.SETTLING,
				HydroTilePool.TileState.FROZEN_WATER]:
			return false
	if float(record.get("quiet_time_s", 0.0)) < maxf(minimum_quiet_time_s, 0.0):
		return false
	if float(record.get("max_velocity_mps", INF)) > maxf(maximum_velocity_mps, 0.0):
		return false
	if float(record.get("max_outgoing_flux_m3s", INF)) \
			> maxf(maximum_outgoing_flux_m3s, 0.0):
		return false
	if float(record.get("disturbance_energy", INF)) \
			> maxf(maximum_disturbance_energy, 0.0):
		return false
	return true
