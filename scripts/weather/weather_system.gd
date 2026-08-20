class_name WeatherSystem
extends Node
## Authoritative dynamic weather simulation for Asterra.
##
## Baked climate is climatology, not a cloud mask. Runtime weather carries
## pressure, temperature, vapour, condensate and winds over the seamless
## cube-sphere. Explicit mesoscale events (tropical cyclones and squall lines)
## live inside that same state, so orbit clouds, ground clouds and later rain /
## lightning all describe the same weather.
##
## Weather map RGBA:
##   R cloud coverage
##   G convective potential
##   B high-cloud / anvil coverage
##   A low-cloud base altitude / 6000 m
## Wind map RGBA (half float):
##   R eastward wind m/s
##   G northward wind m/s
##   B precipitation potential
##   A relative humidity

signal rebuilt
signal advanced(simulation_hours: float)

@export var update_interval: float = 2.0
@export var simulation_rate: float = 120.0

const MAX_COURANT := 0.42
const GEO_SCALE := 220000.0

const EVENT_TROPICAL_CYCLONE := 0
const EVENT_SQUALL_LINE := 1
const MAX_EVENTS := 6
const EVENT_SPAWN_PERIOD := 7200.0

var weather_map: Texture2DArray
var wind_map: Texture2DArray
var face_res: int = 0
var simulation_seconds: float = 0.0

var _cfg: GenConfig
var _grid: PlanetGrid
var _fields: PlanetFields
var _synoptic: NoiseKit
var _cells: NoiseKit

var _state: Dictionary = {}
var _pressure_forcing := PackedFloat32Array()
var _accum: float = 0.0
var _generation: int = 0
var _task_id: int = -1
var _step_in_flight: bool = false

func rebuild() -> void:
	if not Planet.ready_state:
		return
	_generation += 1
	_cfg = Planet.cfg
	_grid = Planet.grid
	_fields = Planet.fields
	face_res = _grid.res
	# Very broad, seamless forcing. It must never directly become cloud coverage:
	# that was what produced the giant face-like stripe seen from orbit.
	_synoptic = NoiseKit.new(_cfg.stream_seed("weather_synoptic"), 2.0, 4)
	_cells = NoiseKit.new(_cfg.stream_seed("weather_cells"), 6.3, 3)
	_initialize_state()
	var built: Dictionary = _build_images(_state, _grid, _fields)
	_publish_images(built)
	_accum = 0.0
	set_process(true)
	rebuilt.emit()

func _process(dt: float) -> void:
	if _state.is_empty() or _step_in_flight:
		return
	_accum += dt
	if _accum < update_interval:
		return
	var real_dt: float = _accum
	_accum = 0.0
	_schedule_step(maxf(1.0, real_dt * simulation_rate))

func _initialize_state() -> void:
	var n: int = _grid.cell_count
	var pressure := PackedFloat32Array()
	var temp := PackedFloat32Array()
	var vapor := PackedFloat32Array()
	var cloud := PackedFloat32Array()
	var high := PackedFloat32Array()
	var precip := PackedFloat32Array()
	var coverage := PackedFloat32Array()
	var convective := PackedFloat32Array()
	var wind_u := PackedFloat32Array()
	var wind_v := PackedFloat32Array()
	for a in [pressure, temp, vapor, cloud, high, precip, coverage, convective, wind_u, wind_v]:
		a.resize(n)
	_pressure_forcing = PackedFloat32Array()
	_pressure_forcing.resize(n)

	# Establish broad highs and lows. A second rotated component prevents one
	# low-frequency ridge from turning into a pole-to-pole cloud stripe.
	for c in n:
		var d: Vector3 = _grid.cell_dir(c)
		var syn: float = _synoptic.s(d)
		var secondary: float = _synoptic.s(Vector3(d.z, d.x, d.y))
		var cell_wave: float = _cells.s(Vector3(d.y, d.z, d.x))
		var p: float = -syn * 4.8 - secondary * 2.1 + cell_wave * 1.0
		pressure[c] = p
		_pressure_forcing[c] = p
		var t: float = _fields.temp_mean[c] + _cells.s(d) * 2.0
		temp[c] = t
		var qsat: float = _qsat_norm(t)
		# Ocean humidity is a reservoir. It is not cloud coverage.
		vapor[c] = clampf(_fields.humidity[c] * qsat, 0.0, 1.5)
		wind_u[c] = _fields.wind_u[c]
		wind_v[c] = _fields.wind_v[c]

	# Seed broad cloud only where the atmospheric state actually supports ascent.
	# Dynamic temperature/pressure gradients create fronts on subsequent steps.
	for c in n:
		var rh: float = clampf(vapor[c] / maxf(_qsat_norm(temp[c]), 0.02), 0.0, 1.35)
		var low: float = NoiseKit.smoothstepf(1.2, 7.2, -pressure[c])
		var oro: float = _orographic_lift(c, wind_u[c], wind_v[c])
		var warm: float = NoiseKit.smoothstepf(8.0, 27.0, temp[c])
		var sat: float = NoiseKit.smoothstepf(0.76, 0.99, rh)
		var organisation: float = clampf(low * 0.70 + oro * 0.34 + _fields.storm_risk[c] * 0.22, 0.0, 1.0)
		cloud[c] = sat * organisation * 0.72
		var cv: float = clampf(warm * rh * (_fields.storm_risk[c] * 0.56 + oro * 0.42 + low * 0.18), 0.0, 1.0)
		convective[c] = cv
		high[c] = clampf(low * sat * 0.10 + cv * 0.36, 0.0, 1.0)
		precip[c] = clampf(maxf(cloud[c] - 0.52, 0.0) * 1.45 + cv * cloud[c] * 0.38, 0.0, 1.0)
		coverage[c] = _coverage_from(cloud[c], rh, low, 0.0, oro)

	var events: Array = _seed_initial_events()
	_state = {
		"pressure": pressure,
		"temp": temp,
		"vapor": vapor,
		"cloud": cloud,
		"high": high,
		"precip": precip,
		"coverage": coverage,
		"convective": convective,
		"wind_u": wind_u,
		"wind_v": wind_v,
		"events": events,
		"spawn_timer": EVENT_SPAWN_PERIOD * 0.65,
		"next_event_id": events.size() + 1,
	}
	_apply_events_to_state(_state, events, _grid, _fields)

