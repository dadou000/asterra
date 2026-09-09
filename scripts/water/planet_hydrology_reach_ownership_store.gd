class_name PlanetHydrologyReachOwnershipStore
extends PlanetHydrologyOwnershipStore
## Production coarse hydrology with a persistent 1D river/reach representation.
##
## Soil and sheet runoff remain coarse 0D/2D reservoirs. Generated macro rivers
## (`fields.river_width > 0`, currently Q >= 1.5 m^3/s from PassHydrology) use
## PlanetRiverReachStore for channel geometry and routing. The authoritative channel
## water is still `channel_storage_m3`; the reach model does not duplicate dynamic
## storage, so all inherited coarse<->fine ownership transactions remain valid.

var river_reaches: PlanetRiverReachStore


func initialize(p_fields: PlanetFields) -> Error:
	var err := super.initialize(p_fields)
	if err != OK:
		return err
	river_reaches = PlanetRiverReachStore.new()
	err = river_reaches.initialize(fields, receiver)
	if err != OK:
		river_reaches = null
		return err
	# Base initialization used the previous 0D channel residence-time storage.
	# Replace only generated macro-river cells with the calibrated 1D normal-flow
	# storage; tributary/sub-grid cells retain the original reservoir state.
	for c in cell_count():
		if river_reaches.is_reach_cell(c):
			channel_storage_m3[c] = river_reaches.normal_storage_for_cell(c)
			channel_discharge_m3s[c] = maxf(float(fields.discharge[c]), 0.0)
	initial_storage_m3 = total_storage_m3()
	return OK


func step(dt_s: float) -> Dictionary:
	if pending_ownership_transaction_count() > 0:
		return {
			"error": ERR_BUSY,
			"reason": "ownership_transaction_pending",
			"pending_promotions": pending_promotion_count(),
			"pending_demotions": pending_demotion_count(),
		}
	if not initialized or river_reaches == null or not river_reaches.initialized \
			or not is_finite(dt_s) or dt_s <= 0.0:
		return {"error": ERR_INVALID_PARAMETER}

	var dt := maxf(dt_s, 1.0e-6)
	var n := cell_count()
	_routing_inflow_m3.fill(0.0)

	var step_precip := 0.0
	var step_climatology := 0.0
	var step_outlet := 0.0
	var step_infiltrated := 0.0
	var step_surface_runoff := 0.0
	var step_overbank_spill := 0.0
	var reach_cells_advanced := 0
	var max_surface_depth := 0.0
	var max_q := 0.0
	var max_reach_bankfull_ratio := 0.0
	var max_reach_depth := 0.0

	# Local vertical water balance and one-hop channel routing. Routed inflow is
	# buffered until the end so one coarse call cannot teleport water through many
	# reaches even under large time warp.
	for c in n:
		channel_discharge_m3s[c] = 0.0
		if fields.elev[c] <= 0.0:
			continue

		var area := maxf(area_m2[c], 1.0)
		var rain_rate := maxf(precipitation_mps[c], 0.0)
		var rain_depth := rain_rate * dt
		var rain_volume := rain_depth * area
		step_precip += rain_volume

		var capacity := maxf(soil_capacity_m[c], 1.0e-6)
		var saturation := clampf(soil_water_m[c] / capacity, 0.0, 1.0)
		var conductivity_scale := clampf(1.0 - 0.72 * saturation, 0.12, 1.0)
		var infiltration_depth := minf(rain_depth,
			infiltration_capacity_mps[c] * conductivity_scale * dt)
		infiltration_depth = minf(infiltration_depth,
			maxf(capacity - soil_water_m[c], 0.0))
		soil_water_m[c] += infiltration_depth
		var infiltrated_volume := infiltration_depth * area
		step_infiltrated += infiltrated_volume

		var runoff_volume := maxf(rain_volume - infiltrated_volume, 0.0)
		surface_storage_m3[c] += runoff_volume
		step_surface_runoff += runoff_volume

		var soil_fraction := _reservoir_fraction(dt, soil_baseflow_tau_s[c])
		var baseflow_depth := soil_water_m[c] * soil_fraction
		soil_water_m[c] = maxf(soil_water_m[c] - baseflow_depth, 0.0)
		channel_storage_m3[c] += baseflow_depth * area

		var surface_fraction := _reservoir_fraction(dt, surface_release_tau_s[c])
		var surface_to_channel := surface_storage_m3[c] * surface_fraction
		surface_storage_m3[c] = maxf(surface_storage_m3[c] - surface_to_channel, 0.0)
		channel_storage_m3[c] += surface_to_channel

		if use_climatology_fallback:
			var climate_volume := maxf(local_climatology_q_m3s[c], 0.0) * dt
			channel_storage_m3[c] += climate_volume
			step_climatology += climate_volume

		var r := receiver[c]
		var out_volume := 0.0
		if r == c:
			# Closed/degenerate land outlet remains a lumped basin. Lakes are kept out
			# of the 1D reach mask and will get their own level-volume representation.
			channel_discharge_m3s[c] = 0.0
		elif river_reaches.is_reach_cell(c):
			var reach_step := river_reaches.advance_cell(c, channel_storage_m3[c], dt)
			if int(reach_step.get("error", FAILED)) != OK:
				return {"error": ERR_INVALID_DATA, "reason": "river_reach_step_failed", "cell": c}
			channel_storage_m3[c] = float(reach_step["remaining_storage_m3"])
			var spill := maxf(float(reach_step["spill_to_surface_m3"]), 0.0)
			if spill > 0.0:
				surface_storage_m3[c] += spill
				step_overbank_spill += spill
			out_volume = maxf(float(reach_step["outflow_volume_m3"]), 0.0)
			channel_discharge_m3s[c] = maxf(float(reach_step["discharge_m3s"]), 0.0)
			reach_cells_advanced += 1
			max_reach_bankfull_ratio = maxf(max_reach_bankfull_ratio,
				float(reach_step["bankfull_depth_ratio"]))
			max_reach_depth = maxf(max_reach_depth, float(reach_step["depth_after_m"]))
		else:
			# Sub-grid tributary/stream: retain the earlier lumped reservoir until it
			# enters a generated macro reach. This keeps tiny drainage O(1) per cell.
			var channel_fraction := _reservoir_fraction(dt, channel_release_tau_s[c])
			out_volume = channel_storage_m3[c] * channel_fraction
			channel_storage_m3[c] = maxf(channel_storage_m3[c] - out_volume, 0.0)
			channel_discharge_m3s[c] = out_volume / dt

		max_q = maxf(max_q, channel_discharge_m3s[c])
		if out_volume > 0.0:
			if r < 0 or r >= n or fields.elev[r] <= 0.0:
				step_outlet += out_volume
			else:
				_routing_inflow_m3[r] += out_volume

		max_surface_depth = maxf(max_surface_depth, surface_storage_m3[c] / area)

	# One-hop routing: incoming channel water becomes receiver storage only after all
	# source reaches selected their outflow, so it cannot move a second reach in the
	# same coarse macro step.
	for c in n:
		if fields.elev[c] > 0.0 and _routing_inflow_m3[c] > 0.0:
			channel_storage_m3[c] += _routing_inflow_m3[c]

	cumulative_precipitation_m3 += step_precip
	cumulative_climatology_input_m3 += step_climatology
	cumulative_outlet_m3 += step_outlet
	simulated_seconds += dt
	step_count += 1

	var report := {
		"error": OK,
		"dt_s": dt,
		"precipitation_input_m3": step_precip,
		"climatology_input_m3": step_climatology,
		"infiltrated_m3": step_infiltrated,
		"surface_runoff_created_m3": step_surface_runoff,
		"river_overbank_spill_m3": step_overbank_spill,
		"river_reaches_advanced": reach_cells_advanced,
		"outlet_to_ocean_m3": step_outlet,
		"max_surface_storage_depth_m": max_surface_depth,
		"max_channel_discharge_m3s": max_q,
		"max_reach_depth_m": max_reach_depth,
		"max_reach_bankfull_ratio": max_reach_bankfull_ratio,
		"storage_m3": total_storage_m3(),
		"mass_error_m3": mass_error_m3(),
	}
	last_step_report = report
	return report


