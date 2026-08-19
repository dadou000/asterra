class_name PassHydrology
extends RefCounted
## 1.4 (second half) -- hydrology and drainage.
##
## A deterministic watershed network: streams -> tributaries -> rivers ->
## lakes/ocean, plus the floodplains, wetlands and groundwater tendencies that
## the runtime hydrology solver will later inherit. Nothing here is painted; it
## all follows from the eroded surface and the rainfall field.

const SECONDS_PER_YEAR := 31556736.0
const RUNOFF_COEFFICIENT := 0.38

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields
var router: FlowRouter

func _init(p_fields: PlanetFields, p_router: FlowRouter) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid
	router = p_router

func run(progress: Callable = Callable()) -> void:
	if progress.is_valid():
		progress.call("Hydrology", 0.0)
	var n := grid.cell_count

	# --- discharge -----------------------------------------------------------
	var weight := PackedFloat32Array()
	weight.resize(n)
	var area := PackedFloat32Array()
	area.resize(n)
	for c in n:
		var cs: float = grid.cell_size[c]
		area[c] = cs * cs
		# Runoff volume per year. Cold cells lock water up as snow for part of the
		# year but still deliver it, so only aridity reduces the coefficient.
		var runoff: float = RUNOFF_COEFFICIENT * clampf(fields.precip[c] / 900.0, 0.15, 2.2)
		weight[c] = area[c] * (fields.precip[c] * 0.001) * runoff
	var vol := router.accumulate(weight)
	var acc := router.accumulate(area)

	for c in n:
		fields.flow_accum[c] = acc[c]
		fields.discharge[c] = vol[c] / SECONDS_PER_YEAR
		fields.flow_dir[c] = router.rec_slot[c]

	if progress.is_valid():
		progress.call("Hydrology: watersheds", 0.25)

	# --- watersheds (outlet id propagated upstream) --------------------------
	for i in n:
		var c := router.order[i]
		var r := router.rec[c]
		fields.watershed[c] = c if r == c else fields.watershed[r]

	# --- Strahler stream order ----------------------------------------------
	var max1 := PackedByteArray()
	var cnt := PackedByteArray()
	max1.resize(n)
	cnt.resize(n)
	for i in range(n - 1, -1, -1):
		var c := router.order[i]
		var so := 1
		if cnt[c] > 0:
			so = max1[c] + (1 if cnt[c] >= 2 else 0)
		fields.stream_order[c] = mini(so, 255)
		var r := router.rec[c]
		if r != c:
			if so > max1[r]:
				max1[r] = so
				cnt[r] = 1
			elif so == max1[r]:
				cnt[r] = mini(255, cnt[r] + 1)
			elif cnt[r] == 0:
				cnt[r] = 1

	if progress.is_valid():
		progress.call("Hydrology: lakes", 0.5)

	# --- lakes ---------------------------------------------------------------
	# A lake needs a depression deep enough to survive sediment infill *and* a
	# positive water balance. Closed basins in dry climates become salt flats, so
	# they stay dry ground with a wetland/evaporite signature instead.
	for c in n:
		var h: float = fields.elev[c]
		var f: float = router.filled[c]
		var wet_enough: bool = fields.precip[c] > 180.0
		if h >= 0.0 and f > h + 4.0 and wet_enough:
			fields.lake_level[c] = f
		else:
			fields.lake_level[c] = -1e9

	# --- river geometry ------------------------------------------------------
	# Regime relation: width ~ a * Q^0.5. Anything under a few m^3/s is a stream
	# that lives below the macro grid and is synthesised locally at runtime.
	for c in n:
		var q: float = fields.discharge[c]
		fields.river_width[c] = 0.0 if q < 1.5 else clampf(7.2 * sqrt(q), 2.0, 2600.0)

	if progress.is_valid():
		progress.call("Hydrology: floodplains", 0.7)

	# --- floodplains and wetlands -------------------------------------------
	for c in n:
		var h: float = fields.elev[c]
		if h < 0.0:
			fields.floodplain[c] = 0.0
			fields.wetland[c] = 0.0
			continue
		var base := c * 8
		var relief := 0.0
		var near_river := fields.river_width[c]
		var near_level := h
		for k in 8:
			var nb := grid.nbr[base + k]
			relief = maxf(relief, absf(h - fields.elev[nb]))
			if fields.river_width[nb] > near_river:
				near_river = fields.river_width[nb]
				near_level = fields.elev[nb]
		var slope := relief / grid.cell_size[c]
		var flat := clampf(1.0 - slope * 34.0, 0.0, 1.0)
		var above := h - near_level
		var fp := flat * NoiseKit.smoothstepf(0.0, 45.0, near_river) * clampf(1.0 - above / 55.0, 0.0, 1.0)
		fields.floodplain[c] = clampf(fp, 0.0, 1.0)
		# Wetlands: flat, wet, poorly drained ground, or a lake margin.
		var wet := flat * clampf(fields.precip[c] / 1400.0, 0.0, 1.4) * (1.0 - fields.aquifer[c] * 0.55)
		if fields.lake_level[c] > -1e8:
			wet = maxf(wet, 0.85)
		fields.wetland[c] = clampf(wet * 0.9 + fp * 0.35, 0.0, 1.0)

	if progress.is_valid():
		progress.call("Hydrology", 1.0)

## Diagnostic used by the test suite: fraction of land drainage that terminates in
## the ocean rather than in an interior sink.
func exorheic_fraction() -> float:
	var land := 0
	var to_sea := 0
	for c in grid.cell_count:
		if fields.elev[c] <= 0.0:
			continue
		land += 1
		if fields.elev[fields.watershed[c]] <= 0.0:
			to_sea += 1
	return 0.0 if land == 0 else float(to_sea) / float(land)
