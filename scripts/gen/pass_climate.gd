class_name PassClimate
extends RefCounted
## 1.5 (first third) -- climate fields.
##
## Temperature, seasonal range, rainfall, humidity, prevailing winds and
## severe-weather likelihood. A three-cell circulation gives the zonal wind bands;
## rainfall is advected moisture depleted by distance from the sea and by upwind
## orographic barriers, so rain shadows and continental interiors emerge instead
## of being authored.
##
## Runs twice per bake: provisionally on the raw crust (so erosion knows where it
## rains) and finally on the eroded surface.

const UPWIND_STEPS := 12

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

var dist_to_ocean := PackedFloat32Array()
var upwind_slot := PackedByteArray()

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	var n := grid.cell_count
	_winds()
	if progress.is_valid():
		progress.call("Climate: moisture transport", 0.2)
	_distance_to_ocean()
	_upwind_slots()

	var s := cfg.stream_seed("climate")
	var jitter := NoiseKit.new(s + 1, 4.5, 4)
	var wet_n := NoiseKit.new(s + 2, 6.5, 4)
	var tilt := deg_to_rad(cfg.axial_tilt_deg)

	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Climate", 0.3 + 0.7 * float(c) / float(n))
		var d := grid.cell_dir(c)
		var h: float = fields.elev[c]
		var land_h := maxf(h, 0.0)
		var latn := absf(clampf(d.y, -1.0, 1.0))          # 0 equator .. 1 pole
		var lat_deg := rad_to_deg(asin(latn))

		# --- temperature ---------------------------------------------------
		# Insolation falls off as sin^2(latitude); polar_bias > 1 steepens that,
		# which is what makes Asterra's ice caps bigger than Earth's.
		var polar_t := pow(latn * latn, 1.0 / clampf(cfg.polar_bias, 0.4, 3.0))
		var t := lerpf(cfg.equator_temp, cfg.pole_temp, clampf(polar_t, 0.0, 1.0))
		t -= land_h * cfg.lapse_rate
		t += jitter.s(d) * 1.6
		# Oceans and coasts are thermally buffered; interiors are not.
		var contin := clampf(dist_to_ocean[c] / 520000.0, 0.0, 1.0)
		if h < 0.0:
			t += 3.2 * (1.0 - polar_t * 0.5)
		else:
			t += 2.4 * (1.0 - contin)
		fields.temp_mean[c] = t

		var rng := (3.5 + 30.0 * pow(latn, 1.25)) * (0.30 + 0.85 * contin)
		rng *= (0.55 + 0.9 * (tilt / deg_to_rad(23.44)))
		fields.temp_range[c] = rng

		# --- precipitation ---------------------------------------------------
		var zonal := _zonal_precip(lat_deg)
		var moisture := exp(-dist_to_ocean[c] / 780000.0)
		moisture = clampf(0.12 + 0.95 * moisture, 0.0, 1.1)
		# Orographic lift on the windward side, rain shadow behind the barrier.
		var barrier := _upwind_barrier(c)
		var rise := land_h - barrier.x                    # x = upwind elevation
		var windward := clampf(rise / 900.0, -1.0, 1.5)
		var shadow := clampf((barrier.y - land_h) / 1500.0, 0.0, 1.0)   # y = max upwind peak
		var p := cfg.base_precip * zonal * moisture
		p *= 1.0 + cfg.orographic_gain * maxf(0.0, windward) * 0.45
		p *= 1.0 - 0.72 * shadow
		# Cold air simply holds less water (Clausius-Clapeyron, crudely).
		p *= clampf(0.30 + 0.042 * (t + 12.0), 0.10, 1.5)
		p *= 0.72 + 0.56 * wet_n.u(d)
		if h < 0.0:
			p = maxf(p, cfg.base_precip * zonal * 0.85)
		fields.precip[c] = maxf(p, 8.0)

		# --- humidity ---------------------------------------------------------
		var hum := clampf(fields.precip[c] / 1800.0, 0.0, 1.0) * 0.7 + moisture * 0.3
		if h < 0.0:
			hum = maxf(hum, 0.8)
		fields.humidity[c] = clampf(hum, 0.02, 1.0)

	_severe_weather()

# ------------------------------------------------------------------ winds ---
func _winds() -> void:
	var s := cfg.stream_seed("wind")
	var wn := NoiseKit.new(s + 1, 3.2, 3)
	for c in grid.cell_count:
		var d := grid.cell_dir(c)
		var lat := asin(clampf(d.y, -1.0, 1.0))
		var band := rad_to_deg(absf(lat))
		var sgn := 1.0 if d.y >= 0.0 else -1.0
		var u := 0.0
		var v := 0.0
		if band < 30.0:
			u = -6.5 * sin(PI * band / 30.0)
			v = -sgn * 2.6 * sin(PI * band / 30.0)
		elif band < 60.0:
			u = 9.5 * sin(PI * (band - 30.0) / 30.0)
			v = sgn * 1.8 * sin(PI * (band - 30.0) / 30.0)
		else:
			u = -5.0 * sin(PI * (band - 60.0) / 30.0)
			v = -sgn * 1.2 * sin(PI * (band - 60.0) / 30.0)
		u += wn.s(d) * 2.2
		v += wn.s(Vector3(d.y, d.z, d.x)) * 1.6
		fields.wind_u[c] = u
		fields.wind_v[c] = v

