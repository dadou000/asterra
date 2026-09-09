class_name PlanetRiverReachStore
extends RefCounted
## Static 1D channel geometry and storage->flow transforms for generated macro rivers.
## Dynamic water stays authoritative in PlanetHydrologyStore.channel_storage_m3.

const MIN_SLOPE := 1.0e-5
const MIN_REACH_LENGTH_M := 1.0
const MIN_WIDTH_M := 1.0
const MIN_DEPTH_M := 0.01
const MAX_SOLVED_DEPTH_M := 80.0
const DEPTH_SOLVE_ITERATIONS := 40

var bankfull_discharge_multiplier := 2.5
var side_slope_h_over_v := 1.5
var manning_n_min := 0.026
var manning_n_max := 0.045
var minimum_travel_velocity_mps := 0.12
var kinematic_celerity_multiplier := 1.5

var fields: PlanetFields
var grid: PlanetGrid
var receiver := PackedInt32Array()
var initialized := false

var reach_mask := PackedByteArray()
var reach_length_m := PackedFloat64Array()
var bottom_width_m := PackedFloat64Array()
var bed_slope := PackedFloat64Array()
var manning_n := PackedFloat64Array()
var normal_depth_m := PackedFloat64Array()
var bankfull_depth_m := PackedFloat64Array()
var normal_storage_m3 := PackedFloat64Array()
var bankfull_storage_m3 := PackedFloat64Array()
var channel_bed_elevation_m := PackedFloat64Array()

var reach_count := 0
var total_reach_length_m := 0.0


func initialize(p_fields: PlanetFields, p_receiver: PackedInt32Array) -> Error:
	if p_fields == null or p_fields.grid == null:
		return ERR_INVALID_PARAMETER
	if p_receiver.size() != p_fields.grid.cell_count \
			or p_fields.river_width.size() != p_fields.grid.cell_count \
			or p_fields.discharge.size() != p_fields.grid.cell_count:
		return ERR_INVALID_DATA
	fields = p_fields
	grid = p_fields.grid
	receiver = p_receiver.duplicate()
	_resize(grid.cell_count)
	# Geometry calibration calls is_reach_cell()/Manning helpers. Mark the object
	# usable before building, then leave it initialized after the deterministic pass.
	initialized = true
	_build_geometry()
	return OK


func is_reach_cell(cell: int) -> bool:
	return initialized and cell >= 0 and cell < reach_mask.size() and reach_mask[cell] != 0


func normal_storage_for_cell(cell: int) -> float:
	return normal_storage_m3[cell] if is_reach_cell(cell) else 0.0


func bankfull_storage_for_cell(cell: int) -> float:
	return bankfull_storage_m3[cell] if is_reach_cell(cell) else 0.0


func advance_cell(cell: int, storage_m3: float, dt_s: float) -> Dictionary:
	if not is_reach_cell(cell) or not is_finite(storage_m3) \
			or not is_finite(dt_s) or dt_s <= 0.0:
		return {"error": ERR_INVALID_PARAMETER}
	var dt := maxf(dt_s, 1.0e-6)
	var volume := maxf(storage_m3, 0.0)
	var bankfull := maxf(bankfull_storage_m3[cell], 0.0)
	var spill := maxf(volume - bankfull, 0.0)
	volume = minf(volume, bankfull)

	var depth_before := depth_from_storage(cell, volume)
	var area := cross_section_area(cell, depth_before)
	var q_capacity := discharge_for_depth(cell, depth_before)
	var velocity := q_capacity / maxf(area, 1.0e-9)
	var celerity := maxf(velocity * maxf(kinematic_celerity_multiplier, 1.0),
		minimum_travel_velocity_mps)
	var travel_tau := maxf(reach_length_m[cell] / maxf(celerity, 1.0e-6), 1.0)
	var travel_fraction := clampf(1.0 - exp(-dt / travel_tau), 0.0, 1.0)
	var outflow_volume := minf(volume,
		minf(q_capacity * dt, volume * travel_fraction))
	outflow_volume = maxf(outflow_volume, 0.0)
	var remaining := maxf(volume - outflow_volume, 0.0)
	var depth_after := depth_from_storage(cell, remaining)
	return {
		"error": OK,
		"cell": cell,
		"remaining_storage_m3": remaining,
		"outflow_volume_m3": outflow_volume,
		"discharge_m3s": outflow_volume / dt,
		"spill_to_surface_m3": spill,
		"depth_before_m": depth_before,
		"depth_after_m": depth_after,
		"stage_after_m": channel_bed_elevation_m[cell] + depth_after,
		"bankfull_depth_ratio": depth_after / maxf(bankfull_depth_m[cell], 1.0e-9),
		"hydraulic_velocity_mps": velocity,
		"travel_time_s": travel_tau,
		"manning_capacity_m3s": q_capacity,
	}


