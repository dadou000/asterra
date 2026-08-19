class_name PassErosion
extends RefCounted
## 1.4 (first half) -- landscape evolution.
##
## An implicit stream-power incision solver (Braun & Willett 2013) plus hillslope
## relaxation and sediment deposition, run on the depression-filled flow network.
## The implicit form is unconditionally stable, so a believable drainage-carved
## landscape falls out of ~20 large timesteps instead of thousands of small ones,
## which is what makes an offline pass affordable inside a game bake.
##
## Rock erodibility comes from the geology pass, and rainfall from a provisional
## climate pass -- wet windward slopes really do incise faster than dry lee sides.

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
	var n := grid.cell_count
	var elev := fields.elev
	var area := PackedFloat32Array()
	area.resize(n)
	var erodibility := PackedFloat32Array()
	erodibility.resize(n)
	var mean_precip := 0.0
	for c in n:
		var cs: float = grid.cell_size[c]
		area[c] = cs * cs
		erodibility[c] = PlanetFields.ROCK_ERODIBILITY[fields.rock[c]]
		mean_precip += fields.precip[c]
	mean_precip = maxf(1.0, mean_precip / float(n))

	var sediment_flux := PackedFloat32Array()
	sediment_flux.resize(n)
	var weight := PackedFloat32Array()
	weight.resize(n)

	for it in cfg.erosion_iterations:
		if progress.is_valid():
			progress.call("Erosion %d/%d" % [it + 1, cfg.erosion_iterations],
				float(it) / float(cfg.erosion_iterations))

		# --- tectonic uplift keeps orogens alive against incision -----------
		for c in n:
			var up: float = fields.uplift[c]
			if up > 0.0 and elev[c] > -200.0:
				elev[c] += up * cfg.uplift_per_step

		# --- route ----------------------------------------------------------
		router.route_and_order(elev, 0.0)

		# Closed depressions silt up as the landscape evolves. Doing this inside
		# the loop matters: the stream-power solver cannot incise a cell that has
		# no real downhill receiver, so without infill every local minimum in the
		# initial noise survives to the end and becomes a lake.
		_infill_basins(elev, cfg.basin_infill_depth, cfg.basin_infill_excess)

		# Effective drainage: area weighted by rainfall, so discharge (not bare
		# area) drives incision.
		for c in n:
			weight[c] = area[c] * (fields.precip[c] / mean_precip)
		var accum := router.accumulate(weight)

		# --- implicit stream-power incision, outlets first -------------------
		var eroded := PackedFloat32Array()
		eroded.resize(n)
		for i in n:
			var c := router.order[i]
			var r := router.rec[c]
			if r == c:
				continue
			var h := elev[c]
			if h <= 0.0:
				continue
			var base := maxf(elev[r], 0.0)
			if h <= base:
				continue
			var k := cfg.stream_power_k * erodibility[c]
			var f: float = k * cfg.erosion_dt * pow(maxf(accum[c], 1.0), cfg.stream_power_m) / router.rec_dist[c]
			# Deposition relaxes incision where the channel is already loaded.
			var slope := (h - base) / router.rec_dist[c]
			var g := cfg.sediment_deposition / (1.0 + slope * 240.0)
			var dep: float = g * sediment_flux[c] / maxf(accum[c], 1.0)
			var nh := (h + f * base + dep) / (1.0 + f)
			nh = maxf(nh, base)
			eroded[c] = h - nh
			elev[c] = nh
			if eroded[c] < 0.0:
				fields.sediment[c] += -eroded[c]

		# --- sediment routing for the next iteration -------------------------
		for c in n:
			weight[c] = maxf(0.0, eroded[c]) * area[c]
		sediment_flux = router.accumulate(weight)

		# --- hillslope relaxation (diffusive transport, sub-grid) ------------
		_relax(elev)

	# Final routing so downstream passes see a network matching the final surface.
	router.route_and_order(elev, 0.0)
	_settle_sediment()

## Deposit sediment into depressions. Pits shallower than `depth` silt up
## completely; deeper basins lose only `excess` of the remainder, so a basin that
## is actively being rimmed by tectonics survives as a real closed lake.
func _infill_basins(elev: PackedFloat32Array, depth: float, excess: float) -> void:
	for c in grid.cell_count:
		if elev[c] <= 0.0:
			continue
		var gap: float = router.filled[c] - elev[c]
		if gap <= 0.0:
			continue
		var fill: float = minf(gap, depth) + maxf(0.0, gap - depth) * excess
		elev[c] += fill
		fields.sediment[c] += fill

func _relax(elev: PackedFloat32Array) -> void:
	var n := grid.cell_count
	var s := clampf(cfg.hillslope_relaxation, 0.0, 0.24)
	if s <= 0.0:
		return
	var src := elev.duplicate()
	for c in n:
		if src[c] <= 0.0:
			continue
		var base := c * 8
		var sum := 0.0
		for k in 8:
			sum += src[grid.nbr[base + k]]
		elev[c] = src[c] + s * (sum * 0.125 - src[c])

## Sediment thickness: deposition accumulates in basins, floodplains and shelves,
## and is stripped from steep bedrock slopes.
func _settle_sediment() -> void:
	var n := grid.cell_count
	for c in n:
		var h: float = fields.elev[c]
		var base := c * 8
		var relief := 0.0
		for k in 8:
			relief = maxf(relief, absf(h - fields.elev[grid.nbr[base + k]]))
		var slope := relief / grid.cell_size[c]
		var deposit: float = fields.sediment[c]
		if h < 0.0:
			deposit += 14.0 + fields.basin[c] * 40.0
		deposit *= clampf(1.0 - slope * 22.0, 0.02, 1.0)
		deposit += fields.basin[c] * 9.0
		fields.sediment[c] = clampf(deposit, 0.0, 160.0)