# --------------------------------------------------------- moisture source ---
func _distance_to_ocean() -> void:
	var n := grid.cell_count
	dist_to_ocean.resize(n)
	var heap := MinHeap.new()
	for c in n:
		if fields.elev[c] < 0.0:
			dist_to_ocean[c] = 0.0
			heap.push(0.0, c)
		else:
			dist_to_ocean[c] = 1e30
	while not heap.is_empty():
		var top := heap.pop()
		var dv: float = top[0]
		var c: int = top[1]
		if dv > dist_to_ocean[c]:
			continue
		var base := c * 8
		var cs: float = grid.cell_size[c]
		for k in 8:
			var nb := grid.nbr[base + k]
			var step := cs * (1.41421356 if (k & 1) == 1 else 1.0)
			var nd := dv + step
			if nd < dist_to_ocean[nb]:
				dist_to_ocean[nb] = nd
				heap.push(nd, nb)
	for c in n:
		if dist_to_ocean[c] > 1e29:
			dist_to_ocean[c] = 2.0e6

func _upwind_slots() -> void:
	var n := grid.cell_count
	upwind_slot.resize(n)
	for c in n:
		var d := grid.cell_dir(c)
		var tb := CubeSphere.tangent_basis(d)
		var east: Vector3 = tb[0]
		var north: Vector3 = tb[1]
		var wind := (east * fields.wind_u[c] + north * fields.wind_v[c])
		if wind.length() < 1e-4:
			upwind_slot[c] = 0
			continue
		var up := -wind.normalized()
		var base := c * 8
		var best := -2.0
		var best_k := 0
		for k in 8:
			var nb := grid.nbr[base + k]
			var seg := (grid.cell_dir(nb) - d)
			if seg.length() < 1e-9:
				continue
			var dp := seg.normalized().dot(up)
			if dp > best:
				best = dp
				best_k = k
		upwind_slot[c] = best_k

## Returns (elevation one step upwind, highest elevation within UPWIND_STEPS).
func _upwind_barrier(c: int) -> Vector2:
	var cur := c
	var first := maxf(0.0, fields.elev[c])
	var peak := first
	for i in UPWIND_STEPS:
		cur = grid.nbr[cur * 8 + upwind_slot[cur]]
		var h := maxf(0.0, fields.elev[cur])
		if i == 0:
			first = h
		peak = maxf(peak, h)
	return Vector2(first, peak)

## Latitudinal rainfall profile: ITCZ, subtropical highs, mid-latitude storm track.
func _zonal_precip(lat_deg: float) -> float:
	var itcz := 1.15 * exp(-pow(lat_deg / 14.0, 2.0))
	var subtrop := -0.48 * exp(-pow((lat_deg - 27.0) / 13.0, 2.0))
	var storm := 0.72 * exp(-pow((lat_deg - 50.0) / 18.0, 2.0))
	var polar := -0.40 * exp(-pow((lat_deg - 90.0) / 24.0, 2.0))
	return clampf(0.78 + itcz + subtrop + storm + polar, 0.08, 2.6)

# -------------------------------------------------------- severe weather ---
func _severe_weather() -> void:
	var s := cfg.stream_seed("storms")
	var sn := NoiseKit.new(s + 1, 3.8, 3)
	for c in grid.cell_count:
		var d := grid.cell_dir(c)
		var lat_deg := rad_to_deg(asin(absf(clampf(d.y, -1.0, 1.0))))
		var t: float = fields.temp_mean[c]
		var hum: float = fields.humidity[c]
		var h: float = fields.elev[c]
		# Tropical cyclogenesis: warm water, away from the equator.
		var cyclone := 0.0
		if h < 0.0 and t > 26.0:
			cyclone = NoiseKit.smoothstepf(7.0, 16.0, lat_deg) * (1.0 - NoiseKit.smoothstepf(28.0, 40.0, lat_deg))
		# Continental convective severe weather: warm moist inflow meeting a dry
		# elevated layer downwind of a mountain barrier -- the "tornado alley"
		# recipe, and the reason it wants flat ground east of a range.
		var alley := 0.0
		if h >= 0.0:
			var lee := clampf((_upwind_barrier(c).y - maxf(0.0, h)) / 1200.0, 0.0, 1.0)
			var warm := NoiseKit.smoothstepf(6.0, 20.0, t)
			var band := NoiseKit.smoothstepf(24.0, 36.0, lat_deg) * (1.0 - NoiseKit.smoothstepf(50.0, 62.0, lat_deg))
			alley = lee * warm * band * clampf(hum * 1.5, 0.0, 1.0) * clampf(fields.temp_range[c] / 26.0, 0.0, 1.4)
		fields.storm_risk[c] = clampf(maxf(cyclone, alley) * (0.75 + 0.5 * sn.u(d)), 0.0, 1.0)