func _schedule_step(sim_dt: float) -> void:
	_step_in_flight = true
	var request_id: int = _generation
	var src: Dictionary = _state
	var grid: PlanetGrid = _grid
	var fields: PlanetFields = _fields
	var forcing: PackedFloat32Array = _pressure_forcing
	var task := func() -> void:
		var next: Dictionary = _simulate_step(src, grid, fields, forcing, sim_dt)
		var images: Dictionary = _build_images(next, grid, fields)
		call_deferred("_on_step_ready", request_id, sim_dt, next, images)
	_task_id = WorkerThreadPool.add_task(task, false, "asterra_weather")

func _on_step_ready(request_id: int, sim_dt: float, next: Dictionary, images: Dictionary) -> void:
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
	_step_in_flight = false
	if request_id != _generation:
		return
	_state = next
	simulation_seconds += sim_dt
	_publish_images(images)
	advanced.emit(simulation_seconds / 3600.0)
	rebuilt.emit()

func _simulate_step(src: Dictionary, grid: PlanetGrid, fields: PlanetFields,
		forcing: PackedFloat32Array, dt: float) -> Dictionary:
	var n: int = grid.cell_count
	var pressure0: PackedFloat32Array = src["pressure"]
	var temp0: PackedFloat32Array = src["temp"]
	var vapor0: PackedFloat32Array = src["vapor"]
	var cloud0: PackedFloat32Array = src["cloud"]
	var high0: PackedFloat32Array = src["high"]
	var wind_u0: PackedFloat32Array = src["wind_u"]
	var wind_v0: PackedFloat32Array = src["wind_v"]

	var pressure := PackedFloat32Array()
	var temp := PackedFloat32Array()
	var vapor := PackedFloat32Array()
	var cloud := PackedFloat32Array()
	var high := PackedFloat32Array()
	var precip := PackedFloat32Array()
	var coverage := PackedFloat32Array()
	var convective := PackedFloat32Array()
	var wind_u := PackedFloat32Array()
	var wind_v := PackedFloat32Array()
	for a in [pressure, temp, vapor, cloud, high, precip, coverage, convective, wind_u, wind_v]:
		a.resize(n)

	var events: Array = _advance_events(src, dt, grid, fields)
	var spawn_timer: float = float(src.get("spawn_timer", 0.0)) - dt
	var next_event_id: int = int(src.get("next_event_id", 1))
	var best_tc_score: float = -1.0
	var best_tc_cell: int = -1
	var best_sq_score: float = -1.0
	var best_sq_cell: int = -1

	for c in n:
		var d: Vector3 = grid.cell_dir(c)
		var tb: Array = CubeSphere.tangent_basis(d)
		var east: Vector3 = tb[0]
		var north: Vector3 = tb[1]
		var base: int = c * 8
		var p_here: float = pressure0[c]
		var t_here: float = temp0[c]
		var grad_p := Vector3.ZERO
		var grad_t := Vector3.ZERO
		var lap_p: float = 0.0
		var lap_t: float = 0.0
		var valid: float = 0.0
		for k in 8:
			var nb: int = grid.nbr[base + k]
			var nd: Vector3 = grid.cell_dir(nb)
			var tangent: Vector3 = nd - d * d.dot(nd)
			var angular: float = tangent.length()
			if angular < 1e-7:
				continue
			var metres: float = maxf(angular * grid.radius, 1.0)
			var along: Vector3 = tangent / angular
			var dp: float = pressure0[nb] - p_here
			var dtemp: float = temp0[nb] - t_here
			grad_p += along * (dp / metres)
			grad_t += along * (dtemp / metres)
			lap_p += dp
			lap_t += dtemp
			valid += 1.0
		if valid > 0.0:
			grad_p /= valid
			grad_t /= valid
			lap_p /= valid
			lap_t /= valid

		var hemi: float = 1.0 if d.y >= 0.0 else -1.0
		var coriolis_weight: float = clampf(absf(d.y) * 1.8, 0.12, 1.0)
		var gp_e: float = grad_p.dot(east)
		var gp_n: float = grad_p.dot(north)
		var geo_u: float = clampf(-gp_n * GEO_SCALE * hemi, -32.0, 32.0)
		var geo_v: float = clampf(gp_e * GEO_SCALE * hemi, -24.0, 24.0)
		var target_u: float = _fields_blend_wind(fields.wind_u[c], geo_u, coriolis_weight)
		var target_v: float = _fields_blend_wind(fields.wind_v[c], geo_v, coriolis_weight)
		wind_u[c] = lerpf(wind_u0[c], target_u, 0.18)
		wind_v[c] = lerpf(wind_v0[c], target_v, 0.18)

		var speed: float = Vector2(wind_u0[c], wind_v0[c]).length()
		var upwind: int = _upwind_neighbor(c, d, wind_u0[c], wind_v0[c], grid)
		var courant: float = clampf(speed * dt / maxf(grid.cell_size[c], 1.0), 0.0, MAX_COURANT)
		var p_adv: float = lerpf(p_here, pressure0[upwind], courant)
		var t_adv: float = lerpf(t_here, temp0[upwind], courant)
		var q_adv: float = lerpf(vapor0[c], vapor0[upwind], courant)
		var cloud_adv: float = lerpf(cloud0[c], cloud0[upwind], courant * 0.88)
		var high_adv: float = lerpf(high0[c], high0[upwind], courant * 0.72)

		var temp_anom: float = t_adv - fields.temp_mean[c]
		var p_new: float = p_adv + lap_p * 0.050
		p_new += (forcing[c] - p_adv) * 0.004
		p_new -= temp_anom * 0.020
		pressure[c] = clampf(p_new, -15.0, 15.0)

		var t_new: float = t_adv + lap_t * 0.020
		t_new += (fields.temp_mean[c] - t_adv) * 0.010
		temp[c] = t_new

		var qsat: float = _qsat_norm(t_new)
		var climate_target: float = clampf(fields.humidity[c] * qsat, 0.0, 1.45)
		var water_source: float = 0.0
		if fields.elev[c] < 0.0 or fields.lake_level[c] > -1e8:
			water_source = 1.0
		else:
			water_source = clampf(fields.soil_moisture[c] * 0.72 + fields.wetland[c] * 0.45 + fields.vegetation[c] * 0.12, 0.0, 1.0)
		var source_target: float = maxf(climate_target, qsat * lerpf(0.40, 0.90, water_source))
		var source_rate: float = lerpf(0.005, 0.022, water_source)
		var q: float = q_adv + maxf(source_target - q_adv, 0.0) * source_rate

		var rh_before: float = clampf(q / maxf(qsat, 0.02), 0.0, 1.45)
		var oro: float = _orographic_lift_with_grid(c, wind_u[c], wind_v[c], grid, fields)
		var low: float = NoiseKit.smoothstepf(1.0, 8.0, -pressure[c])
		var front: float = clampf(grad_t.length() * 9000.0 + grad_p.length() * 65000.0, 0.0, 1.0)
		var warm: float = NoiseKit.smoothstepf(8.0, 29.0, t_new)
		var lift: float = clampf(oro * 0.52 + low * 0.25 + front * 0.36 + fields.storm_risk[c] * 0.20, 0.0, 1.0)
		var condense_rh: float = lerpf(0.95, 0.78, lift)
		var condensation: float = maxf(rh_before - condense_rh, 0.0) * 0.40
		q = maxf(q - condensation * qsat * 0.34, 0.0)
		var cw: float = cloud_adv + condensation
		var rh: float = clampf(q / maxf(qsat, 0.02), 0.0, 1.40)
		cw -= maxf(0.79 - rh, 0.0) * 0.050
		cw = clampf(cw, 0.0, 1.35)

		var cv: float = clampf(warm * rh * (lift * 0.82 + fields.storm_risk[c] * 0.38) * NoiseKit.smoothstepf(0.08, 0.62, cw), 0.0, 1.0)
		var rain: float = clampf(maxf(cw - 0.52, 0.0) * (0.52 + cv * 1.18) + cv * cw * 0.18, 0.0, 1.0)
		cw = maxf(cw - rain * 0.075, 0.0)
		q = maxf(q - rain * qsat * 0.012, 0.0)
		vapor[c] = q
		cloud[c] = cw
		precip[c] = rain
		convective[c] = cv

		var hi: float = high_adv * 0.984
		hi += cv * 0.050 + front * NoiseKit.smoothstepf(0.70, 0.98, rh) * 0.022
		high[c] = clampf(hi, 0.0, 1.0)
		coverage[c] = _coverage_from(cw, rh, low, front, oro)

		# Candidate genesis is computed from the actual instantaneous atmosphere.
		var lat_deg: float = absf(rad_to_deg(asin(clampf(d.y, -1.0, 1.0))))
		if fields.elev[c] < 0.0:
			var tropical_band: float = NoiseKit.smoothstepf(6.0, 11.0, lat_deg) * (1.0 - NoiseKit.smoothstepf(32.0, 40.0, lat_deg))
			var warm_sea: float = NoiseKit.smoothstepf(23.5, 29.0, fields.temp_mean[c] + 2.0)
			var tc_score: float = tropical_band * warm_sea * rh * (0.35 + low * 0.45 + fields.storm_risk[c] * 0.35)
			if tc_score > best_tc_score:
				best_tc_score = tc_score
				best_tc_cell = c
		else:
			var sq_score: float = warm * rh * front * (0.45 + fields.storm_risk[c] * 0.70)
			if sq_score > best_sq_score:
				best_sq_score = sq_score
				best_sq_cell = c

	# Spawn only occasionally. Existing systems evolve rather than being replaced
	# by a new random mask every update.
	if spawn_timer <= 0.0 and events.size() < MAX_EVENTS:
		var spawned := false
		if best_tc_cell >= 0 and best_tc_score > 0.42 and _count_event_type(events, EVENT_TROPICAL_CYCLONE) < 2:
			var tc_dir: Vector3 = grid.cell_dir(best_tc_cell)
			if _far_from_events(tc_dir, events, 650000.0, grid.radius):
				events.append(_make_tropical_event(best_tc_cell, clampf(best_tc_score, 0.45, 1.0), grid, fields, next_event_id))
				next_event_id += 1
				spawned = true
		if not spawned and best_sq_cell >= 0 and best_sq_score > 0.32 and _count_event_type(events, EVENT_SQUALL_LINE) < 4:
			var sq_dir: Vector3 = grid.cell_dir(best_sq_cell)
			if _far_from_events(sq_dir, events, 350000.0, grid.radius):
				events.append(_make_squall_event(best_sq_cell, clampf(best_sq_score, 0.38, 1.0), grid, fields, next_event_id))
				next_event_id += 1
		spawn_timer = EVENT_SPAWN_PERIOD

	var next: Dictionary = {
		"pressure": pressure,
		"temp": temp,
		"vapor": vapor,
		"cloud": cloud,
		"high": high,
		"precip": precip,
		"coverage": coverage,
		"convective": convective,
		"wind_u": wind_u,
		"wind_v": wind_v,
		"events": events,
		"spawn_timer": spawn_timer,
		"next_event_id": next_event_id,
	}
	_apply_events_to_state(next, events, grid, fields)
	return next

