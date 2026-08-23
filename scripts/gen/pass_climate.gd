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
## Temperature is solved rather than authored: `ClimateEBM` closes a zonal energy
## budget (absorbed sunlight = thermal emission + transport divergence, with an
## ice-albedo feedback), and this pass adds the local departures from that zonal
## mean -- elevation, surface albedo, where the ocean puts the heat it carries,
## and how much of the budget evaporation takes. Every one of those is a term in
## a surface energy balance, which is why the seasonal range comes out of the
## same numbers instead of a separate curve.
##
## Runs twice per bake: provisionally on the raw crust (so erosion knows where it
## rains) and finally on the eroded surface.

const UPWIND_STEPS := 12

## Length of a year, s. The seasonal response is a slab driven at this frequency.
const SECONDS_PER_YEAR := 3.156e7

## Volumetric heat capacity of sea water, J/m^3 per deg C.
const SEAWATER_HEAT_CAPACITY := 4.0e6
## 1 mm/yr of evaporation carries this much latent heat away, W/m^2. (1 kg/m^2/yr
## times the latent heat of vaporisation, over the length of a year.) Earth's
## ~1000 mm/yr global mean lands on ~79 W/m^2, which is the observed figure.
const MM_YR_TO_W := 0.0792
## Fractional rise in evaporation per degree of warming (Clausius-Clapeyron near
## 288 K). This is what damps the seasons of a wet place and frees those of a dry
## one: rainforests barely swing, deserts swing hard.
const CLAUSIUS_SLOPE := 0.06
## Land loses part of its rainfall to runoff rather than evaporating it back.
const LAND_EVAP_FRACTION := 0.65
## Evaporation cannot exceed this share of the absorbed shortwave.
const MAX_EVAP_FRACTION := 0.70
## Sensible heat leaves a hot dry surface efficiently, so the aridity term is
## damped far harder than the radiative departures are.
const SENSIBLE_COUPLING := 20.0
## E-folding distance for maritime air pushing inland, m. Calibrated so that a
## coastal site ~150 km inland keeps roughly half the ocean thermal inertia,
## which is what makes maritime climate belts narrow rather than continental.
const MARINE_DECAY_M := 150000.0

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields
var ebm: ClimateEBM

var dist_to_ocean := PackedFloat32Array()
var upwind_slot := PackedByteArray()
## exp(-distance inland / MARINE_DECAY_M): 1 at sea, 0 deep in a continent.
var marine := PackedFloat32Array()
## Zonal-mean maritime influence, so the transport term redistributes heat within
## a band instead of adding to it.
var band_marine := PackedFloat32Array()
## Latent heat flux, W/m^2, and its zonal mean.
var latent := PackedFloat32Array()
var band_latent := PackedFloat32Array()

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	_winds()
	if progress.is_valid():
		progress.call("Climate: moisture transport", 0.1)
	_distance_to_ocean()
	_upwind_slots()
	_maritime_influence()

	if progress.is_valid():
		progress.call("Climate: energy balance", 0.2)
	ebm = ClimateEBM.new(cfg)
	ebm.bind_geography(fields)
	ebm.solve()

	_temperature(progress)
	_precipitation(progress)
	_moisture_feedback(progress)
	_severe_weather()

# ---------------------------------------------------------------- sweep 1 ---
## Local departures from the solved zonal mean. Every term here is a piece of a
## surface energy balance divided by `anomaly_damping` -- how hard the atmosphere
## pulls a single cell back towards the zonal mean it sits in. That damping is
## deliberately stiff: a lone cold cell is mixed away laterally within days, and
## letting it damp as slowly as the seasonal cycle does is exactly what sends the
## ice-albedo feedback into a cell-scale runaway.
func _temperature(progress: Callable) -> void:
	var n := grid.cell_count
	var jitter := NoiseKit.new(cfg.stream_seed("climate") + 1, 4.5, 4)
	var damping: float = maxf(0.5, cfg.anomaly_damping)

	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Climate: temperature", 0.3 + 0.2 * float(c) / float(n))
		var d := grid.cell_dir(c)
		var x := clampf(d.y, -1.0, 1.0)                   # sin(latitude)
		var land_h := maxf(fields.elev[c], 0.0)
		var lapsed := land_h * cfg.lapse_rate

		var t := ebm.temp_at(x)

		# This cell is not the average surface of its band. Where it is darker
		# than the band the balance solved with, it runs warmer; where it is
		# brighter -- land against ocean, ice against either -- it runs cooler.
		var a_local := _surface_albedo(c, t - lapsed)
		t += (ebm.albedo_at(x) - a_local) * ebm.insol_at(x) / damping

		# The heat the circulation carries into a band does not land evenly on
		# it: the maritime edge takes more of it than the interior. Subtracting
		# the band mean keeps this a redistribution, not an extra source.
		var b := ebm.band_of(x)
		t += ebm.transport_at(x) * (marine[c] - band_marine[b]) \
				* cfg.marine_influence / damping

		t -= lapsed
		t += jitter.s(d) * 1.2
		fields.temp_mean[c] = t

