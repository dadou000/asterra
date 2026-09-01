class_name HydroReferenceSolver
extends RefCounted
## Small deterministic finite-volume shallow-water reference solver.
##
## This is a correctness oracle for the GPU implementation, not a production
## planetary solver. It uses hydrostatic reconstruction + Rusanov fluxes so a
## lake at rest over uneven bathymetry remains balanced while wet/dry fronts stay
## positive. State is conservative: h, hu, hv, with bed elevation stored apart.

const G := 9.81
const DRY_EPS := 1.0e-5
const MIN_DEPTH_FOR_FRICTION := 1.0e-3

var width: int
var height: int
var cell_size_m: float
var cfl := 0.42
var manning_n := 0.025

var h := PackedFloat32Array()
var hu := PackedFloat32Array()
var hv := PackedFloat32Array()
var bed := PackedFloat32Array()
var rain_mps := PackedFloat32Array()
var infiltration_mps := PackedFloat32Array()

var _next_h := PackedFloat32Array()
var _next_hu := PackedFloat32Array()
var _next_hv := PackedFloat32Array()


func _init(p_width: int, p_height: int, p_cell_size_m: float) -> void:
	width = maxi(p_width, 1)
	height = maxi(p_height, 1)
	cell_size_m = maxf(p_cell_size_m, 1.0e-3)
	var count := width * height
	for field in [h, hu, hv, bed, rain_mps, infiltration_mps,
			_next_h, _next_hu, _next_hv]:
		field.resize(count)


func index(x: int, y: int) -> int:
	return y * width + x


func set_bed(x: int, y: int, elevation_m: float) -> void:
	bed[index(x, y)] = elevation_m


func set_state(x: int, y: int, depth_m: float, velocity_mps := Vector2.ZERO) -> void:
	var i := index(x, y)
	var depth := maxf(depth_m, 0.0)
	h[i] = depth
	hu[i] = depth * velocity_mps.x
	hv[i] = depth * velocity_mps.y


func set_surface_elevation(x: int, y: int, elevation_m: float) -> void:
	var i := index(x, y)
	set_state(x, y, maxf(elevation_m - bed[i], 0.0))


func set_rain_rate(x: int, y: int, rate_mps: float) -> void:
	rain_mps[index(x, y)] = maxf(rate_mps, 0.0)


func set_infiltration_rate(x: int, y: int, rate_mps: float) -> void:
	infiltration_mps[index(x, y)] = maxf(rate_mps, 0.0)


func surface_elevation(x: int, y: int) -> float:
	var i := index(x, y)
	return bed[i] + h[i]


func velocity(x: int, y: int) -> Vector2:
	var i := index(x, y)
	if h[i] <= DRY_EPS:
		return Vector2.ZERO
	return Vector2(hu[i] / h[i], hv[i] / h[i])


func total_volume_m3() -> float:
	var sum := 0.0
	var cell_area := cell_size_m * cell_size_m
	for depth in h:
		sum += float(depth) * cell_area
	return sum


func total_momentum() -> Vector2:
	var result := Vector2.ZERO
	var cell_area := cell_size_m * cell_size_m
	for i in h.size():
		result.x += hu[i] * cell_area
		result.y += hv[i] * cell_area
	return result


func max_speed_mps() -> float:
	var result := 0.0
	for i in h.size():
		if h[i] <= DRY_EPS:
			continue
		var inv_h := 1.0 / h[i]
		result = maxf(result, Vector2(hu[i] * inv_h, hv[i] * inv_h).length())
	return result


func stable_timestep(max_dt: float = INF) -> float:
	var max_signal := 0.0
	for i in h.size():
		var depth := h[i]
		if depth <= DRY_EPS:
			continue
		var c := sqrt(G * depth)
		var inv_h := 1.0 / depth
		var u := hu[i] * inv_h
		var v := hv[i] * inv_h
		max_signal = maxf(max_signal, maxf(absf(u) + c, absf(v) + c))
	if max_signal <= 1.0e-8:
		return max_dt
	return minf(max_dt, cfl * cell_size_m / max_signal)