func _fields_blend_wind(climate: float, geostrophic: float, weight: float) -> float:
	return climate * (0.72 - weight * 0.12) + geostrophic * (0.28 + weight * 0.12)

func _upwind_neighbor(c: int, d: Vector3, wu: float, wv: float, grid: PlanetGrid) -> int:
	var tb: Array = CubeSphere.tangent_basis(d)
	var wind: Vector3 = (tb[0] as Vector3) * wu + (tb[1] as Vector3) * wv
	if wind.length_squared() < 0.01:
		return c
	var desired: Vector3 = -wind.normalized()
	var best: int = c
	var best_dot: float = -2.0
	var base: int = c * 8
	for k in 8:
		var nb: int = grid.nbr[base + k]
		var nd: Vector3 = grid.cell_dir(nb)
		var tangent: Vector3 = nd - d * d.dot(nd)
		if tangent.length_squared() < 1e-10:
			continue
		var score: float = tangent.normalized().dot(desired)
		if score > best_dot:
			best_dot = score
			best = nb
	return best

func _coverage_from(cw: float, rh: float, low: float, front: float, oro: float) -> float:
	var condensate: float = NoiseKit.smoothstepf(0.050, 0.52, cw)
	var stratiform: float = NoiseKit.smoothstepf(0.84, 1.02, rh) * clampf(low * 0.40 + front * 0.52 + oro * 0.28, 0.0, 1.0)
	return clampf(maxf(condensate, stratiform * 0.82), 0.0, 1.0)

