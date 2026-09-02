class_name PlanetHydrologyStore
extends RefCounted
## Persistent coarse planetary hydrology on the generated PlanetGrid drainage graph.
##
## This is the representation below fine sparse SWE. Every macro land cell owns a
## small amount of persistent water state; runoff is routed through the exact
## `flow_dir`/`grid.nbr` graph baked by PassHydrology. Camera position is irrelevant.
##
## State is intentionally conservative and low-order:
##   atmospheric precipitation -> soil bucket -> delayed baseflow
##                              -> surface runoff -> channel reservoir -> receiver
##
## Fine SWE promotion is a consumer of this state, never its storage backend.
## Snow/rain phase is not guessed here. Until WeatherSystem publishes the native
## surface-temperature/snow field, precipitation input is explicitly treated as
## liquid-water-equivalent. The state layout already leaves phase separation to a
## future upstream forcing stage rather than baking a latitude/elevation heuristic.

const SNAPSHOT_VERSION := 1
const SECONDS_PER_DAY := 86400.0
const MM_H_TO_M_S := 1.0e-3 / 3600.0

var soil_capacity_min_m := 0.015
var soil_capacity_max_m := 0.65
var infiltration_min_mm_h := 1.0
var infiltration_max_mm_h := 60.0
var soil_baseflow_tau_min_days := 2.5
var soil_baseflow_tau_max_days := 24.0
var surface_release_tau_min_s := 900.0
var surface_release_tau_max_s := 21600.0
var channel_velocity_min_mps := 0.35
var channel_velocity_max_mps := 4.5
var lake_residence_multiplier := 18.0

var fields: PlanetFields
var grid: PlanetGrid
var initialized := false
var use_climatology_fallback := false

# Static metric/topology arrays.
var area_m2 := PackedFloat64Array()
var receiver := PackedInt32Array()
var route_order := PackedInt32Array() # outlets first, headwaters last
var soil_capacity_m := PackedFloat64Array()
var infiltration_capacity_mps := PackedFloat64Array()
var soil_baseflow_tau_s := PackedFloat64Array()
var surface_release_tau_s := PackedFloat64Array()
var channel_release_tau_s := PackedFloat64Array()
var local_climatology_q_m3s := PackedFloat64Array()

# Persistent dynamic state.
var precipitation_mps := PackedFloat64Array()
var soil_water_m := PackedFloat64Array()
var surface_storage_m3 := PackedFloat64Array()
var channel_storage_m3 := PackedFloat64Array()
var channel_discharge_m3s := PackedFloat64Array()

# Scratch reused every step.
var _routing_inflow_m3 := PackedFloat64Array()

# Conservative ledger. Initial storage is a baseline, then every external input
# and ocean outlet is integrated explicitly.
var initial_storage_m3 := 0.0
var cumulative_precipitation_m3 := 0.0
var cumulative_climatology_input_m3 := 0.0
var cumulative_outlet_m3 := 0.0
var simulated_seconds := 0.0
var step_count := 0
var last_step_report: Dictionary = {}


func initialize(p_fields: PlanetFields) -> Error:
	if p_fields == null or p_fields.grid == null:
		return ERR_INVALID_PARAMETER
	if p_fields.flow_dir.size() != p_fields.grid.cell_count \
			or p_fields.elev.size() != p_fields.grid.cell_count:
		return ERR_INVALID_DATA

	fields = p_fields
	grid = p_fields.grid
	var n := grid.cell_count
	_resize_all(n)
	_build_topology()
	_build_static_hydraulic_properties()
	_initialize_dynamic_state()
	initialized = true
	return OK


func cell_count() -> int:
	return 0 if grid == null else grid.cell_count


func set_precipitation_field_mps(values: PackedFloat64Array) -> Error:
	if not initialized or values.size() != cell_count():
		return ERR_INVALID_PARAMETER
	precipitation_mps = values.duplicate()
	for i in precipitation_mps.size():
		var value := precipitation_mps[i]
		precipitation_mps[i] = maxf(value, 0.0) if is_finite(value) else 0.0
	return OK


func set_precipitation_field_f32_mps(values: PackedFloat32Array) -> Error:
	if not initialized or values.size() != cell_count():
		return ERR_INVALID_PARAMETER
	for i in values.size():
		var value := float(values[i])
		precipitation_mps[i] = maxf(value, 0.0) if is_finite(value) else 0.0
	return OK


func clear_precipitation() -> void:
	if not precipitation_mps.is_empty():
		precipitation_mps.fill(0.0)


func set_climatology_fallback_enabled(enabled: bool) -> void:
	use_climatology_fallback = enabled


