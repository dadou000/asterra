class_name PassMacro
extends RefCounted
## 1.2 Macro geography.
##
## Continents and islands, mountain belts and plateaus, large valleys and basins,
## coastlines and continental shelves. Built from a tectonic plate model rather
## than raw noise so that later passes (geology, hydrology, settlement) inherit a
## structure that can *explain itself*: mountains sit on convergent margins, oil
## sits in the basins those margins bend downward, and so on.

const CONTINENTAL_BASE := 420.0
const RIDGE_HEIGHT := 2100.0
const TRENCH_DEPTH := -3200.0
const SHELF_BREAK_DEPTH := 550.0

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

var _seeds: Array[Vector3] = []
var _is_continental: Array[bool] = []
var _drift: Array[Vector3] = []
var _plate_bias: Array[float] = []

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	_build_plates()
	_assign_plates(progress)
	_build_relief(progress)
	_hypsometric_remap()

# ------------------------------------------------------------------ plates ---
func _build_plates() -> void:
	var s := cfg.stream_seed("plates")
	for p in cfg.plate_count:
		var seed_dir := HashRNG.unit_sphere(s, p)
		_seeds.append(seed_dir)
		# Roughly a third of the plates carry continental crust.
		_is_continental.append(HashRNG.unit3(s, p, 7) < 0.36)
		# Drift: a tangent vector at the plate seed, deterministic per plate.
		var raw := HashRNG.unit_sphere(s, p + 9973)
		var tangent := (raw - seed_dir * raw.dot(seed_dir))
		if tangent.length() < 1e-4:
			tangent = CubeSphere.tangent_basis(seed_dir)[0]
		_drift.append(tangent.normalized() * (0.4 + HashRNG.unit3(s, p, 31) * 0.6))
		_plate_bias.append((HashRNG.unit3(s, p, 53) - 0.5) * 900.0)

func _assign_plates(progress: Callable) -> void:
	var s := cfg.stream_seed("plate_warp")
	var wx := NoiseKit.new(s + 1, 1.1 / cfg.continent_scale, 4)
	var wy := NoiseKit.new(s + 2, 1.1 / cfg.continent_scale, 4)
	var wz := NoiseKit.new(s + 3, 1.1 / cfg.continent_scale, 4)
	var detail := NoiseKit.new(s + 4, 5.5, 5)
	var n := grid.cell_count
	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Plate tessellation", float(c) / float(n))
		var d := grid.cell_dir(c)
		# Warping the *query* point (not the seeds) gives irregular, non-Voronoi
		# looking margins while remaining a strict nearest-seed partition.
		var q := NoiseKit.warp(d, wx, wy, wz, 0.42)
		q = (q + Vector3(detail.s(d), detail.s(Vector3(d.y, d.z, d.x)), detail.s(Vector3(d.z, d.x, d.y))) * 0.045).normalized()
		var best := -2.0
		var best_i := 0
		var second := -2.0
		var second_i := 0
		for p in _seeds.size():
			var dp := q.dot(_seeds[p])
			if dp > best:
				second = best
				second_i = best_i
				best = dp
				best_i = p
			elif dp > second:
				second = dp
				second_i = p
		fields.plate[c] = best_i
		# Angular gap to the second-nearest seed -> boundary proximity.
		var gap := acos(clampf(second, -1.0, 1.0)) - acos(clampf(best, -1.0, 1.0))
		var margin_width := 0.085
		fields.plate_boundary[c] = 1.0 - clampf(gap / margin_width, 0.0, 1.0)
		# Convergence sign from relative plate motion across the margin.
		var conv := 0.0
		if fields.plate_boundary[c] > 0.0:
			var nrm := (_seeds[second_i] - _seeds[best_i])
			nrm = (nrm - d * nrm.dot(d))
			if nrm.length() > 1e-6:
				nrm = nrm.normalized()
				var rel := _drift[best_i] - _drift[second_i]
				conv = rel.dot(nrm)
		fields.uplift[c] = conv * fields.plate_boundary[c]