func _orographic_lift(c: int, wu: float, wv: float) -> float:
	return _orographic_lift_with_grid(c, wu, wv, _grid, _fields)

func _orographic_lift_with_grid(c: int, wu: float, wv: float,
		grid: PlanetGrid, fields: PlanetFields) -> float:
	var d: Vector3 = grid.cell_dir(c)
	var tb: Array = CubeSphere.tangent_basis(d)
	var wind: Vector3 = (tb[0] as Vector3) * wu + (tb[1] as Vector3) * wv
	if wind.length_squared() < 0.04:
		return 0.0
	var upwind: Vector3 = -wind.normalized()
	var base: int = c * 8
	var best_dot: float = -2.0
	var best_h: float = fields.elev[c]
	for k in 8:
		var nb: int = grid.nbr[base + k]
		var nd: Vector3 = grid.cell_dir(nb)
		var tangent: Vector3 = nd - d * d.dot(nd)
		if tangent.length_squared() < 1e-10:
			continue
		var score: float = tangent.normalized().dot(upwind)
		if score > best_dot:
			best_dot = score
			best_h = fields.elev[nb]
	return clampf((fields.elev[c] - best_h) / 850.0, 0.0, 1.0)

# ------------------------------------------------------- weather events ---
func _seed_initial_events() -> Array:
	var best_tc_score: float = -1.0
	var best_tc_cell: int = -1
	var best_sq_score: float = -1.0
	var best_sq_cell: int = -1
	for c in _grid.cell_count:
		var d: Vector3 = _grid.cell_dir(c)
		var lat_deg: float = absf(rad_to_deg(asin(clampf(d.y, -1.0, 1.0))))
		if _fields.elev[c] < 0.0:
			var band: float = NoiseKit.smoothstepf(6.0, 11.0, lat_deg) * (1.0 - NoiseKit.smoothstepf(32.0, 40.0, lat_deg))
			var warm: float = NoiseKit.smoothstepf(23.0, 29.0, _fields.temp_mean[c] + 2.0)
			var score: float = band * warm * _fields.humidity[c] * (0.55 + _fields.storm_risk[c] * 0.55) * (0.82 + _cells.u(d) * 0.24)
			if score > best_tc_score:
				best_tc_score = score
				best_tc_cell = c
		else:
			var warm_land: float = NoiseKit.smoothstepf(10.0, 27.0, _fields.temp_mean[c])
			var score_land: float = warm_land * _fields.humidity[c] * (0.32 + _fields.storm_risk[c] * 0.90) * (0.82 + _cells.u(d) * 0.24)
			if score_land > best_sq_score:
				best_sq_score = score_land
				best_sq_cell = c
	var events: Array = []
	var next_id := 1
	if best_tc_cell >= 0 and best_tc_score > 0.34:
		events.append(_make_tropical_event(best_tc_cell, clampf(best_tc_score, 0.52, 0.82), _grid, _fields, next_id))
		next_id += 1
	if best_sq_cell >= 0 and best_sq_score > 0.26:
		events.append(_make_squall_event(best_sq_cell, clampf(best_sq_score, 0.45, 0.78), _grid, _fields, next_id))
	return events

