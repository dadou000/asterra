class_name PlanetHydrologyRiverCoupledStore
extends PlanetHydrologyRiverPromotionStore
## Persistent 1D river store with explicit holes for promoted sparse 2D river nodes.
##
## A promoted sparse tile represents the upstream/node portion of one macro reach.
## The remaining coarse channel storage represents the downstream residual segment.
## Donor/confluence inflow and the refined share of local lateral inflow are diverted
## into `refined_pending_inflow_m3` and remain coarse-owned until GPU acknowledgement.
## Fine outflow is accepted back into the downstream residual 1D segment. These are
## internal representation transfers and therefore update the inherited promotion /
## demotion ledgers rather than environmental input/output ledgers.

var refined_mask := PackedByteArray()
var refined_pending_inflow_m3 := PackedFloat64Array()
var refined_inflow_rate_m3s := PackedFloat64Array()
var _refined_step_inflow_m3 := PackedFloat64Array()
var _refined_records: Dictionary = {} # cell -> Dictionary


func initialize(p_fields: PlanetFields) -> Error:
	var err := super.initialize(p_fields)
	if err != OK:
		return err
	var n := cell_count()
	refined_mask.resize(n)
	refined_pending_inflow_m3.resize(n)
	refined_inflow_rate_m3s.resize(n)
	_refined_step_inflow_m3.resize(n)
	refined_mask.fill(0)
	refined_pending_inflow_m3.fill(0.0)
	refined_inflow_rate_m3s.fill(0.0)
	_refined_step_inflow_m3.fill(0.0)
	_refined_records.clear()
	return OK


func refined_reach_count() -> int:
	return _refined_records.size()


func is_refined_reach(cell: int) -> bool:
	return initialized and cell >= 0 and cell < refined_mask.size() \
		and refined_mask[cell] != 0


func register_refined_reach(cell: int, tile_id: int, slot: int,
		represented_volume_m3: float) -> Dictionary:
	if not initialized or river_reaches == null or not river_reaches.is_reach_cell(cell):
		return _ownership_error(ERR_INVALID_PARAMETER, "not_a_river_reach")
	if is_refined_reach(cell):
		return _ownership_error(ERR_ALREADY_EXISTS, "reach_already_refined")
	if tile_id < 0 or slot < 0 or not is_finite(represented_volume_m3) \
			or represented_volume_m3 <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_refined_reach")

	var storage_before_promotion := channel_storage_m3[cell] + represented_volume_m3
	var depth := river_reaches.depth_from_storage(cell,
		minf(storage_before_promotion, river_reaches.bankfull_storage_for_cell(cell)))
	var area := river_reaches.cross_section_area(cell, depth)
	var represented_length := represented_volume_m3 / maxf(area, 1.0e-9)
	represented_length = clampf(represented_length, 0.0,
		maxf(river_reaches.reach_length_m[cell], 0.0))
	var residual_length := maxf(river_reaches.reach_length_m[cell] - represented_length, 0.0)
	var record := {
		"cell": cell,
		"tile_id": tile_id,
		"slot": slot,
		"represented_volume_m3": represented_volume_m3,
		"represented_length_m": represented_length,
		"residual_length_m": residual_length,
		"reach_length_m": river_reaches.reach_length_m[cell],
		"receiver": receiver[cell],
	}
	_refined_records[cell] = record
	refined_mask[cell] = 1
	refined_pending_inflow_m3[cell] = 0.0
	refined_inflow_rate_m3s[cell] = 0.0
	_refined_step_inflow_m3[cell] = 0.0
	return {"error": OK, "record": record.duplicate(true)}


## Remove only the coarse-side coupling metadata. The caller must already have
## conservatively returned/remapped any authoritative fine water.
func unregister_refined_reach(cell: int,
		return_pending_to_channel: bool = true) -> Dictionary:
	if not is_refined_reach(cell):
		return _ownership_error(ERR_DOES_NOT_EXIST, "reach_not_refined")
	var pending := maxf(refined_pending_inflow_m3[cell], 0.0)
	if return_pending_to_channel and pending > 0.0:
		channel_storage_m3[cell] += pending
	refined_pending_inflow_m3[cell] = 0.0
	refined_inflow_rate_m3s[cell] = 0.0
	_refined_step_inflow_m3[cell] = 0.0
	refined_mask[cell] = 0
	var record := (_refined_records[cell] as Dictionary).duplicate(true)
	_refined_records.erase(cell)
	return {"error": OK, "record": record, "returned_pending_m3": pending}