func step(dt_s: float) -> Dictionary:
	if not initialized or not is_finite(dt_s) or dt_s <= 0.0:
		return {"error": ERR_INVALID_PARAMETER}
	var dt := maxf(dt_s, 1.0e-6)
	var n := cell_count()
	_routing_inflow_m3.fill(0.0)

	var step_precip := 0.0
	var step_climatology := 0.0
	var step_outlet := 0.0
	var step_infiltrated := 0.0
	var step_surface_runoff := 0.0
	var max_surface_depth := 0.0
	var max_q := 0.0

	# Local vertical water balance and channel release. Downstream inflow is held in
	# a separate buffer so water received during this macro step cannot teleport
	# through an arbitrary number of PlanetGrid cells in the same call.
	for c in n:
		channel_discharge_m3s[c] = 0.0
		if fields.elev[c] <= 0.0:
			continue

		var area := maxf(area_m2[c], 1.0)
		var rain_rate := maxf(precipitation_mps[c], 0.0)
		var rain_depth := rain_rate * dt
		var rain_volume := rain_depth * area
		step_precip += rain_volume

		# Soil infiltration is capacity- and saturation-limited. It only intercepts
		# current precipitation; already-ponded surface/channel water is never deleted.
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

		# Slow soil drainage is the first persistent baseflow reservoir. No ET is
		# removed yet; SurfaceEnergy/vegetation will become a separate conservative
		# sink when that coupling is implemented.
		var soil_fraction := _reservoir_fraction(dt, soil_baseflow_tau_s[c])
		var baseflow_depth := soil_water_m[c] * soil_fraction
		soil_water_m[c] = maxf(soil_water_m[c] - baseflow_depth, 0.0)
		channel_storage_m3[c] += baseflow_depth * area

		# Sheet/surface storage enters the macro channel network gradually. Flat
		# floodplains deliberately retain it much longer than steep terrain.
		var surface_fraction := _reservoir_fraction(dt, surface_release_tau_s[c])
		var surface_to_channel := surface_storage_m3[c] * surface_fraction
		surface_storage_m3[c] = maxf(surface_storage_m3[c] - surface_to_channel, 0.0)
		channel_storage_m3[c] += surface_to_channel

		# When the native weather backend is unavailable, keep the generated mean
		# drainage network alive from the *local* climatological contribution. This
		# reconstructs the generator's upstream accumulation without double-counting
		# upstream discharge at every cell.
		if use_climatology_fallback:
			var climate_volume := maxf(local_climatology_q_m3s[c], 0.0) * dt
			channel_storage_m3[c] += climate_volume
			step_climatology += climate_volume

		var r := receiver[c]
		if r == c and fields.elev[c] > 0.0:
			# Rare closed/degenerate land outlet: retain storage as a coarse lake/basin.
			channel_discharge_m3s[c] = 0.0
		else:
			var channel_fraction := _reservoir_fraction(dt, channel_release_tau_s[c])
			var out_volume := channel_storage_m3[c] * channel_fraction
			channel_storage_m3[c] = maxf(channel_storage_m3[c] - out_volume, 0.0)
			channel_discharge_m3s[c] = out_volume / dt
			max_q = maxf(max_q, channel_discharge_m3s[c])
			if r == c or r < 0 or r >= n or fields.elev[r] <= 0.0:
				step_outlet += out_volume
			else:
				_routing_inflow_m3[r] += out_volume

		max_surface_depth = maxf(max_surface_depth,
			surface_storage_m3[c] / area)

	# Apply routed water only after every source cell has chosen its outflow.
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
		"outlet_to_ocean_m3": step_outlet,
		"max_surface_storage_depth_m": max_surface_depth,
		"max_channel_discharge_m3s": max_q,
		"storage_m3": total_storage_m3(),
		"mass_error_m3": mass_error_m3(),
	}
	last_step_report = report
	return report


func total_storage_m3() -> float:
	if not initialized:
		return 0.0
	var total := 0.0
	for c in cell_count():
		if fields.elev[c] <= 0.0:
			continue
		total += maxf(soil_water_m[c], 0.0) * maxf(area_m2[c], 1.0)
		total += maxf(surface_storage_m3[c], 0.0)
		total += maxf(channel_storage_m3[c], 0.0)
	return total


func mass_error_m3() -> float:
	if not initialized:
		return 0.0
	var expected := initial_storage_m3 + cumulative_precipitation_m3 \
		+ cumulative_climatology_input_m3 - cumulative_outlet_m3
	return expected - total_storage_m3()