func _make_tropical_event(c: int, intensity: float, grid: PlanetGrid,
		fields: PlanetFields, id: int) -> Dictionary:
	var center: Vector3 = grid.cell_dir(c)
	var frame: Array = CubeSphere.tangent_basis(center)
	return {
		"id": id,
		"type": EVENT_TROPICAL_CYCLONE,
		"center": center,
		"east": frame[0],
		"north": frame[1],
		"intensity": intensity,
		"radius": lerpf(220000.0, 430000.0, intensity),
		"age": 0.0,
		"life": lerpf(2.0, 6.0, intensity) * 86400.0,
		"spin": 1.0 if center.y >= 0.0 else -1.0,
	}

func _make_squall_event(c: int, intensity: float, grid: PlanetGrid,
		fields: PlanetFields, id: int) -> Dictionary:
	var center: Vector3 = grid.cell_dir(c)
	var frame: Array = CubeSphere.tangent_basis(center)
	var east: Vector3 = frame[0]
	var north: Vector3 = frame[1]
	var motion: Vector3 = east * fields.wind_u[c] + north * fields.wind_v[c]
	if motion.length_squared() < 1.0:
		motion = east
	motion = (motion - center * center.dot(motion)).normalized()
	var axis: Vector3 = center.cross(motion).normalized()
	return {
		"id": id,
		"type": EVENT_SQUALL_LINE,
		"center": center,
		"motion": motion,
		"axis": axis,
		"intensity": intensity,
		"length": lerpf(420000.0, 1050000.0, intensity),
		"width": lerpf(75000.0, 180000.0, intensity),
		"age": 0.0,
		"life": lerpf(6.0, 18.0, intensity) * 3600.0,
	}

