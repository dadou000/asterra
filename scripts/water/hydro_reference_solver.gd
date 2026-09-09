class_name HydroReferenceSolver
extends RefCounted
## Correctness oracle for the GPU shallow-water solver.
## Conservative h/hu/hv + hydrostatic reconstruction + Rusanov fluxes.

const G := 9.81
const DRY_EPS := 1.0e-5

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
var _nh := PackedFloat32Array()
var _nhu := PackedFloat32Array()
var _nhv := PackedFloat32Array()


func _init(w: int, hgt: int, dx: float) -> void:
	width = maxi(w, 1)
	height = maxi(hgt, 1)
	cell_size_m = maxf(dx, 1.0e-3)
	var n := width * height
	# Packed arrays are copy-on-write values: resize members directly.
	h.resize(n)
	hu.resize(n)
	hv.resize(n)
	bed.resize(n)
	rain_mps.resize(n)
	infiltration_mps.resize(n)
	_nh.resize(n)
	_nhu.resize(n)
	_nhv.resize(n)


func index(x: int, y: int) -> int:
	return y * width + x


func set_bed(x: int, y: int, z: float) -> void:
	bed[index(x, y)] = z


func set_state(x: int, y: int, depth: float, velocity := Vector2.ZERO) -> void:
	var i := index(x, y)
	var d := maxf(depth, 0.0)
	h[i] = d
	hu[i] = d * velocity.x
	hv[i] = d * velocity.y


func set_surface_elevation(x: int, y: int, eta: float) -> void:
	var i := index(x, y)
	set_state(x, y, maxf(eta - bed[i], 0.0))


func set_rain_rate(x: int, y: int, rate: float) -> void:
	rain_mps[index(x, y)] = maxf(rate, 0.0)


func set_infiltration_rate(x: int, y: int, rate: float) -> void:
	infiltration_mps[index(x, y)] = maxf(rate, 0.0)


func surface_elevation(x: int, y: int) -> float:
	var i := index(x, y)
	return bed[i] + h[i]


func velocity(x: int, y: int) -> Vector2:
	var i := index(x, y)
	if h[i] <= DRY_EPS:
		return Vector2.ZERO
	return Vector2(hu[i], hv[i]) / h[i]


func total_volume_m3() -> float:
	var volume := 0.0
	var area := cell_size_m * cell_size_m
	for depth in h:
		volume += float(depth) * area
	return volume


func max_speed_mps() -> float:
	var result := 0.0
	for i in h.size():
		if h[i] > DRY_EPS:
			result = maxf(result, (Vector2(hu[i], hv[i]) / h[i]).length())
	return result


func stable_timestep(limit: float = INF) -> float:
	var max_signal := 0.0
	for i in h.size():
		var d := h[i]
		if d <= DRY_EPS:
			continue
		var c := sqrt(G * d)
		max_signal = maxf(max_signal,
			maxf(absf(hu[i] / d) + c, absf(hv[i] / d) + c))
	if max_signal <= 1.0e-8:
		return limit
	return minf(limit, cfl * cell_size_m / max_signal)


func advance(requested_dt: float) -> int:
	var remaining := maxf(requested_dt, 0.0)
	var count := 0
	while remaining > 1.0e-8:
		var dt := stable_timestep(remaining)
		if not is_finite(dt) or dt <= 0.0:
			break
		_step(dt)
		remaining -= dt
		count += 1
		if count > 100000:
			push_error("HydroReferenceSolver: emergency substep limit")
			break
	return count


