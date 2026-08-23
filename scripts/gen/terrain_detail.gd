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
var _mountain_range: FastNoiseLite
var _mountain_crag: FastNoiseLite
var _dune: FastNoiseLite
var _badland: FastNoiseLite
var _channel: FastNoiseLite
var _ground: FastNoiseLite
var _scale: float
var _amplitude_scale: float
var _base_freq: float
## Per-band amplitude weights for the current sampling interval. 1 until
## `set_sample_spacing` says otherwise.
var _w_shape := 1.0
var _w_micro := 1.0
var _w_range := 1.0
var _w_crag := 1.0
var _w_dune := 1.0
var _w_badland := 1.0
var _w_channel := 1.0
var _w_ground := 1.0
var _spacing := 0.0
var _base_octaves := 6

func _init(cfg: GenConfig) -> void:
	var s := cfg.stream_seed("detail")
	_scale = cfg.planet_radius
	_base_freq = cfg.detail_base_frequency
	_amplitude_scale = maxf(0.05, cfg.detail_amplitude / 260.0)
	_base_octaves = cfg.detail_octaves
	_fbm = _mk(s + 1, cfg.detail_base_frequency, cfg.detail_octaves, FastNoiseLite.FRACTAL_FBM)
	_ridge = _mk(s + 2, cfg.detail_base_frequency * 1.7, maxi(3, cfg.detail_octaves - 1), FastNoiseLite.FRACTAL_RIDGED)
	_warp = _mk(s + 3, cfg.detail_base_frequency * 0.55, 3, FastNoiseLite.FRACTAL_FBM)
	_micro = _mk(s + 4, cfg.detail_base_frequency * 26.0, 3, FastNoiseLite.FRACTAL_FBM)
	# Separate spectra represent different geomorphic processes. The mountain
	# octave begins above the macro-grid Nyquist scale and fills the missing
	# 8-20 km silhouettes; dunes/badlands live at successively shorter scales.
	_mountain_range = _mk(s + 5, cfg.detail_base_frequency * 0.18, 5, FastNoiseLite.FRACTAL_RIDGED)
	# A dedicated 450-800 m band prevents close mountain faces from becoming
	# smooth ramps between the kilometre ridges and the ~100 m micro octave.
	_mountain_crag = _mk(s + 6, cfg.detail_base_frequency * 4.20, 3, FastNoiseLite.FRACTAL_RIDGED)
	_dune = _mk(s + 7, cfg.detail_base_frequency * 3.20, 5, FastNoiseLite.FRACTAL_RIDGED)
	_badland = _mk(s + 8, cfg.detail_base_frequency * 3.40, 4, FastNoiseLite.FRACTAL_RIDGED)
	# Drainage. A ridged fractal takes its maximum along connected filaments that
	# branch and rejoin, which is the one thing ordinary fBm cannot produce and
	# the one thing a landscape is actually organised by. Six octaves from ~3 km
	# down to ~100 m give trunk valleys with tributaries inside them.
	_channel = _mk(s + 9, cfg.detail_base_frequency * 0.85, 6, FastNoiseLite.FRACTAL_RIDGED)
	# Everything above stops at the ~110 m micro octave, so from a hundred metres
	# down the surface was a mathematically smooth ramp -- which is why standing on
	# it felt like standing on a plane. Four octaves from ~32 m to ~4 m fill the
	# band a person actually walks over. The floor is Nyquist on the finest mesh:
	# a chunk at maximum depth carries vertices 0.75 m apart.
	_ground = _mk(s + 10, cfg.detail_base_frequency * 90.0, 4, FastNoiseLite.FRACTAL_FBM)