func refined_reach_record(cell: int) -> Dictionary:
	if not is_refined_reach(cell):
		return {}
	return (_refined_records[cell] as Dictionary).duplicate(true)


func refined_inflow_available_m3(cell: int) -> float:
	return maxf(refined_pending_inflow_m3[cell], 0.0) if is_refined_reach(cell) else 0.0


func refined_inflow_rate(cell: int) -> float:
	return maxf(refined_inflow_rate_m3s[cell], 0.0) if is_refined_reach(cell) else 0.0


## GPU acknowledgement that pending coarse-owned inflow entered the fine river.
func consume_refined_inflow(cell: int, volume_m3: float) -> Dictionary:
	if not is_refined_reach(cell) or not is_finite(volume_m3) or volume_m3 < 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_refined_inflow_consume")
	var available := maxf(refined_pending_inflow_m3[cell], 0.0)
	var epsilon := maxf(1.0e-9, available * 1.0e-10)
	if volume_m3 > available + epsilon:
		return _ownership_error(ERR_CANT_ACQUIRE_RESOURCE, "refined_inflow_exceeds_pending")
	var consumed := minf(maxf(volume_m3, 0.0), available)
	refined_pending_inflow_m3[cell] = maxf(available - consumed, 0.0)
	cumulative_promoted_to_fine_m3 += consumed
	return {
		"error": OK,
		"cell": cell,
		"consumed_volume_m3": consumed,
		"pending_volume_m3": refined_pending_inflow_m3[cell],
	}


## GPU acknowledgement that fine river water left the refined node into the
## downstream residual 1D reach.
func accept_refined_outflow(cell: int, volume_m3: float) -> Dictionary:
	if not is_refined_reach(cell) or not is_finite(volume_m3) or volume_m3 < 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_refined_outflow")
	var accepted := maxf(volume_m3, 0.0)
	channel_storage_m3[cell] += accepted
	cumulative_demoted_from_fine_m3 += accepted
	return {
		"error": OK,
		"cell": cell,
		"accepted_volume_m3": accepted,
		"residual_channel_storage_m3": channel_storage_m3[cell],
	}


func total_storage_m3() -> float:
	var total := super.total_storage_m3()
	if not initialized:
		return total
	for c in cell_count():
		if refined_mask[c] != 0:
			total += maxf(refined_pending_inflow_m3[c], 0.0)
	return total


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
	_refined_step_inflow_m3.fill(0.0)
	refined_inflow_rate_m3s.fill(0.0)

	var step_precip := 0.0
	var step_climatology := 0.0
	var step_outlet := 0.0
	var step_infiltrated := 0.0
	var step_surface_runoff := 0.0
	var step_overbank_spill := 0.0
	var step_refined_inflow := 0.0
	var reach_cells_advanced := 0
	var max_surface_depth := 0.0
	var max_q := 0.0
	var max_reach_bankfull_ratio := 0.0
	var max_reach_depth := 0.0

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
		var baseflow_volume := baseflow_depth * area

		var surface_fraction := _reservoir_fraction(dt, surface_release_tau_s[c])
		var surface_to_channel := surface_storage_m3[c] * surface_fraction
		surface_storage_m3[c] = maxf(surface_storage_m3[c] - surface_to_channel, 0.0)

		var climate_volume := 0.0
		if use_climatology_fallback:
			climate_volume = maxf(local_climatology_q_m3s[c], 0.0) * dt
			step_climatology += climate_volume

		var lateral_channel := baseflow_volume + surface_to_channel + climate_volume
		if is_refined_reach(c):
			var record := _refined_records[c] as Dictionary
			var reach_length := maxf(float(record.get("reach_length_m", 0.0)), 1.0e-9)
			var fine_fraction := clampf(float(record.get("represented_length_m", 0.0))
				/ reach_length, 0.0, 1.0)
			var fine_lateral := lateral_channel * fine_fraction
			var residual_lateral := lateral_channel - fine_lateral
			refined_pending_inflow_m3[c] += fine_lateral
			_refined_step_inflow_m3[c] += fine_lateral
			step_refined_inflow += fine_lateral
			channel_storage_m3[c] += residual_lateral
		else:
			channel_storage_m3[c] += lateral_channel

		var r := receiver[c]
		var out_volume := 0.0
		if r == c:
			channel_discharge_m3s[c] = 0.0
		elif river_reaches.is_reach_cell(c):
			var reach_step: Dictionary
			if is_refined_reach(c):
				var rec := _refined_records[c] as Dictionary
				reach_step = _advance_residual_segment(c, channel_storage_m3[c], dt,
					maxf(float(rec.get("residual_length_m", 0.0)), 0.0))
			else:
				reach_step = river_reaches.advance_cell(c, channel_storage_m3[c], dt)
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

	# Donor/confluence water entering a refined node becomes pending fine inflow.
	for c in n:
		if fields.elev[c] <= 0.0 or _routing_inflow_m3[c] <= 0.0:
			continue
		if is_refined_reach(c):
			var incoming := _routing_inflow_m3[c]
			refined_pending_inflow_m3[c] += incoming
			_refined_step_inflow_m3[c] += incoming
			step_refined_inflow += incoming
		else:
			channel_storage_m3[c] += _routing_inflow_m3[c]

	for c in n:
		if is_refined_reach(c):
			refined_inflow_rate_m3s[c] = _refined_step_inflow_m3[c] / dt

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
		"river_refined_pending_inflow_m3": step_refined_inflow,
		"river_refined_count": refined_reach_count(),
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