func _step(dt: float) -> void:
	var s := dt / cell_size_m
	for y in height:
		for x in width:
			var i := index(x, y)
			var c := Vector3(h[i], hu[i], hv[i])
			var w := _state(maxi(x - 1, 0), y)
			var e := _state(mini(x + 1, width - 1), y)
			var so := _state(x, maxi(y - 1, 0))
			var n := _state(x, mini(y + 1, height - 1))
			if x == 0: w.y = -c.y
			if x == width - 1: e.y = -c.y
			if y == 0: so.z = -c.z
			if y == height - 1: n.z = -c.z

			var zc := bed[i]
			var zw := bed[index(maxi(x - 1, 0), y)] if x > 0 else zc
			var ze := bed[index(mini(x + 1, width - 1), y)] if x < width - 1 else zc
			var zs := bed[index(x, maxi(y - 1, 0))] if y > 0 else zc
			var zn := bed[index(x, mini(y + 1, height - 1))] if y < height - 1 else zc

			var fw := _iface_x(w, zw, c, zc)
			var fe := _iface_x(c, zc, e, ze)
			var fs := _iface_y(so, zs, c, zc)
			var fn := _iface_y(c, zc, n, zn)
			var u := c - (fe[0] - fw[0]) * s - (fn[0] - fs[0]) * s

			# Well-balanced hydrostatic pressure correction.
			u.y += 0.5 * G * s * (float(fe[1]) * float(fe[1]) - float(fw[2]) * float(fw[2]))
			u.z += 0.5 * G * s * (float(fn[1]) * float(fn[1]) - float(fs[2]) * float(fs[2]))

			u.x = maxf(u.x + (rain_mps[i] - infiltration_mps[i]) * dt, 0.0)
			if u.x <= DRY_EPS:
				u = Vector3.ZERO
			else:
				u = _friction(u, dt)
			_nh[i] = u.x
			_nhu[i] = u.y
			_nhv[i] = u.z

	var a := h; h = _nh; _nh = a
	var b := hu; hu = _nhu; _nhu = b
	var cbuf := hv; hv = _nhv; _nhv = cbuf


func _state(x: int, y: int) -> Vector3:
	var i := index(x, y)
	return Vector3(h[i], hu[i], hv[i])


## [shared numerical flux, left/bottom reconstructed h, right/top reconstructed h]
func _iface_x(l: Vector3, zl: float, r: Vector3, zr: float) -> Array:
	var zstar := maxf(zl, zr)
	var hl := maxf(l.x + zl - zstar, 0.0)
	var hr := maxf(r.x + zr - zstar, 0.0)
	return [_rusanov_x(_reconstruct(l, hl), _reconstruct(r, hr)), hl, hr]


func _iface_y(l: Vector3, zl: float, r: Vector3, zr: float) -> Array:
	var zstar := maxf(zl, zr)
	var hl := maxf(l.x + zl - zstar, 0.0)
	var hr := maxf(r.x + zr - zstar, 0.0)
	return [_rusanov_y(_reconstruct(l, hl), _reconstruct(r, hr)), hl, hr]


func _reconstruct(q: Vector3, depth: float) -> Vector3:
	if q.x <= DRY_EPS or depth <= DRY_EPS:
		return Vector3.ZERO
	var scale := depth / q.x
	return Vector3(depth, q.y * scale, q.z * scale)


func _rusanov_x(l: Vector3, r: Vector3) -> Vector3:
	var a := maxf(_speed_x(l), _speed_x(r))
	return (_flux_x(l) + _flux_x(r)) * 0.5 - (r - l) * (0.5 * a)


func _rusanov_y(l: Vector3, r: Vector3) -> Vector3:
	var a := maxf(_speed_y(l), _speed_y(r))
	return (_flux_y(l) + _flux_y(r)) * 0.5 - (r - l) * (0.5 * a)


func _flux_x(q: Vector3) -> Vector3:
	if q.x <= DRY_EPS: return Vector3.ZERO
	var u := q.y / q.x
	return Vector3(q.y, q.y * u + 0.5 * G * q.x * q.x, q.z * u)


func _flux_y(q: Vector3) -> Vector3:
	if q.x <= DRY_EPS: return Vector3.ZERO
	var v := q.z / q.x
	return Vector3(q.z, q.y * v, q.z * v + 0.5 * G * q.x * q.x)


func _speed_x(q: Vector3) -> float:
	return 0.0 if q.x <= DRY_EPS else absf(q.y / q.x) + sqrt(G * q.x)


func _speed_y(q: Vector3) -> float:
	return 0.0 if q.x <= DRY_EPS else absf(q.z / q.x) + sqrt(G * q.x)


func _friction(q: Vector3, dt: float) -> Vector3:
	if q.x <= 1.0e-3 or manning_n <= 0.0:
		return q
	var speed := (Vector2(q.y, q.z) / q.x).length()
	if speed <= 1.0e-8:
		return q
	var denom := 1.0 + dt * G * manning_n * manning_n * speed / pow(q.x, 4.0 / 3.0)
	return Vector3(q.x, q.y / denom, q.z / denom)