## Band-limit synthesis to a sampling interval, in metres.
##
## `height()` is point-sampled on the chunk mesh's vertex grid, and an octave
## shorter than that grid is not detail -- it is aliasing. It arrives as an
## uncorrelated per-vertex offset, which is what makes a distant mountainside
## read as crumpled foil and stamps the triangulation across it as a regular
## herringbone of triangular facets. It is also why an LOD change pops: two
## levels alias the same octave into different noise, so the surface jumps rather
## than gaining detail.
##
## Nothing below the sampling Nyquist can be represented, so it is removed rather
## than sampled: octaves finer than the grid are dropped from each fractal, and a
## band whose own base wavelength is already too short fades out entirely. The
## band that goes missing is not lost -- `relief_detail_normal` in the terrain
## shader puts exactly that range back as shading, read from the whole-planet
## elevation texture, which is what it is for.
##
## Call once per chunk build. Zero restores full detail, which is what the
## camera, collision and orbit-texture queries want.
func set_sample_spacing(metres: float) -> void:
	if is_equal_approx(metres, _spacing):
		return
	_spacing = metres
	if metres <= 0.0:
		_w_shape = 1.0
		_w_micro = 1.0
		_w_range = 1.0
		_w_crag = 1.0
		_w_dune = 1.0
		_w_badland = 1.0
		_w_channel = 1.0
		_w_ground = 1.0
		_restore_octaves()
		return
	_w_shape = _band_weight(1.0)
	_w_micro = _band_weight(26.0)
	_w_range = _band_weight(0.18)
	_w_crag = _band_weight(4.20)
	_w_dune = _band_weight(3.20)
	_w_badland = _band_weight(3.40)
	_w_channel = _band_weight(0.85)
	_w_ground = _band_weight(90.0)
	_trim(_fbm, 1.0, _base_octaves)
	_trim(_ridge, 1.7, maxi(3, _base_octaves - 1))
	_trim(_warp, 0.55, 3)
	_trim(_micro, 26.0, 3)
	_trim(_mountain_range, 0.18, 5)
	_trim(_mountain_crag, 4.20, 3)
	_trim(_dune, 3.20, 5)
	_trim(_badland, 3.40, 4)
	_trim(_channel, 0.85, 6)
	_trim(_ground, 90.0, 4)

## Weight for a band whose base frequency is `mult` times the detail frequency.
##
## A wavelength needs several samples across it, not two. Nyquist is the wrong
## bar for a linearly interpolated mesh: at two samples per wavelength a sine
## renders as a zigzag, and at three the triangulation is still plainly visible
## in the shading, because neighbouring vertex normals disagree by more than a
## triangle can interpolate smoothly. SAMPLES_PER_WAVELENGTH is what the terrain
## shader's relief correction is told to assume, so the two have to move
## together.
const SAMPLES_PER_WAVELENGTH := 4.0

## Width of the roll-off, as a multiple of SAMPLES_PER_WAVELENGTH.
##
## This is not just a smoothing preference: it sets how much a chunk's surface
## can differ from its own parent's, which is exactly the geomorph error. Two
## LOD levels differ by a factor of two in spacing, so a roll-off only a factor
## of two wide hands a whole band to the child that the parent does not have and
## the morph cannot land -- it arrives as a pop at every LOD transition. Spread
## over about two and a half octaves instead, each level adds only a fraction of
## a band and the child stays close enough to its parent to morph onto it.
const BAND_ROLLOFF := 2.8

func _band_weight(mult: float) -> float:
	var wavelength := 1.0 / maxf(_base_freq * mult, 1e-9)
	return smoothstep(_spacing * (SAMPLES_PER_WAVELENGTH * 0.55),
		_spacing * (SAMPLES_PER_WAVELENGTH * 0.55 * BAND_ROLLOFF), wavelength)

## Drop the octaves of one fractal that fall below the sampling grid.
func _trim(n: FastNoiseLite, mult: float, full_octaves: int) -> void:
	var wavelength := 1.0 / maxf(_base_freq * mult, 1e-9)
	var keep := 1
	# Trim well below where `_band_weight` has already faded the band out, so the
	# hard step of dropping an octave lands where its amplitude is negligible
	# rather than adding a second discontinuity between levels.
	while keep < full_octaves 			and wavelength / pow(2.0, float(keep)) > _spacing * 1.6:
		keep += 1
	n.fractal_octaves = keep