func _advance_residual_segment(cell: int, storage_m3: float,
		dt_s: float, length_m: float) -> Dictionary:
	var volume := maxf(storage_m3, 0.0)
	var length := maxf(length_m, 0.0)
	if length <= 1.0e-6:
		return {
			"error": OK,
			"remaining_storage_m3": 0.0,
			"outflow_volume_m3": volume,
			"discharge_m3s": volume / maxf(dt_s, 1.0e-6),
			"spill_to_surface_m3": 0.0,
			"depth_after_m": 0.0,
			"bankfull_depth_ratio": 0.0,
		}

	var bank_area := river_reaches.cross_section_area(cell,
		river_reaches.bankfull_depth_m[cell])
	var bank_storage := bank_area * length
	var spill := maxf(volume - bank_storage, 0.0)
	volume = minf(volume, bank_storage)
	var area := volume / length
	var depth := _depth_from_area(cell, area)
	var q_capacity := river_reaches.discharge_for_depth(cell, depth)
	var velocity := q_capacity / maxf(area, 1.0e-9)
	var celerity := maxf(velocity * maxf(river_reaches.kinematic_celerity_multiplier, 1.0),
		river_reaches.minimum_travel_velocity_mps)
	var tau := maxf(length / maxf(celerity, 1.0e-6), 1.0)
	var fraction := clampf(1.0 - exp(-dt_s / tau), 0.0, 1.0)
	var out_volume := minf(volume, minf(q_capacity * dt_s, volume * fraction))
	var remaining := maxf(volume - out_volume, 0.0)
	var depth_after := _depth_from_area(cell, remaining / length)
	return {
		"error": OK,
		"remaining_storage_m3": remaining,
		"outflow_volume_m3": out_volume,
		"discharge_m3s": out_volume / maxf(dt_s, 1.0e-6),
		"spill_to_surface_m3": spill,
		"depth_after_m": depth_after,
		"bankfull_depth_ratio": depth_after /
			maxf(river_reaches.bankfull_depth_m[cell], 1.0e-9),
	}


func _depth_from_area(cell: int, cross_area_m2: float) -> float:
	var a := maxf(cross_area_m2, 0.0)
	var b := maxf(river_reaches.bottom_width_m[cell], 1.0)
	var z := maxf(river_reaches.side_slope_h_over_v, 0.0)
	if z <= 1.0e-9:
		return a / b
	return maxf((-b + sqrt(maxf(b * b + 4.0 * z * a, 0.0))) / (2.0 * z), 0.0)


func cell_state(cell: int) -> Dictionary:
	var state := super.cell_state(cell)
	if state.is_empty():
		return state
	state["river_refined"] = is_refined_reach(cell)
	if is_refined_reach(cell):
		state["river_refinement"] = refined_reach_record(cell)
		state["river_pending_inflow_m3"] = refined_pending_inflow_m3[cell]
		state["river_inflow_rate_m3s"] = refined_inflow_rate_m3s[cell]
	return state


func snapshot() -> Dictionary:
	# Fine river state is owned by the sparse runtime; a coarse-only snapshot cannot
	# safely serialize this split representation yet.
	if refined_reach_count() > 0:
		return {}
	return super.snapshot()


func stats() -> Dictionary:
	var out := super.stats()
	var pending := 0.0
	for c in cell_count():
		if refined_mask[c] != 0:
			pending += maxf(refined_pending_inflow_m3[c], 0.0)
	out["river_refinement"] = {
		"active_reaches": refined_reach_count(),
		"pending_inflow_m3": pending,
		"snapshot_blocked": refined_reach_count() > 0,
		"coupling": "coarse_donors_to_fine_node_to_residual_1d",
	}
	return out