func mass_relative_error() -> float:
	var scale := maxf(initial_storage_m3 + cumulative_precipitation_m3 \
		+ cumulative_climatology_input_m3, 1.0)
	return absf(mass_error_m3()) / scale


func cell_state(cell: int) -> Dictionary:
	if not initialized or cell < 0 or cell >= cell_count():
		return {}
	var area := maxf(area_m2[cell], 1.0)
	return {
		"cell": cell,
		"direction": grid.cell_dir(cell),
		"receiver": receiver[cell],
		"soil_water_m": soil_water_m[cell],
		"soil_capacity_m": soil_capacity_m[cell],
		"surface_storage_m3": surface_storage_m3[cell],
		"surface_storage_depth_m": surface_storage_m3[cell] / area,
		"channel_storage_m3": channel_storage_m3[cell],
		"channel_discharge_m3s": channel_discharge_m3s[cell],
		"baseline_discharge_m3s": float(fields.discharge[cell]),
		"floodplain": float(fields.floodplain[cell]),
		"stream_order": int(fields.stream_order[cell]),
	}


## Bounded debug/promotion query. Production promotion policy can call this at a
## low cadence; it does not participate in the O(N) hydrology step itself.
func promotion_candidates(max_count: int = 64,
		surface_depth_threshold_m: float = 0.025,
		discharge_ratio_threshold: float = 2.0) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if not initialized or max_count <= 0:
		return out
	var depth_threshold := maxf(surface_depth_threshold_m, 1.0e-4)
	var ratio_threshold := maxf(discharge_ratio_threshold, 1.0)
	for c in cell_count():
		if fields.elev[c] <= 0.0:
			continue
		var area := maxf(area_m2[c], 1.0)
		var surface_depth := surface_storage_m3[c] / area
		var baseline_q := maxf(float(fields.discharge[c]), 0.05)
		var discharge_ratio := channel_discharge_m3s[c] / baseline_q
		var depth_score := surface_depth / depth_threshold
		var q_score := discharge_ratio / ratio_threshold
		var score := maxf(depth_score, q_score)
		if score < 1.0:
			continue
		var candidate := {
			"cell": c,
			"direction": grid.cell_dir(c),
			"score": score,
			"surface_depth_m": surface_depth,
			"discharge_m3s": channel_discharge_m3s[c],
			"baseline_discharge_m3s": float(fields.discharge[c]),
			"floodplain": float(fields.floodplain[c]),
			"stream_order": int(fields.stream_order[c]),
		}
		_insert_bounded_candidate(out, candidate, max_count)
	return out


func snapshot() -> Dictionary:
	if not initialized:
		return {}
	return {
		"version": SNAPSHOT_VERSION,
		"cell_count": cell_count(),
		"soil_water_m": soil_water_m.duplicate(),
		"surface_storage_m3": surface_storage_m3.duplicate(),
		"channel_storage_m3": channel_storage_m3.duplicate(),
		"channel_discharge_m3s": channel_discharge_m3s.duplicate(),
		"initial_storage_m3": initial_storage_m3,
		"cumulative_precipitation_m3": cumulative_precipitation_m3,
		"cumulative_climatology_input_m3": cumulative_climatology_input_m3,
		"cumulative_outlet_m3": cumulative_outlet_m3,
		"simulated_seconds": simulated_seconds,
		"step_count": step_count,
	}


func restore_snapshot(data: Dictionary) -> Error:
	if not initialized or int(data.get("version", -1)) != SNAPSHOT_VERSION \
			or int(data.get("cell_count", -1)) != cell_count():
		return ERR_INVALID_DATA
	var soil_value: Variant = data.get("soil_water_m", null)
	var surface_value: Variant = data.get("surface_storage_m3", null)
	var channel_value: Variant = data.get("channel_storage_m3", null)
	var discharge_value: Variant = data.get("channel_discharge_m3s", null)
	if not (soil_value is PackedFloat64Array and surface_value is PackedFloat64Array \
			and channel_value is PackedFloat64Array \
			and discharge_value is PackedFloat64Array):
		return ERR_INVALID_DATA
	var soil := soil_value as PackedFloat64Array
	var surface := surface_value as PackedFloat64Array
	var channel := channel_value as PackedFloat64Array
	var discharge := discharge_value as PackedFloat64Array
	if soil.size() != cell_count() or surface.size() != cell_count() \
			or channel.size() != cell_count() or discharge.size() != cell_count():
		return ERR_INVALID_DATA
	soil_water_m = soil.duplicate()
	surface_storage_m3 = surface.duplicate()
	channel_storage_m3 = channel.duplicate()
	channel_discharge_m3s = discharge.duplicate()
	initial_storage_m3 = float(data.get("initial_storage_m3", total_storage_m3()))
	cumulative_precipitation_m3 = float(data.get("cumulative_precipitation_m3", 0.0))
	cumulative_climatology_input_m3 = float(data.get("cumulative_climatology_input_m3", 0.0))
	cumulative_outlet_m3 = float(data.get("cumulative_outlet_m3", 0.0))
	simulated_seconds = float(data.get("simulated_seconds", 0.0))
	step_count = int(data.get("step_count", 0))
	return OK