# ---------------------------------------------------------------- sweep 2 ---
func _precipitation(progress: Callable) -> void:
	var n := grid.cell_count
	var wet_n := NoiseKit.new(cfg.stream_seed("climate") + 2, 6.5, 4)

	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Climate: precipitation", 0.5 + 0.3 * float(c) / float(n))
		var d := grid.cell_dir(c)
		var h: float = fields.elev[c]
		var land_h := maxf(h, 0.0)
		var t: float = fields.temp_mean[c]
		var lat_deg := rad_to_deg(asin(absf(clampf(d.y, -1.0, 1.0))))

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

		var hum := clampf(fields.precip[c] / 1800.0, 0.0, 1.0) * 0.7 + moisture * 0.3
		if h < 0.0:
			hum = maxf(hum, 0.8)
		fields.humidity[c] = clampf(hum, 0.02, 1.0)

# ---------------------------------------------------------------- sweep 3 ---
## Evaporation closes the loop back onto temperature. A surface that can
## evaporate spends part of its energy budget on latent heat instead of warming,
## and -- because evaporation itself rises with temperature -- it is also much
## harder to push away from its mean. That single quantity is what separates a
## rainforest from a desert at the same latitude: not just rainfall, but a mild
## even year against a scorching swinging one.
func _moisture_feedback(progress: Callable) -> void:
	var n := grid.cell_count
	var spatial: float = maxf(0.5, cfg.anomaly_damping)
	var seasonal: float = maxf(0.5, cfg.seasonal_damping)
	var omega := TAU / SECONDS_PER_YEAR
	var c_ocean: float = maxf(1.0, cfg.ocean_mixed_layer) * SEAWATER_HEAT_CAPACITY
	var c_land: float = maxf(1.0, cfg.land_heat_capacity)

	latent.resize(n)
	band_latent.resize(ClimateEBM.BANDS)
	var band_w := PackedFloat32Array()
	band_w.resize(ClimateEBM.BANDS)
	for b in ClimateEBM.BANDS:
		band_latent[b] = 0.0
		band_w[b] = 0.0

	for c in n:
		var x := clampf(grid.cell_dir(c).y, -1.0, 1.0)
		var absorbed := ebm.insol_at(x) * (1.0 - _surface_albedo(c, fields.temp_mean[c]))
		var supply: float = fields.precip[c] * MM_YR_TO_W
		# Open water evaporates freely; land loses part of its rainfall to runoff.
		# The lake test is `surface above bed` rather than the usual sentinel
		# check, because the provisional pass runs before hydrology has written
		# one and a zero-filled lake_level would read as a lake everywhere.
		if fields.elev[c] >= 0.0 and fields.lake_level[c] <= fields.elev[c]:
			supply *= LAND_EVAP_FRACTION
		# Freezing shuts evaporation down whatever the rainfall says.
		supply *= NoiseKit.smoothstepf(-8.0, 2.0, fields.temp_mean[c])
		var e := minf(supply, MAX_EVAP_FRACTION * maxf(0.0, absorbed))
		latent[c] = e
		var cs: float = grid.cell_size[c]
		var w := cs * cs
		var b := ebm.band_of(x)
		band_latent[b] += e * w
		band_w[b] += w
	for b in ClimateEBM.BANDS:
		band_latent[b] /= maxf(1e-6, band_w[b])

	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Climate: evaporative feedback", 0.8 + 0.2 * float(c) / float(n))
		var x := clampf(grid.cell_dir(c).y, -1.0, 1.0)
		var b := ebm.band_of(x)
		var e: float = latent[c]

		# Annual mean: energy this cell does not spend on evaporation, relative
		# to its band, has to leave as radiation and sensible heat instead.
		fields.temp_mean[c] += (band_latent[b] - e) / (spatial + SENSIBLE_COUPLING)

		# Seasonal range: a damped slab driven by the annual insolation harmonic.
		# Thermal inertia sets the lag and the attenuation; evaporation adds to
		# the damping, which is why wet places have flat years.
		var c_eff: float = c_ocean if fields.elev[c] < 0.0 \
				else lerpf(c_land, c_ocean, marine[c])
		var b_eff := seasonal + e * CLAUSIUS_SLOPE
		var forcing := ebm.insol_amp_at(x) * (1.0 - _surface_albedo(c, fields.temp_mean[c]))
		var wc := omega * c_eff
		fields.temp_range[c] = 2.0 * forcing / sqrt(b_eff * b_eff + wc * wc)

## Planetary albedo of one cell, ice cover included.
func _surface_albedo(c: int, t: float) -> float:
	var base: float = cfg.albedo_ocean if fields.elev[c] < 0.0 else cfg.albedo_land
	return lerpf(base, cfg.albedo_ice, ebm.ice_at(t))

## exp(-distance inland), plus its zonal mean so the transport term stays a
## redistribution within each band.
func _maritime_influence() -> void:
	var n := grid.cell_count
	marine.resize(n)
	band_marine.resize(ClimateEBM.BANDS)
	var band_w := PackedFloat32Array()
	band_w.resize(ClimateEBM.BANDS)
	for b in ClimateEBM.BANDS:
		band_marine[b] = 0.0
		band_w[b] = 0.0
	for c in n:
		var m := exp(-dist_to_ocean[c] / MARINE_DECAY_M)
		marine[c] = m
		var cs: float = grid.cell_size[c]
		var w := cs * cs
		var b := ClimateEBM.band_of(grid.cell_dir(c).y)
		band_marine[b] += m * w
		band_w[b] += w
	for b in ClimateEBM.BANDS:
		band_marine[b] /= maxf(1e-6, band_w[b])

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