func reach_state(cell: int, storage_m3: float = -1.0) -> Dictionary:
	if not is_reach_cell(cell):
		return {}
	var volume := normal_storage_m3[cell] if storage_m3 < 0.0 else maxf(storage_m3, 0.0)
	var depth := depth_from_storage(cell, minf(volume, bankfull_storage_m3[cell]))
	return {
		"cell": cell,
		"receiver": receiver[cell],
		"length_m": reach_length_m[cell],
		"bottom_width_m": bottom_width_m[cell],
		"bed_slope": bed_slope[cell],
		"manning_n": manning_n[cell],
		"normal_depth_m": normal_depth_m[cell],
		"bankfull_depth_m": bankfull_depth_m[cell],
		"normal_storage_m3": normal_storage_m3[cell],
		"bankfull_storage_m3": bankfull_storage_m3[cell],
		"channel_bed_elevation_m": channel_bed_elevation_m[cell],
		"storage_m3": volume,
		"depth_m": depth,
		"stage_m": channel_bed_elevation_m[cell] + depth,
		"bankfull_depth_ratio": depth / maxf(bankfull_depth_m[cell], 1.0e-9),
		"manning_capacity_m3s": discharge_for_depth(cell, depth),
		"baseline_discharge_m3s": maxf(float(fields.discharge[cell]), 0.0),
		"stream_order": int(fields.stream_order[cell]),
	}


func cross_section_area(cell: int, depth_m: float) -> float:
	if not is_reach_cell(cell):
		return 0.0
	var d := maxf(depth_m, 0.0)
	var b := maxf(bottom_width_m[cell], MIN_WIDTH_M)
	var z := maxf(side_slope_h_over_v, 0.0)
	return d * (b + z * d)


func wetted_perimeter(cell: int, depth_m: float) -> float:
	if not is_reach_cell(cell):
		return 0.0
	var d := maxf(depth_m, 0.0)
	var b := maxf(bottom_width_m[cell], MIN_WIDTH_M)
	var z := maxf(side_slope_h_over_v, 0.0)
	return b + 2.0 * d * sqrt(1.0 + z * z)


func discharge_for_depth(cell: int, depth_m: float) -> float:
	if not is_reach_cell(cell) or depth_m <= 0.0:
		return 0.0
	var area := cross_section_area(cell, depth_m)
	var perimeter := maxf(wetted_perimeter(cell, depth_m), 1.0e-9)
	var hydraulic_radius := area / perimeter
	return (1.0 / maxf(manning_n[cell], 1.0e-4)) * area \
		* pow(maxf(hydraulic_radius, 1.0e-9), 2.0 / 3.0) \
		* sqrt(maxf(bed_slope[cell], MIN_SLOPE))