func stats() -> Dictionary:
	return {
		"initialized": initialized,
		"cells": cell_count(),
		"simulated_seconds": simulated_seconds,
		"step_count": step_count,
		"use_climatology_fallback": use_climatology_fallback,
		"precipitation_phase": "water_equivalent_pending_surface_phase_field",
		"initial_storage_m3": initial_storage_m3,
		"storage_m3": total_storage_m3(),
		"cumulative_precipitation_m3": cumulative_precipitation_m3,
		"cumulative_climatology_input_m3": cumulative_climatology_input_m3,
		"cumulative_outlet_m3": cumulative_outlet_m3,
		"mass_error_m3": mass_error_m3(),
		"mass_relative_error": mass_relative_error(),
		"last_step": last_step_report.duplicate(true),
	}


func _resize_all(n: int) -> void:
	area_m2.resize(n)
	receiver.resize(n)
	route_order.resize(n)
	soil_capacity_m.resize(n)
	infiltration_capacity_mps.resize(n)
	soil_baseflow_tau_s.resize(n)
	surface_release_tau_s.resize(n)
	channel_release_tau_s.resize(n)
	local_climatology_q_m3s.resize(n)
	precipitation_mps.resize(n)
	soil_water_m.resize(n)
	surface_storage_m3.resize(n)
	channel_storage_m3.resize(n)
	channel_discharge_m3s.resize(n)
	_routing_inflow_m3.resize(n)


func _build_topology() -> void:
	var n := cell_count()
	var donor_count := PackedInt32Array()
	donor_count.resize(n)
	for c in n:
		var slot := int(fields.flow_dir[c])
		var r := c
		if slot >= 0 and slot < 8:
			r = int(grid.nbr[c * 8 + slot])
		receiver[c] = r
		if r != c:
			donor_count[r] += 1

	# Same outlet-first topological order used by FlowRouter, reconstructed from the
	# persisted `flow_dir` so runtime does not need the generation-only router object.
	var donor_start := PackedInt32Array()
	donor_start.resize(n + 1)
	var total_donors := 0
	for c in n:
		donor_start[c] = total_donors
		total_donors += donor_count[c]
	donor_start[n] = total_donors
	var cursor := donor_start.duplicate()
	var donor_list := PackedInt32Array()
	donor_list.resize(total_donors)
	for c in n:
		var r := receiver[c]
		if r != c:
			donor_list[cursor[r]] = c
			cursor[r] += 1
	var head := 0
	var tail := 0
	for c in n:
		if receiver[c] == c:
			route_order[tail] = c
			tail += 1
	while head < tail:
		var c := route_order[head]
		head += 1
		for i in range(donor_start[c], donor_start[c + 1]):
			route_order[tail] = donor_list[i]
			tail += 1
	if tail < n:
		# Defensive containment for malformed/cyclic imported data. Preserve every
		# cell rather than indexing uninitialized order entries.
		var seen := PackedByteArray()
		seen.resize(n)
		for i in tail:
			seen[route_order[i]] = 1
		for c in n:
			if seen[c] == 0:
				route_order[tail] = c
				tail += 1