func _advance_events(src: Dictionary, dt: float, grid: PlanetGrid, fields: PlanetFields) -> Array:
	var source_events: Array = src.get("events", [])
	var wind_u0: PackedFloat32Array = src["wind_u"]
	var wind_v0: PackedFloat32Array = src["wind_v"]
	var out: Array = []
	for item in source_events:
		var ev: Dictionary = (item as Dictionary).duplicate(true)
		var age: float = float(ev.get("age", 0.0)) + dt
		var life: float = float(ev.get("life", 1.0))
		if age >= life:
			continue
		var center: Vector3 = ev["center"]
		var c: int = grid.dir_to_index(center)
		var frame: Array = CubeSphere.tangent_basis(center)
		var east: Vector3 = frame[0]
		var north: Vector3 = frame[1]
		var flow: Vector3 = east * wind_u0[c] + north * wind_v0[c]
		var kind: int = int(ev["type"])
		var intensity: float = float(ev["intensity"])
		var motion: Vector3
		if kind == EVENT_TROPICAL_CYCLONE:
			# Steering flow plus a weak poleward drift. Genesis/decay responds to
			# warm water and landfall instead of following a scripted track.
			flow += north * (1.4 if center.y >= 0.0 else -1.4)
			if flow.length_squared() < 1.0:
				flow = -east
			motion = flow.normalized()
			var speed: float = clampf(flow.length() * 0.42 + 3.0, 3.5, 16.0)
			center = (center + motion * (speed * dt / grid.radius)).normalized()
			c = grid.dir_to_index(center)
			var ocean: bool = fields.elev[c] < 0.0
			var warm: float = NoiseKit.smoothstepf(23.5, 29.0, fields.temp_mean[c] + 2.0)
			var sustain: float = warm * fields.humidity[c] if ocean else 0.0
			var target: float = clampf(0.22 + sustain * 0.90, 0.0, 1.0)
			intensity += (target - intensity) * clampf(dt / 18000.0, 0.0, 0.20)
			if not ocean:
				intensity -= dt / 18000.0 * 0.22
			intensity = clampf(intensity, 0.0, 1.0)
			ev["radius"] = minf(float(ev["radius"]) + dt * 1.2, 520000.0)
			var new_frame: Array = CubeSphere.tangent_basis(center)
			ev["east"] = new_frame[0]
			ev["north"] = new_frame[1]
		else:
			if flow.length_squared() < 1.0:
				flow = ev.get("motion", east)
			motion = (flow - center * center.dot(flow)).normalized()
			var line_speed: float = clampf(flow.length() * 0.72 + 5.0, 7.0, 28.0)
			center = (center + motion * (line_speed * dt / grid.radius)).normalized()
			c = grid.dir_to_index(center)
			var warm_land: float = NoiseKit.smoothstepf(8.0, 27.0, fields.temp_mean[c])
			var sustain_line: float = warm_land * fields.humidity[c] * (0.35 + fields.storm_risk[c] * 0.85)
			intensity += (sustain_line - intensity) * clampf(dt / 9000.0, 0.0, 0.24)
			intensity -= dt / maxf(life, 1.0) * 0.22
			intensity = clampf(intensity, 0.0, 1.0)
			motion = (motion - center * center.dot(motion)).normalized()
			ev["motion"] = motion
			ev["axis"] = center.cross(motion).normalized()
			ev["length"] = minf(float(ev["length"]) + dt * 5.0, 1300000.0)
		if intensity < 0.10:
			continue
		ev["center"] = center
		ev["intensity"] = intensity
		ev["age"] = age
		out.append(ev)
	return out

