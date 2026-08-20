class_name TerrainDetail
extends RefCounted
## Sub-grid terrain synthesis.
##
## The macro grid resolves ~8 km cells. Everything between those cells is
## generated here, deterministically from the same world seed, so a hillside is
## identical on every machine and every visit without storing a single byte.
## Amplitude is driven by the macro relief and rock hardness, so plains stay
## plains and orogens get rugged -- fBm is never allowed to overwrite the
## tectonic story.
##
## One instance per thread: FastNoiseLite is cheap to construct and this keeps
## chunk meshing lock-free.

var _fbm: FastNoiseLite
var _ridge: FastNoiseLite
var _warp: FastNoiseLite
var _micro: FastNoiseLite
var _scale: float

func _init(cfg: GenConfig) -> void:
	var s := cfg.stream_seed("detail")
	_scale = cfg.planet_radius
	_fbm = _mk(s + 1, cfg.detail_base_frequency, cfg.detail_octaves, FastNoiseLite.FRACTAL_FBM)
	_ridge = _mk(s + 2, cfg.detail_base_frequency * 1.7, maxi(3, cfg.detail_octaves - 1), FastNoiseLite.FRACTAL_RIDGED)
	_warp = _mk(s + 3, cfg.detail_base_frequency * 0.55, 3, FastNoiseLite.FRACTAL_FBM)
	_micro = _mk(s + 4, cfg.detail_base_frequency * 26.0, 3, FastNoiseLite.FRACTAL_FBM)

func _mk(sd: int, freq: float, oct: int, fractal: int) -> FastNoiseLite:
	var n := FastNoiseLite.new()
	n.seed = sd
	n.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	n.frequency = freq
	n.fractal_type = fractal as FastNoiseLite.FractalType
	n.fractal_octaves = oct
	# gain * lacunarity = 0.9: each successive octave contributes slightly less
	# gradient than the last, so the surface stays walkable instead of becoming
	# fractally vertical.
	n.fractal_gain = 0.45
	n.fractal_lacunarity = 2.0
	return n

## `relief` = macro elevation range in metres, `flatten` = 0..1 suppression
## (floodplains, lake shores, wetlands), `hardness` = 0.3..1.6 rock resistance.
func height(d: Vector3, relief: float, flatten: float, hardness: float) -> float:
	var x := d.x * _scale
	var y := d.y * _scale
	var z := d.z * _scale
	var w := _warp.get_noise_3d(x, y, z) * 260.0
	var f := _fbm.get_noise_3d(x + w, y - w * 0.6, z + w * 0.3)
	var r := _ridge.get_noise_3d(x, y, z) * 0.5 + 0.5
	# Rugged terrain gets ridged character; gentle terrain stays smooth fBm.
	var rugged := clampf(relief / 900.0, 0.0, 1.0)
	var shape := lerpf(f, (r * 2.0 - 1.0) * 0.85 + f * 0.25, rugged)
	# Amplitude is capped against the detail wavelength, not just against relief:
	# a 2.8 km feature carrying 600 m of relief would be a cliff everywhere.
	var amp := clampf(relief * 0.11, 5.0, 205.0) * clampf(hardness, 0.35, 1.6)
	var h := shape * amp
	h += _micro.get_noise_3d(x, y, z) * clampf(amp * 0.03, 0.25, 2.2)
	return h * (1.0 - clampf(flatten, 0.0, 1.0) * 0.88)