## Advance by exactly requested_dt. The reference solver substeps internally to
## satisfy CFL, matching how the GPU hierarchy will later subcycle.
func advance(requested_dt: float) -> int:
	var remaining := maxf(requested_dt, 0.0)
	var steps := 0
	while remaining > 1.0e-8:
		var dt := stable_timestep(remaining)
		if not is_finite(dt) or dt <= 0.0:
			break
		_step(dt)
		remaining -= dt
		steps += 1
		if steps > 100000:
			push_error("HydroReferenceSolver exceeded emergency substep limit")
			break
	return steps


func _step(dt: float) -> void:
	var inv_dx := 1.0 / cell_size_m
	for y in height:
		for x in width:
			var i := index(x, y)
			var current := Vector3(h[i], hu[i], hv[i])

			var west := _cell_state(maxi(x - 1, 0), y)
			var east := _cell_state(mini(x + 1, width - 1), y)
			var south := _cell_state(x, maxi(y - 1, 0))
			var north := _cell_state(x, mini(y + 1, height - 1))

			# Reflect normal momentum at domain walls. Interior neighbor states are
			# unchanged. A closed box is the safest correctness fixture; open/radiative
			# boundaries belong to hydraulic-domain coupling later.
			if x == 0:
				west.y = -current.y
			if x == width - 1:
				east.y = -current.y
			if y == 0:
				south.z = -current.z
			if y == height - 1:
				north.z = -current.z

			var zc := bed[i]
			var zw := bed[index(maxi(x - 1, 0), y)] if x > 0 else zc
			var ze := bed[index(mini(x + 1, width - 1), y)] if x < width - 1 else zc
			var zs := bed[index(x, maxi(y - 1, 0))] if y > 0 else zc
			var zn := bed[index(x, mini(y + 1, height - 1))] if y < height - 1 else zc

			var fx_w := _hydrostatic_interface_x(west, zw, current, zc)
			var fx_e := _hydrostatic_interface_x(current, zc, east, ze)
			var fy_s := _hydrostatic_interface_y(south, zs, current, zc)
			var fy_n := _hydrostatic_interface_y(current, zc, north, zn)

			# Audusse hydrostatic-reconstruction source correction. The shared
			# Rusanov flux is conservative; the cell-side correction balances bed
			# pressure exactly for eta = h + z = constant, u = v = 0.
			var west_flux: Vector3 = fx_w[0]
			var east_flux: Vector3 = fx_e[0]
			var south_flux: Vector3 = fy_s[0]
			var north_flux: Vector3 = fy_n[0]
			var h_w_star: float = fx_w[2]
			var h_e_star: float = fx_e[1]
			var h_s_star: float = fy_s[2]
			var h_n_star: float = fy_n[1]

			var updated := current \
				- (east_flux - west_flux) * (dt * inv_dx) \
				- (north_flux - south_flux) * (dt * inv_dx)

			updated.y += dt * inv_dx * 0.5 * G * (
				h_e_star * h_e_star - h_w_star * h_w_star)
			updated.z += dt * inv_dx * 0.5 * G * (
				h_n_star * h_n_star - h_s_star * h_s_star)

			# Surface sources. Infiltration cannot remove more water than exists in
			# this substep, preserving positivity even at a drying front.
			var net_source := rain_mps[i] - infiltration_mps[i]
			updated.x = maxf(updated.x + net_source * dt, 0.0)

			if updated.x <= DRY_EPS:
				updated = Vector3.ZERO
			else:
				updated = _apply_manning_friction(updated, dt)

			_next_h[i] = updated.x
			_next_hu[i] = updated.y
			_next_hv[i] = updated.z

	var swap_h := h
	h = _next_h
	_next_h = swap_h
	var swap_hu := hu
	hu = _next_hu
	_next_hu = swap_hu
	var swap_hv := hv
	hv = _next_hv
	_next_hv = swap_hv