func _apply_events_to_state(state: Dictionary, events: Array, grid: PlanetGrid,
		fields: PlanetFields) -> void:
	if events.is_empty():
		return
	var pressure: PackedFloat32Array = state["pressure"]
	var cloud: PackedFloat32Array = state["cloud"]
	var high: PackedFloat32Array = state["high"]
	var precip: PackedFloat32Array = state["precip"]
	var coverage: PackedFloat32Array = state["coverage"]
	var convective: PackedFloat32Array = state["convective"]
	var wind_u: PackedFloat32Array = state["wind_u"]
	var wind_v: PackedFloat32Array = state["wind_v"]

	for c in grid.cell_count:
		var d: Vector3 = grid.cell_dir(c)
		var cell_frame: Array = CubeSphere.tangent_basis(d)
		var cell_east: Vector3 = cell_frame[0]
		var cell_north: Vector3 = cell_frame[1]
		for item in events:
			var ev: Dictionary = item
			var center: Vector3 = ev["center"]
			var dot_center: float = clampf(center.dot(d), -1.0, 1.0)
			var kind: int = int(ev["type"])
			var intensity: float = float(ev["intensity"])
			if kind == EVENT_TROPICAL_CYCLONE:
				var radius: float = float(ev["radius"])
				var max_angle: float = radius * 1.55 / grid.radius
				if dot_center < cos(max_angle):
					continue
				var dist: float = acos(dot_center) * grid.radius
				var x: float = dist / maxf(radius, 1.0)
				var outer: float = 1.0 - NoiseKit.smoothstepf(1.05, 1.55, x)
				if outer <= 0.0:
					continue
				var rel: Vector3 = d - center * dot_center
				if rel.length_squared() < 1e-12:
					rel = ev["east"]
				else:
					rel = rel.normalized()
				var ev_east: Vector3 = ev["east"]
				var ev_north: Vector3 = ev["north"]
				var angle: float = atan2(rel.dot(ev_north), rel.dot(ev_east))
				var eye: float = 1.0 - NoiseKit.smoothstepf(0.045, 0.135, x)
				var eyewall: float = exp(-pow((x - 0.18) / 0.075, 2.0))
				var spiral_phase: float = 0.5 + 0.5 * cos(angle * 3.0 - x * 12.0)
				var spiral: float = pow(maxf(spiral_phase, 0.0), 3.0) * outer * NoiseKit.smoothstepf(0.22, 0.95, x)
				var storm: float = intensity * maxf(eyewall, outer * (0.22 + spiral * 0.76))
				pressure[c] = clampf(pressure[c] - outer * intensity * 7.5, -18.0, 15.0)
				coverage[c] = maxf(coverage[c], clampf(0.16 + storm * 0.84, 0.0, 1.0))
				cloud[c] = maxf(cloud[c], clampf(0.20 + storm * 1.05, 0.0, 1.35))
				convective[c] = maxf(convective[c], clampf(eyewall * intensity + spiral * intensity * 0.82, 0.0, 1.0))
				high[c] = maxf(high[c], clampf(outer * intensity * (0.48 + storm * 0.42), 0.0, 1.0))
				precip[c] = maxf(precip[c], clampf(storm * 1.05, 0.0, 1.0))
				# Rotating inflow. The eye is calm; strongest winds sit in the eyewall.
				var toward: Vector3 = center - d * dot_center
				if toward.length_squared() > 1e-10:
					toward = toward.normalized()
					var spin: float = float(ev["spin"])
					var around: Vector3 = d.cross(toward).normalized() * spin
					var wind_ring: float = exp(-pow((x - 0.22) / 0.32, 2.0)) * intensity
					var max_wind: float = 24.0 + intensity * 48.0
					var storm_wind: Vector3 = around * (max_wind * wind_ring) + toward * (5.0 * outer * intensity)
					wind_u[c] += storm_wind.dot(cell_east)
					wind_v[c] += storm_wind.dot(cell_north)
				# Clear the eye after all cloud additions so background stratiform
				# cloud cannot fill it back in.
				if eye > 0.0:
					var clear_factor: float = 1.0 - eye * 0.96
					coverage[c] *= clear_factor
					cloud[c] *= clear_factor
					convective[c] *= 1.0 - eye * 0.92
					precip[c] *= 1.0 - eye * 0.96
					high[c] *= 1.0 - eye * 0.72
			else:
				var length: float = float(ev["length"])
				var width: float = float(ev["width"])
				var max_dist: float = length * 0.72
				if dot_center < cos(max_dist / grid.radius):
					continue
				var dist_line: float = acos(dot_center) * grid.radius
				var rel_line: Vector3 = d - center * dot_center
				if rel_line.length_squared() < 1e-12:
					continue
				rel_line = rel_line.normalized()
				var axis: Vector3 = ev["axis"]
				var motion: Vector3 = ev["motion"]
				var along: float = rel_line.dot(axis) * dist_line
				var across: float = rel_line.dot(motion) * dist_line
				var half_len: float = maxf(length * 0.5, 1.0)
				var along_env: float = exp(-pow(absf(along) / half_len, 4.0))
				var bow: float = 0.18 * (along * along / half_len)
				var front_offset: float = across - bow
				var line_core: float = along_env * exp(-pow(front_offset / maxf(width * 0.22, 1.0), 2.0))
				var trailing: float = along_env * exp(-pow((front_offset + width * 0.42) / maxf(width * 0.70, 1.0), 2.0))
				var storm_line: float = intensity * maxf(line_core, trailing * 0.58)
				if storm_line < 0.005:
					continue
				coverage[c] = maxf(coverage[c], clampf(line_core * intensity * 0.95 + trailing * intensity * 0.54, 0.0, 1.0))
				cloud[c] = maxf(cloud[c], clampf(0.18 + storm_line * 1.10, 0.0, 1.35))
				convective[c] = maxf(convective[c], clampf(line_core * intensity * 1.12, 0.0, 1.0))
				high[c] = maxf(high[c], clampf(trailing * intensity * 0.92 + line_core * intensity * 0.38, 0.0, 1.0))
				precip[c] = maxf(precip[c], clampf(line_core * intensity * 1.10 + trailing * intensity * 0.38, 0.0, 1.0))
				pressure[c] = clampf(pressure[c] - storm_line * 2.8, -18.0, 15.0)
				var gust: Vector3 = motion * (18.0 + intensity * 28.0) * line_core
				wind_u[c] += gust.dot(cell_east)
				wind_v[c] += gust.dot(cell_north)

	state["pressure"] = pressure
	state["cloud"] = cloud
	state["high"] = high
	state["precip"] = precip
	state["coverage"] = coverage
	state["convective"] = convective
	state["wind_u"] = wind_u
	state["wind_v"] = wind_v

func _count_event_type(events: Array, kind: int) -> int:
	var count := 0
	for item in events:
		var ev: Dictionary = item
		if int(ev["type"]) == kind:
			count += 1
	return count

func _far_from_events(d: Vector3, events: Array, metres: float, radius: float) -> bool:
	var min_angle: float = metres / maxf(radius, 1.0)
	var min_dot: float = cos(min_angle)
	for item in events:
		var ev: Dictionary = item
		var center: Vector3 = ev["center"]
		if center.dot(d) > min_dot:
			return false
	return true