func cell_state(cell: int) -> Dictionary:
	var state := super.cell_state(cell)
	if state.is_empty():
		return state
	var reach := {} if river_reaches == null \
		else river_reaches.reach_state(cell, channel_storage_m3[cell])
	state["river_reach"] = reach
	state["river_reach_active"] = not reach.is_empty()
	if not reach.is_empty():
		state["river_width_m"] = float(reach.get("bottom_width_m", 0.0))
		state["river_depth_m"] = float(reach.get("depth_m", 0.0))
		state["river_stage_m"] = float(reach.get("stage_m", fields.elev[cell]))
		state["river_bankfull_ratio"] = float(reach.get("bankfull_depth_ratio", 0.0))
	return state


## Channel-only anomaly query for the future 1D->2D reach promotion bridge. This is
## intentionally separate from automatic surface/flood promotion so high Q alone
## cannot be mistaken for a flat flood parcel.
func channel_reach_candidates(max_count: int = 64,
		discharge_ratio_threshold: float = 2.0,
		bankfull_ratio_threshold: float = 0.85) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if not initialized or river_reaches == null or max_count <= 0:
		return out
	var q_threshold := maxf(discharge_ratio_threshold, 1.0)
	var stage_threshold := clampf(bankfull_ratio_threshold, 0.05, 1.5)
	for c in cell_count():
		if not river_reaches.is_reach_cell(c):
			continue
		var reach := river_reaches.reach_state(c, channel_storage_m3[c])
		var baseline_q := maxf(float(fields.discharge[c]), 0.05)
		var q_ratio := channel_discharge_m3s[c] / baseline_q
		var bank_ratio := float(reach.get("bankfull_depth_ratio", 0.0))
		var score := maxf(q_ratio / q_threshold, bank_ratio / stage_threshold)
		if score < 1.0:
			continue
		_insert_bounded_candidate(out, {
			"cell": c,
			"direction": grid.cell_dir(c),
			"receiver": receiver[c],
			"score": score,
			"discharge_m3s": channel_discharge_m3s[c],
			"baseline_discharge_m3s": float(fields.discharge[c]),
			"discharge_ratio": q_ratio,
			"river_width_m": float(reach.get("bottom_width_m", 0.0)),
			"river_depth_m": float(reach.get("depth_m", 0.0)),
			"river_stage_m": float(reach.get("stage_m", 0.0)),
			"bankfull_depth_m": float(reach.get("bankfull_depth_m", 0.0)),
			"bankfull_ratio": bank_ratio,
			"stream_order": int(fields.stream_order[c]),
			"channel_storage_m3": channel_storage_m3[c],
		}, max_count)
	return out


func stats() -> Dictionary:
	var out := super.stats()
	out["river_reaches"] = {} if river_reaches == null else river_reaches.stats()
	out["channel_representation"] = "hybrid_subgrid_0d_macro_river_1d"
	return out