func depth_from_storage(cell: int, storage_m3: float) -> float:
	if not is_reach_cell(cell) or storage_m3 <= 0.0:
		return 0.0
	var area := storage_m3 / maxf(reach_length_m[cell], MIN_REACH_LENGTH_M)
	var b := maxf(bottom_width_m[cell], MIN_WIDTH_M)
	var z := maxf(side_slope_h_over_v, 0.0)
	if z <= 1.0e-9:
		return area / b
	return maxf((-b + sqrt(maxf(b * b + 4.0 * z * area, 0.0))) / (2.0 * z), 0.0)


func stats() -> Dictionary:
	return {
		"initialized": initialized,
		"reach_count": reach_count,
		"total_reach_length_m": total_reach_length_m,
		"representation": "macro_cell_1d_trapezoidal_manning",
		"bankfull_discharge_multiplier": bankfull_discharge_multiplier,
	}


func _resize(n: int) -> void:
	reach_mask.resize(n)
	reach_length_m.resize(n)
	bottom_width_m.resize(n)
	bed_slope.resize(n)
	manning_n.resize(n)
	normal_depth_m.resize(n)
	bankfull_depth_m.resize(n)
	normal_storage_m3.resize(n)
	bankfull_storage_m3.resize(n)
	channel_bed_elevation_m.resize(n)
	reach_mask.fill(0)


func _build_geometry() -> void:
	reach_count = 0
	total_reach_length_m = 0.0
	for c in grid.cell_count:
		if fields.elev[c] <= 0.0 or float(fields.river_width[c]) <= 0.0 \
				or fields.lake_level[c] > -1.0e8:
			continue
		var r := receiver[c]
		if r < 0 or r >= grid.cell_count or r == c:
			continue
		var a := grid.cell_dir(c)
		var bdir := grid.cell_dir(r)
		var angle := acos(clampf(a.dot(bdir), -1.0, 1.0))
		var length := maxf(angle * grid.radius, maxf(float(grid.cell_size[c]), 1.0))
		var width := maxf(float(fields.river_width[c]), MIN_WIDTH_M)
		var raw_slope := (float(fields.elev[c]) - float(fields.elev[r])) / length
		var slope := maxf(raw_slope, MIN_SLOPE)
		var roughness := lerpf(manning_n_min, manning_n_max,
			clampf(float(fields.floodplain[c]), 0.0, 1.0))

		reach_mask[c] = 1
		reach_length_m[c] = length
		bottom_width_m[c] = width
		bed_slope[c] = slope
		manning_n[c] = roughness
		reach_count += 1
		total_reach_length_m += length

		var baseline_q := maxf(float(fields.discharge[c]), 1.0e-6)
		var normal_depth := _solve_depth_for_discharge(c, baseline_q)
		var bank_q := maxf(baseline_q * maxf(bankfull_discharge_multiplier, 1.05),
			baseline_q + 0.25)
		var bank_depth := _solve_depth_for_discharge(c, bank_q)
		bank_depth = maxf(bank_depth, maxf(normal_depth * 1.15, MIN_DEPTH_M))
		normal_depth_m[c] = normal_depth
		bankfull_depth_m[c] = bank_depth
		normal_storage_m3[c] = cross_section_area(c, normal_depth) * length
		bankfull_storage_m3[c] = cross_section_area(c, bank_depth) * length
		channel_bed_elevation_m[c] = float(fields.elev[c]) - bank_depth


func _solve_depth_for_discharge(cell: int, target_q_m3s: float) -> float:
	var target := maxf(target_q_m3s, 0.0)
	if target <= 0.0:
		return MIN_DEPTH_M
	var low := 0.0
	var high := 0.25
	while high < MAX_SOLVED_DEPTH_M and discharge_for_depth(cell, high) < target:
		high *= 2.0
	high = minf(high, MAX_SOLVED_DEPTH_M)
	for _iteration in DEPTH_SOLVE_ITERATIONS:
		var mid := 0.5 * (low + high)
		if discharge_for_depth(cell, mid) < target:
			low = mid
		else:
			high = mid
	return maxf(0.5 * (low + high), MIN_DEPTH_M)