func _cell_state(x: int, y: int) -> Vector3:
	var i := index(x, y)
	return Vector3(h[i], hu[i], hv[i])


## Returns [shared_flux, left_reconstructed_depth, right_reconstructed_depth].
func _hydrostatic_interface_x(left: Vector3, z_left: float,
		right: Vector3, z_right: float) -> Array:
	var z_star := maxf(z_left, z_right)
	var hl := maxf(left.x + z_left - z_star, 0.0)
	var hr := maxf(right.x + z_right - z_star, 0.0)
	var ul := _reconstruct_state(left, hl)
	var ur := _reconstruct_state(right, hr)
	return [_rusanov_x(ul, ur), hl, hr]


func _hydrostatic_interface_y(lower: Vector3, z_lower: float,
		upper: Vector3, z_upper: float) -> Array:
	var z_star := maxf(z_lower, z_upper)
	var hl := maxf(lower.x + z_lower - z_star, 0.0)
	var hr := maxf(upper.x + z_upper - z_star, 0.0)
	var ul := _reconstruct_state(lower, hl)
	var ur := _reconstruct_state(upper, hr)
	return [_rusanov_y(ul, ur), hl, hr]


func _reconstruct_state(original: Vector3, reconstructed_h: float) -> Vector3:
	if original.x <= DRY_EPS or reconstructed_h <= DRY_EPS:
		return Vector3.ZERO
	var scale := reconstructed_h / original.x
	return Vector3(reconstructed_h, original.y * scale, original.z * scale)


func _rusanov_x(left: Vector3, right: Vector3) -> Vector3:
	var fl := _physical_flux_x(left)
	var fr := _physical_flux_x(right)
	var sl := _signal_speed_x(left)
	var sr := _signal_speed_x(right)
	var a := maxf(sl, sr)
	return (fl + fr) * 0.5 - (right - left) * (0.5 * a)


func _rusanov_y(lower: Vector3, upper: Vector3) -> Vector3:
	var fl := _physical_flux_y(lower)
	var fr := _physical_flux_y(upper)
	var sl := _signal_speed_y(lower)
	var sr := _signal_speed_y(upper)
	var a := maxf(sl, sr)
	return (fl + fr) * 0.5 - (upper - lower) * (0.5 * a)


func _physical_flux_x(state: Vector3) -> Vector3:
	var depth := state.x
	if depth <= DRY_EPS:
		return Vector3.ZERO
	var u := state.y / depth
	var v := state.z / depth
	return Vector3(state.y, state.y * u + 0.5 * G * depth * depth,
		state.y * v)


func _physical_flux_y(state: Vector3) -> Vector3:
	var depth := state.x
	if depth <= DRY_EPS:
		return Vector3.ZERO
	var u := state.y / depth
	var v := state.z / depth
	return Vector3(state.z, state.z * u,
		state.z * v + 0.5 * G * depth * depth)


func _signal_speed_x(state: Vector3) -> float:
	if state.x <= DRY_EPS:
		return 0.0
	return absf(state.y / state.x) + sqrt(G * state.x)


func _signal_speed_y(state: Vector3) -> float:
	if state.x <= DRY_EPS:
		return 0.0
	return absf(state.z / state.x) + sqrt(G * state.x)


func _apply_manning_friction(state: Vector3, dt: float) -> Vector3:
	var depth := state.x
	if depth <= MIN_DEPTH_FOR_FRICTION or manning_n <= 0.0:
		return state
	var velocity := Vector2(state.y, state.z) / depth
	var speed := velocity.length()
	if speed <= 1.0e-8:
		return state
	var denom := 1.0 + dt * G * manning_n * manning_n * speed \
		/ pow(depth, 4.0 / 3.0)
	return Vector3(depth, state.y / denom, state.z / denom)