func active_events() -> Array:
	var source: Array = _state.get("events", [])
	var out: Array = []
	for item in source:
		var ev: Dictionary = item
		var center: Vector3 = ev["center"]
		var ll: Vector2 = CubeSphere.dir_to_latlon(center)
		var kind: int = int(ev["type"])
		var label := "Squall line / straight-line wind storm"
		if kind == EVENT_TROPICAL_CYCLONE:
			var lon_deg: float = rad_to_deg(ll.y)
			if lon_deg > 100.0 and lon_deg < 180.0:
				label = "Typhoon"
			elif lon_deg > -100.0 and lon_deg < 20.0:
				label = "Hurricane"
			else:
				label = "Tropical cyclone"
		out.append({
			"id": int(ev["id"]),
			"type": kind,
			"name": label,
			"latitude": rad_to_deg(ll.x),
			"longitude": rad_to_deg(ll.y),
			"intensity": float(ev["intensity"]),
			"age_hours": float(ev["age"]) / 3600.0,
		})
	return out

# ----------------------------------------------------------- GPU maps ---
func _build_images(state: Dictionary, grid: PlanetGrid, fields: PlanetFields) -> Dictionary:
	var res: int = grid.res
	var tex_res: int = res + 2
	var cell_step: float = 2.0 / float(res)
	var weather_images: Array[Image] = []
	var wind_images: Array[Image] = []
	var coverage: PackedFloat32Array = state["coverage"]
	var convective: PackedFloat32Array = state["convective"]
	var high: PackedFloat32Array = state["high"]
	var precip: PackedFloat32Array = state["precip"]
	var temp: PackedFloat32Array = state["temp"]
	var vapor: PackedFloat32Array = state["vapor"]
	var wind_u: PackedFloat32Array = state["wind_u"]
	var wind_v: PackedFloat32Array = state["wind_v"]

	for face in 6:
		var wimg: Image = Image.create(tex_res, tex_res, false, Image.FORMAT_RGBAH)
		var fimg: Image = Image.create(tex_res, tex_res, false, Image.FORMAT_RGBAH)
		for y in tex_res:
			var j: int = y - 1
			var v: float = (float(j) + 0.5) * cell_step - 1.0
			for x in tex_res:
				var i: int = x - 1
				var c: int
				if i >= 0 and i < res and j >= 0 and j < res:
					c = grid.idx(face, i, j)
				else:
					var u: float = (float(i) + 0.5) * cell_step - 1.0
					var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
					c = grid.dir_to_index(d)
				var qsat: float = _qsat_norm(temp[c])
				var rh: float = clampf(vapor[c] / maxf(qsat, 0.02), 0.02, 1.0)
				var dew: float = _dewpoint_c(temp[c], rh)
				# Normal cloud is intentionally kept clear of terrain. Fog/stratus can
				# later be a dedicated near-surface system instead of letting the main
				# volumetric shell cut through mountains.
				var lcl: float = clampf((temp[c] - dew) * 125.0, 650.0, 4300.0)
				var terrain_guard: float = maxf(fields.elev[c], 0.0)
				if fields.elev[c] >= 0.0:
					terrain_guard += minf(maxf(fields.relief[c], 0.0) * 0.38, 1900.0)
				var clearance: float = lerpf(650.0, 1050.0, convective[c])
				var terrain_floor: float = terrain_guard + clearance
				var cloud_base: float = clampf(maxf(lcl, terrain_floor), 700.0, 6500.0)
				wimg.set_pixel(x, y, Color(coverage[c], convective[c], high[c], cloud_base / 6000.0))
				fimg.set_pixel(x, y, Color(wind_u[c], wind_v[c], precip[c], rh))
		weather_images.append(wimg)
		wind_images.append(fimg)
	return {"weather": weather_images, "wind": wind_images}

func _publish_images(built: Dictionary) -> void:
	if built.is_empty():
		return
	var weather_images: Array[Image] = built["weather"]
	var wind_images: Array[Image] = built["wind"]
	if weather_map == null:
		weather_map = Texture2DArray.new()
	var err: Error = weather_map.create_from_images(weather_images)
	if err != OK:
		push_error("Failed to build weather texture array (%d)" % err)
		return
	if wind_map == null:
		wind_map = Texture2DArray.new()
	err = wind_map.create_from_images(wind_images)
	if err != OK:
		push_error("Failed to build wind texture array (%d)" % err)

func _qsat_norm(temp_c: float) -> float:
	var t: float = clampf(temp_c, -55.0, 45.0)
	var es: float = exp(17.625 * t / (243.04 + t))
	var ref: float = exp(17.625 * 30.0 / (243.04 + 30.0))
	return clampf(es / ref, 0.035, 1.65)

func _dewpoint_c(temp_c: float, rh: float) -> float:
	var a: float = 17.625
	var b: float = 243.04
	var safe_rh: float = clampf(rh, 0.02, 1.0)
	var gamma: float = log(safe_rh) + a * temp_c / (b + temp_c)
	return b * gamma / (a - gamma)
