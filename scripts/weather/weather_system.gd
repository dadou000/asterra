class_name WeatherSystem
extends Node
## Authoritative dynamic weather simulation for Asterra.
##
## The baked climate is climatology: it defines the long-term temperature,
## moisture and prevailing circulation of each place. Runtime weather is a
## separate prognostic state that advects moisture/cloud water, evolves pressure
## systems, responds to terrain lift and continuously condenses/precipitates.
## Every renderer samples this same spherical state, so ground and orbit weather
## cannot disagree.
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

## Broad weather does not need to run every frame. Fine cloud motion is advected
## continuously in the raymarch shader while this model advances synoptic state.
@export var update_interval: float = 2.0
## Simulated seconds per real second. 120x makes systems visibly evolve during a
## test flight without making a front cross a continent in a few seconds.
@export var simulation_rate: float = 120.0

const MAX_COURANT := 0.42
const GEO_SCALE := 220000.0

var weather_map: Texture2DArray
var wind_map: Texture2DArray
var face_res: int = 0
var simulation_seconds: float = 0.0

var _cfg: GenConfig
var _grid: PlanetGrid
var _fields: PlanetFields
var _synoptic: NoiseKit
var _fronts: NoiseKit
var _cells: NoiseKit

## Prognostic state. Packed arrays are copy-on-write, which lets a worker read a
## stable snapshot while the render thread keeps using the previous published map.
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
	_synoptic = NoiseKit.new(_cfg.stream_seed("weather_synoptic"), 2.25, 4)
	_fronts = NoiseKit.ridged(_cfg.stream_seed("weather_fronts"), 4.2, 4)
	_cells = NoiseKit.new(_cfg.stream_seed("weather_cells"), 7.5, 3)
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

	# First establish a coherent synoptic atmosphere. Ocean humidity is a moisture
	# reservoir, not an instruction to cover every ocean pixel with cumulus.
	for c in n:
		var d: Vector3 = _grid.cell_dir(c)
		var syn: float = _synoptic.s(d)
		var secondary: float = _cells.s(Vector3(d.z, d.x, d.y))
		var p: float = -syn * 7.0 + secondary * 1.4
		pressure[c] = p
		_pressure_forcing[c] = p
		var t: float = _fields.temp_mean[c] + _cells.s(d) * 2.2
		temp[c] = t
		var qsat: float = _qsat_norm(t)
		vapor[c] = clampf(_fields.humidity[c] * qsat, 0.0, 1.5)
		wind_u[c] = _fields.wind_u[c]
		wind_v[c] = _fields.wind_v[c]

	# Then seed condensate only inside organised moist systems. This immediately
	# fixes the old ocean-wide popcorn field while still allowing marine decks.
	for c in n:
		var d: Vector3 = _grid.cell_dir(c)
		var rh: float = clampf(vapor[c] / maxf(_qsat_norm(temp[c]), 0.02), 0.0, 1.35)
		var low: float = NoiseKit.smoothstepf(0.5, 6.5, -pressure[c])
		var front: float = clampf(absf(_fronts.s(d)), 0.0, 1.0)
		var oro: float = _orographic_lift(c, wind_u[c], wind_v[c])
		var warm: float = NoiseKit.smoothstepf(8.0, 27.0, temp[c])
		var sat: float = NoiseKit.smoothstepf(0.70, 0.98, rh)
		var organisation: float = clampf(low * 0.58 + front * 0.25 + oro * 0.30 + _fields.storm_risk[c] * 0.22, 0.0, 1.0)
		cloud[c] = sat * organisation * 0.92
		var cv: float = clampf(warm * rh * (_fields.storm_risk[c] * 0.55 + oro * 0.42 + low * 0.18), 0.0, 1.0)
		convective[c] = cv
		high[c] = clampf(front * sat * 0.24 + cv * 0.42, 0.0, 1.0)
		precip[c] = clampf(maxf(cloud[c] - 0.48, 0.0) * 1.7 + cv * cloud[c] * 0.45, 0.0, 1.0)
		coverage[c] = _coverage_from(cloud[c], rh, low, front, oro)

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
	}

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

		# Pressure-gradient circulation. The baked wind remains the climatological
		# backbone while evolving highs/lows bend and strengthen it.
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

		# Synoptic systems advect and diffuse, but weakly relax toward a broad
		# planetary forcing so numerical diffusion never erases all weather.
		var temp_anom: float = t_adv - fields.temp_mean[c]
		var p_new: float = p_adv + lap_p * 0.055
		p_new += (forcing[c] - p_adv) * 0.006
		p_new -= temp_anom * 0.020
		pressure[c] = clampf(p_new, -13.0, 13.0)

		var t_new: float = t_adv + lap_t * 0.020
		t_new += (fields.temp_mean[c] - t_adv) * 0.012
		temp[c] = t_new

		# Evapotranspiration restores moisture toward the local climatology, with
		# ocean/lakes acting as fast reservoirs and dry land responding to soil.
		var qsat: float = _qsat_norm(t_new)
		var climate_target: float = clampf(fields.humidity[c] * qsat, 0.0, 1.45)
		var water_source: float = 0.0
		if fields.elev[c] < 0.0 or fields.lake_level[c] > -1e8:
			water_source = 1.0
		else:
			water_source = clampf(fields.soil_moisture[c] * 0.72 + fields.wetland[c] * 0.45 + fields.vegetation[c] * 0.12, 0.0, 1.0)
		var source_target: float = maxf(climate_target, qsat * lerpf(0.42, 0.92, water_source))
		var source_rate: float = lerpf(0.006, 0.030, water_source)
		var q: float = q_adv + maxf(source_target - q_adv, 0.0) * source_rate

		var rh_before: float = clampf(q / maxf(qsat, 0.02), 0.0, 1.45)
		var oro: float = _orographic_lift_with_grid(c, wind_u[c], wind_v[c], grid, fields)
		var low: float = NoiseKit.smoothstepf(0.8, 7.5, -pressure[c])
		var front: float = clampf(grad_t.length() * 90000.0 / 10.0 + grad_p.length() * 500000.0 / 8.0, 0.0, 1.0)
		var warm: float = NoiseKit.smoothstepf(8.0, 29.0, t_new)
		var lift: float = clampf(oro * 0.52 + low * 0.25 + front * 0.30 + fields.storm_risk[c] * 0.20, 0.0, 1.0)
		var condense_rh: float = lerpf(0.94, 0.77, lift)
		var condensation: float = maxf(rh_before - condense_rh, 0.0) * 0.42
		q = maxf(q - condensation * qsat * 0.34, 0.0)
		var cw: float = cloud_adv + condensation
		var rh: float = clampf(q / maxf(qsat, 0.02), 0.0, 1.40)
		# Unsaturated air eats cloud edges; this is the physical large-scale
		# dissipation, distinct from purely visual small-scale erosion in the shader.
		cw -= maxf(0.78 - rh, 0.0) * 0.045
		cw = clampf(cw, 0.0, 1.35)

		var cv: float = clampf(warm * rh * (lift * 0.82 + fields.storm_risk[c] * 0.38) * NoiseKit.smoothstepf(0.08, 0.62, cw), 0.0, 1.0)
		var rain: float = clampf(maxf(cw - 0.50, 0.0) * (0.55 + cv * 1.15) + cv * cw * 0.18, 0.0, 1.0)
		cw = maxf(cw - rain * 0.075, 0.0)
		q = maxf(q - rain * qsat * 0.012, 0.0)
		vapor[c] = q
		cloud[c] = cw
		precip[c] = rain
		convective[c] = cv

		var hi: float = high_adv * 0.985
		hi += cv * 0.050 + front * NoiseKit.smoothstepf(0.68, 0.98, rh) * 0.018
		high[c] = clampf(hi, 0.0, 1.0)
		coverage[c] = _coverage_from(cw, rh, low, front, oro)

	return {
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
	}

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
	var condensate: float = NoiseKit.smoothstepf(0.035, 0.48, cw)
	var stratiform: float = NoiseKit.smoothstepf(0.82, 1.02, rh) * clampf(low * 0.42 + front * 0.46 + oro * 0.30, 0.0, 1.0)
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
				var lcl: float = clampf((temp[c] - dew) * 125.0, 300.0, 3600.0)
				var terrain_floor: float = maxf(fields.elev[c], 0.0) + lerpf(220.0, 500.0, 1.0 - rh)
				var cloud_base: float = clampf(maxf(lcl, terrain_floor), 350.0, 5200.0)
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
	# Magnus saturation vapour pressure, normalised to 30 C. The state stores an
	# absolute-moisture proxy so cooling an air mass raises RH and forms cloud.
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