# ------------------------------------------------------------------ relief ---
func _build_relief(progress: Callable) -> void:
	var s := cfg.stream_seed("relief")
	var cont := NoiseKit.new(s + 11, 0.95 / cfg.continent_scale, 6, 2.05, 0.52)
	var cont2 := NoiseKit.new(s + 12, 2.6 / cfg.continent_scale, 5)
	var belt := NoiseKit.ridged(s + 13, 7.5, 5)
	var plateau := NoiseKit.new(s + 14, 2.2, 3)
	var basins := NoiseKit.new(s + 15, 1.7, 4)
	var hotspot := NoiseKit.cellular(s + 16, 3.1, FastNoiseLite.RETURN_DISTANCE)
	var rough := NoiseKit.new(s + 17, 12.0, 5)
	var coast := NoiseKit.new(s + 18, 8.5, 6, 2.0, 0.55)
	var chain := NoiseKit.new(s + 19, 1.6, 3)
	var interior := NoiseKit.new(s + 20, 4.5, 5)
	var n := grid.cell_count

	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Macro relief", float(c) / float(n))
		var d := grid.cell_dir(c)
		var p: int = fields.plate[c]
		var continental: bool = _is_continental[p]

		# Crustal base: continental blocks are buoyant, oceanic crust is not.
		# The continentality noise lets continents spill past their plate and lets
		# ocean floor intrude, which is what makes coastlines irregular.
		var cness := cont.s(d) * 0.62 + cont2.s(d) * 0.38
		var block := (0.55 if continental else -0.55) + cness * 0.75
		# Fractal coastal detail: only matters within a narrow band around the
		# land/sea threshold, which is what turns blobs into real coastlines.
		block += coast.s(d) * 0.30
		var h := lerpf(cfg.abyssal_depth, CONTINENTAL_BASE, NoiseKit.smoothstepf(-0.45, 0.45, block))
		h += _plate_bias[p] * (0.6 if continental else 0.25)

		var bnd: float = fields.plate_boundary[c]
		var conv: float = fields.uplift[c]

		# Convergent margins -> orogeny. Continental collision builds high, broad
		# belts; ocean-continent subduction builds a narrower arc plus a trench.
		if conv > 0.0:
			var belt_n := belt.u(d)
			var orogeny := conv * cfg.max_uplift * (0.55 + 0.75 * belt_n)
			if continental:
				h += orogeny
				# Broad internal plateau behind the belt (Tibet analogue).
				var pl := NoiseKit.smoothstepf(0.15, 0.75, plateau.u(d)) * conv * 2600.0
				h += pl * NoiseKit.smoothstepf(0.1, 0.6, bnd)
			else:
				h += orogeny * 0.35
				h += TRENCH_DEPTH * conv * NoiseKit.smoothstepf(0.55, 1.0, bnd)
		elif conv < 0.0:
			# Divergent margins -> mid-ocean ridge or continental rift valley.
			if continental:
				h -= 900.0 * -conv * bnd
			else:
				h += RIDGE_HEIGHT * -conv * bnd

		# Large valleys and basins: long-wavelength subsidence.
		var bas := NoiseKit.smoothstepf(0.35, -0.55, basins.s(d))
		fields.basin[c] = bas
		h -= bas * 1400.0 * (1.0 if continental else 0.3)

		# Hot-spot island chains, so oceans are not empty. Gated by a
		# low-frequency mask so only a few hotspots are active, and roughened so
		# they do not read as perfect cones.
		var hs := 1.0 - clampf(hotspot.u(d) * 2.3, 0.0, 1.0)
		if not continental and hs > 0.0:
			var active := NoiseKit.smoothstepf(0.52, 0.82, chain.u(d))
			h += pow(hs, 1.7) * 5600.0 * active * (0.55 + 0.75 * rough.u(d))

		# Interior relief so continents are not featureless plateaus.
		if continental:
			h += interior.s(d) * 380.0 * clampf(cness + 0.5, 0.0, 1.2)

		# Broad-spectrum roughness so nothing reads as pure fBm.
		h += rough.s(d) * 320.0 * (1.0 + bnd)

		fields.base_elev[c] = h
		fields.elev[c] = h

## Hypsometry.
##
## Tectonics decides *where* the crust is high or low; it is bad at deciding *how*
## high. A raw plate model produces a bimodal histogram with a gap in it, which
## puts entire continents kilometres above the sea. So the elevation field is
## remapped through a target hypsometric curve: a monotone transform, so every
## slope, divide and drainage direction the tectonics produced is preserved
## exactly, while abyssal plains, shelves, coastal plains and orogens end up
## occupying realistic fractions of the surface.
##
## It also makes `ocean_fraction` exact rather than approximate.
const HYPSO_OCEAN := [
	# rank within the ocean fraction -> depth (m)
	[0.00, -5400.0], [0.04, -4900.0], [0.20, -4450.0], [0.50, -3900.0],
	[0.72, -3000.0], [0.85, -1500.0], [0.93, -500.0], [0.975, -150.0], [1.00, 0.0],
]
const HYPSO_LAND := [
	# rank within the land fraction -> elevation (m)
	[0.00, 0.0], [0.12, 90.0], [0.30, 250.0], [0.50, 520.0], [0.68, 900.0],
	[0.82, 1450.0], [0.92, 2350.0], [0.975, 3600.0], [1.00, 6400.0],
]

func _hypsometric_remap() -> void:
	var n := grid.cell_count
	var lo := 1e30
	var hi := -1e30
	for c in n:
		lo = minf(lo, fields.elev[c])
		hi = maxf(hi, fields.elev[c])
	if hi - lo < 1e-3:
		return
	const BINS := 8192
	var hist := PackedInt32Array()
	hist.resize(BINS)
	var scale := float(BINS) / (hi - lo)
	for c in n:
		var b := clampi(int((fields.elev[c] - lo) * scale), 0, BINS - 1)
		hist[b] += 1
	var cum := PackedInt32Array()
	cum.resize(BINS + 1)
	var acc := 0
	for b in BINS:
		cum[b] = acc
		acc += hist[b]
	cum[BINS] = acc

	var ocean_f := clampf(cfg.ocean_fraction, 0.02, 0.98)
	var inv_n := 1.0 / float(n)
	for c in n:
		var v: float = fields.elev[c]
		var fb := (v - lo) * scale
		var b := clampi(int(fb), 0, BINS - 1)
		# Rank of this cell within the global elevation distribution, interpolated
		# inside its histogram bin so the transform stays continuous.
		var r := (float(cum[b]) + (fb - float(b)) * float(hist[b])) * inv_n
		r = clampf(r, 0.0, 1.0)
		var h: float
		if r < ocean_f:
			h = _curve(HYPSO_OCEAN, r / ocean_f)
		else:
			h = _curve(HYPSO_LAND, (r - ocean_f) / maxf(1e-6, 1.0 - ocean_f))
		fields.elev[c] = h
		fields.base_elev[c] = h

static func _curve(table: Array, t: float) -> float:
	var x := clampf(t, 0.0, 1.0)
	for i in range(1, table.size()):
		var a: Array = table[i - 1]
		var b: Array = table[i]
		if x <= float(b[0]):
			var span: float = float(b[0]) - float(a[0])
			var f: float = 0.0 if span <= 0.0 else (x - float(a[0])) / span
			return lerpf(float(a[1]), float(b[1]), f)
	return float((table[table.size() - 1] as Array)[1])