func _restore_octaves() -> void:
	_fbm.fractal_octaves = _base_octaves
	_ridge.fractal_octaves = maxi(3, _base_octaves - 1)
	_warp.fractal_octaves = 3
	_micro.fractal_octaves = 3
	_mountain_range.fractal_octaves = 5
	_mountain_crag.fractal_octaves = 3
	_dune.fractal_octaves = 5
	_badland.fractal_octaves = 4
	_channel.fractal_octaves = 6
	_ground.fractal_octaves = 4

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
## `surface` packs continuous vegetation, moisture, aeolian-sand and frost
## weights.  Reusing the already-sampled micro octave keeps biome geometry free
## of extra noise calls in the threaded chunk builder.
func height(d: Vector3, relief: float, flatten: float, hardness: float,
		surface: Color = Color(0.0, 0.0, 0.0, 0.0),
		geomorph: Color = Color(0.0, 0.0, 0.0, 0.0),
		drainage: Color = Color(0.0, 0.0, 0.0, 0.0)) -> float:
	var x := d.x * _scale
	var y := d.y * _scale
	var z := d.z * _scale
	var w := _warp.get_noise_3d(x, y, z) * 260.0
	var f := _fbm.get_noise_3d(x + w, y - w * 0.6, z + w * 0.3)
	var r := _ridge.get_noise_3d(x, y, z) * 0.5 + 0.5
	# Rugged terrain gets ridged character; soil-mantled terrain stays smooth.
	var rugged := clampf(relief / 700.0, 0.0, 1.0)
	var ridge_base := r * 2.0 - 1.0
	var ridge_shape := signf(ridge_base) * pow(absf(ridge_base), 0.72)
	var shape := lerpf(f, ridge_shape * 0.92 + f * 0.22, rugged)
	# Amplitude is capped against the detail wavelength, not just against relief:
	# a 2.8 km feature carrying 600 m of relief would be a cliff everywhere.
	var amp := clampf(relief * 0.18, 5.0, 340.0) \
		* clampf(hardness, 0.35, 1.6) * _amplitude_scale
	var h := shape * amp * _w_shape
	var micro := _micro.get_noise_3d(x, y, z)
	var micro_amp := clampf(amp * 0.038, 0.25, 6.5 * _amplitude_scale) * _w_micro
	h += micro * micro_amp

	# Orogenic terrain: broad range-scale ridges provide the silhouette missing
	# between 8 km macro cells; shorter ridges fracture those masses into peaks,
	# aretes, gullies and talus instead of inflated smooth hills.
	# Expensive process-specific spectra fade in from zero before their first
	# sample. This avoids spending mountain noise on ecotone cells where the
	# common relief spectrum already provides the few metres of required shape.
	var mountain_weight := smoothstep(0.16, 0.48, geomorph.r)
	if mountain_weight > 0.001:
		var range_n := _mountain_range.get_noise_3d(x + w * 0.34, y - w * 0.18, z + w * 0.27)
		var range_shape := signf(range_n) * pow(absf(range_n), 0.62)
		var crag_n := _mountain_crag.get_noise_3d(x - w * 0.42, y + w * 0.21, z - w * 0.31)
		var crag_shape := signf(crag_n) * pow(absf(crag_n), 0.54)
		var mountain_amp := clampf(relief * 0.48, 55.0, 880.0) \
			* clampf(hardness, 0.55, 1.65) * _amplitude_scale
		h += (range_shape * 0.78 * _w_range + ridge_shape * 0.32 * _w_shape) 			* mountain_amp * mountain_weight
		# Crags are proportional to the range, but capped below their wavelength so
		# the mesh forms steep faces and gullies without ubiquitous needle spikes.
		var crag_amp := clampf(mountain_amp * 0.115, 12.0, 82.0 * _amplitude_scale) * _w_crag
		h += crag_shape * crag_amp * mountain_weight
		h += signf(micro) * pow(absf(micro), 0.58) \
			* minf(24.0 * _amplitude_scale, mountain_amp * 0.055) * mountain_weight

	# Arid terrain separates sandy ergs from exposed badlands. Sand produces
	# rounded multi-kilometre dune trains; low-sand deserts expose terraced mesas,
	# yardangs and sharp drainage ribs.
	var arid_weight := smoothstep(0.10, 0.40, geomorph.g)
	if arid_weight > 0.001:
		var sandiness := clampf(surface.b * 1.55, 0.0, 1.0)
		var dune_amp := lerpf(14.0, 92.0, sandiness) * _amplitude_scale
		var badland_amp := clampf(relief * 0.19 + 16.0, 18.0, 190.0) * _amplitude_scale
		var arid_shape := 0.0
		if sandiness > 0.08:
			var dune_n := _dune.get_noise_3d(x + w, y - w * 0.35, z + w * 0.15)
			var dune_shape := signf(dune_n) * pow(absf(dune_n), 0.54)
			arid_shape += dune_shape * dune_amp * _w_dune * smoothstep(0.08, 0.92, sandiness)
		if sandiness < 0.92:
			var bad := _badland.get_noise_3d(x - w * 0.20, y + w * 0.45, z - w * 0.33)
			var bad_t := clampf(bad * 0.5 + 0.5, 0.0, 0.9999)
			var terraces: float = ((floor(bad_t * 9.0) + 0.5) / 9.0) * 2.0 - 1.0
			arid_shape += terraces * badland_amp * _w_badland 				* (1.0 - smoothstep(0.08, 0.92, sandiness))
		h += arid_shape * arid_weight
		h += micro * lerpf(5.5, 2.0, sandiness) * arid_weight * _amplitude_scale * _w_micro

	# Ice and permafrost are broad and flowing rather than noisy rock. A smooth
	# long wave makes glacier lobes; narrow negative micro-ridges suggest crevasse
	# and patterned-ground relief without turning the ice sheet into mountains.
	var glacial_weight := smoothstep(0.16, 0.46, geomorph.b)
	if glacial_weight > 0.001:
		# The low-frequency warp field already has the right flowing spectrum;
		# reusing it avoids a fifth noise evaluation across every polar vertex.
		var ice := clampf(w / 260.0, -1.0, 1.0)
		var ice_amp := clampf(relief * 0.12 + 10.0, 10.0, 175.0) * _amplitude_scale
		h = lerpf(h, h * 0.78 + ice * ice_amp, glacial_weight * 0.72)
		var crevasse := maxf(absf(micro) - 0.34, 0.0)
		h -= crevasse * crevasse * 22.0 * glacial_weight * _amplitude_scale
		# Thick ice buries short-wavelength bedrock roughness. Preserve broad
		# glacial flow and crevasses while damping the cauliflower-like crag field.
		h = lerpf(h, h - micro * micro_amp * 0.82, glacial_weight * 0.68)

	# Ground-scale undulation. Deliberately not suppressed by `flatten` as hard as
	# the kilometre bands are: a floodplain is flat at a kilometre and still has
	# half a metre of hummock and swale across a stride.
	var ground_amp := clampf(amp * 0.042 + 0.85, 0.85, 5.0) * _amplitude_scale
	ground_amp *= 1.0 - clampf(maxf(flatten, geomorph.a * 0.82), 0.0, 1.0) * 0.55
	h += _ground.get_noise_3d(x + w * 0.15, y - w * 0.11, z + w * 0.19) * ground_amp * _w_ground

	# Erosional dissection. Everything above is additive noise, which is why
	# unmodified it reads as heaped lumps: real terrain is not piled up, it is what
	# is left after water has removed the rest. Inverting the drainage fractal and
	# sharpening it cuts narrow, connected, branching valleys into the mass, and
	# that single subtraction is what turns a field of blobs into ridges, spurs and
	# catchments.
	var incise_amp := clampf(relief * 0.26, 4.0, 300.0) * _amplitude_scale
	# Ice buries drainage, floodplains are the deposit end of it, and neither
	# should be carved.
	incise_amp *= (1.0 - glacial_weight * 0.80) * _w_channel
	if incise_amp > 0.5:
		# Stretch the correlation frame along the baked downslope direction. Noise
		# still supplies sub-grid tributaries, but it no longer crosses macro divides
		# or runs orthogonally through the drainage network.
		var basis := CubeSphere.tangent_basis(d)
		var flow: Vector3 = Vector3(basis[0]) * drainage.r + Vector3(basis[1]) * drainage.g
		var sample_pos := Vector3(x + w * 0.62, y - w * 0.44, z + w * 0.53)
		if flow.length_squared() > 0.25:
			flow = flow.normalized()
			var along: Vector3 = flow * sample_pos.dot(flow)
			var across: Vector3 = sample_pos - along
			sample_pos = along * 0.38 + across * 1.55
		var chan_n := _channel.get_noise_3d(sample_pos.x, sample_pos.y, sample_pos.z)
		var chan := clampf(chan_n * 0.5 + 0.5, 0.0, 1.0)
		# The exponent is the valley's width. Low values flood the whole surface
		# with shallow dimples; this keeps the network narrow and the interfluves
		# broad, which is the ratio real dissected uplands have.
		chan = pow(chan, lerpf(3.6, 2.65, drainage.b))
		var hydrologic_strength := lerpf(0.42, 1.35, drainage.b)
		h -= chan * incise_amp * hydrologic_strength
		# Valley floors carry their own deposit, so the very bottom is flatter than
		# the walls rather than a knife edge.
		h += chan * chan * incise_amp * lerpf(0.16, 0.34, drainage.a)

	# Floodplains and wetlands correctly suppress kilometre-scale relief, but
	# their close surface is not mathematically flat: hummocks, levees and shallow
	# abandoned channels remain below the metre-to-few-metre scale.
	var flatten_amount := clampf(maxf(flatten, geomorph.a * 0.82), 0.0, 1.0)
	h *= 1.0 - flatten_amount * 0.91
	if geomorph.a > 0.08:
		var hummock := micro * 0.55 + (absf(micro) - 0.28) * 0.45
		h += hummock * lerpf(0.35, 2.4, surface.g) * geomorph.a * _amplitude_scale
	return h