func _build_static_hydraulic_properties() -> void:
	var n := cell_count()
	for c in n:
		var spacing := maxf(float(grid.cell_size[c]), 1.0)
		area_m2[c] = spacing * spacing
		if fields.elev[c] <= 0.0:
			soil_capacity_m[c] = 0.0
			infiltration_capacity_mps[c] = 0.0
			soil_baseflow_tau_s[c] = SECONDS_PER_DAY
			surface_release_tau_s[c] = surface_release_tau_min_s
			channel_release_tau_s[c] = maxf(spacing / channel_velocity_max_mps, 60.0)
			continue

		var soil_depth := maxf(float(fields.soil_depth[c]), 0.0)
		var sand := clampf(float(fields.soil_sand[c]), 0.0, 1.0)
		var silt := clampf(float(fields.soil_silt[c]), 0.0, 1.0)
		var clay := clampf(float(fields.soil_clay[c]), 0.0, 1.0)
		var organic := clampf(float(fields.soil_organic[c]), 0.0, 1.0)
		# Approximate plant-available/retained water depth from texture and profile
		# thickness. This is a storage parameter, not a new physical water source.
		var retained_fraction := 0.07 + 0.08 * silt + 0.12 * clay + 0.10 * organic
		soil_capacity_m[c] = clampf(soil_depth * retained_fraction,
			soil_capacity_min_m, soil_capacity_max_m)

		var infil_mm_h := 4.0 + 34.0 * sand + 8.0 * organic - 10.0 * clay
		infiltration_capacity_mps[c] = clampf(infil_mm_h,
			infiltration_min_mm_h, infiltration_max_mm_h) * MM_H_TO_M_S

		var aquifer := clampf(float(fields.aquifer[c]), 0.0, 1.0)
		var baseflow_days := lerpf(soil_baseflow_tau_min_days,
			soil_baseflow_tau_max_days, aquifer)
		soil_baseflow_tau_s[c] = baseflow_days * SECONDS_PER_DAY

		var floodplain := clampf(float(fields.floodplain[c]), 0.0, 1.0)
		var relief := maxf(float(fields.relief[c]), 0.0)
		var slope_proxy := clampf(relief / spacing, 0.0, 1.0)
		var retain := clampf(floodplain * 0.78 + (1.0 - slope_proxy) * 0.22, 0.0, 1.0)
		surface_release_tau_s[c] = lerpf(surface_release_tau_min_s,
			surface_release_tau_max_s, retain)

		var baseline_q := maxf(float(fields.discharge[c]), 0.0)
		var order_gain := clampf(float(fields.stream_order[c]) / 7.0, 0.0, 1.0)
		var q_gain := clampf(sqrt(baseline_q) / 18.0, 0.0, 1.0)
		var velocity := lerpf(channel_velocity_min_mps, channel_velocity_max_mps,
			maxf(order_gain, q_gain))
		var tau := maxf(spacing / maxf(velocity, 0.05), 60.0)
		if fields.lake_level[c] > -1.0e8:
			tau *= maxf(lake_residence_multiplier, 1.0)
		channel_release_tau_s[c] = tau

	# Recover each cell's *local* climatological contribution from the generated
	# accumulated discharge. Summing these local values downstream reconstructs the
	# baseline network without injecting upstream flow again at every receiver.
	for c in n:
		local_climatology_q_m3s[c] = maxf(float(fields.discharge[c]), 0.0)
	for c in n:
		var r := receiver[c]
		if r != c and r >= 0 and r < n:
			local_climatology_q_m3s[r] -= maxf(float(fields.discharge[c]), 0.0)
	for c in n:
		local_climatology_q_m3s[c] = maxf(local_climatology_q_m3s[c], 0.0)


func _initialize_dynamic_state() -> void:
	precipitation_mps.fill(0.0)
	soil_water_m.fill(0.0)
	surface_storage_m3.fill(0.0)
	channel_storage_m3.fill(0.0)
	channel_discharge_m3s.fill(0.0)
	_routing_inflow_m3.fill(0.0)
	for c in cell_count():
		if fields.elev[c] <= 0.0:
			continue
		var moisture := clampf(float(fields.soil_moisture[c]), 0.0, 1.0)
		soil_water_m[c] = soil_capacity_m[c] * moisture
		var baseline_q := maxf(float(fields.discharge[c]), 0.0)
		channel_discharge_m3s[c] = baseline_q
		channel_storage_m3[c] = baseline_q * channel_release_tau_s[c]
	initial_storage_m3 = total_storage_m3()
	cumulative_precipitation_m3 = 0.0
	cumulative_climatology_input_m3 = 0.0
	cumulative_outlet_m3 = 0.0
	simulated_seconds = 0.0
	step_count = 0
	last_step_report.clear()


func _reservoir_fraction(dt_s: float, tau_s: float) -> float:
	var tau := maxf(tau_s, 1.0e-3)
	return clampf(1.0 - exp(-maxf(dt_s, 0.0) / tau), 0.0, 1.0)


func _insert_bounded_candidate(out: Array[Dictionary], candidate: Dictionary,
		max_count: int) -> void:
	var insert_at := out.size()
	var score := float(candidate.get("score", 0.0))
	for i in out.size():
		if score > float(out[i].get("score", 0.0)):
			insert_at = i
			break
	if insert_at < max_count:
		out.insert(insert_at, candidate)
		if out.size() > max_count:
			out.pop_back()
	elif out.size() < max_count:
		out.append(candidate)
