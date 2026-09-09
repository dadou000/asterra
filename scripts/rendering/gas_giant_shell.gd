class_name GasGiantShell
extends Node3D
## Raymarched envelope for one gas giant, Jool-style (after Blackrack's EVE
## Raymarched Volumetrics -- see C:/Users/david/Desktop/clouds).
##
## TWO inward-facing decks, drawn atmo-first:
##   - "atmo" : the whole envelope, core -> cloud tops. A cheap analytic march of
##     the smooth density with a GREEN Rayleigh-ish tint (Jool's colour comes from
##     the atmosphere, not the clouds) -- lit, thickening toward the limb, always
##     closing the view (no seams / no stars). Grey haze deep, never pure black.
##   - "cloud" : a bounded deck near the tops. Grey clouds whose COVERAGE comes
##     from a procedural equirect texture (bands + warped swirls, generated once at
##     bind, seeded) sampled with an animated flow offset -- crisp bands that
##     swirl, for a texture fetch instead of a stack of FBM octaves per step.
##
## One instance per gas-giant BodyRuntime that is the resident (active) body or a
## concurrently-rendered one; fed each frame by main._sync_gas_giant_shells().

const LAYER_SHADER := preload("res://shaders/gas_giant_shell.gdshader")
const GAS_GIANT_MODEL := preload("res://scripts/gen/gas_giant_model.gd")

## The atmo deck fades in from this far ABOVE the tops on a descent approach.
const OUTER_APPROACH_FLOOR_M := 450_000.0
const OUTER_APPROACH_FRAC := 0.06
const APPROACH_M := 300_000.0
const KEEP_M := 600_000.0
const HYSTERESIS_M := 250_000.0

const COVERAGE_W := 512
const COVERAGE_H := 256
## Tileable 3D noise volume for the cloud shape -- baked once, shared by every gas
## giant (it is just noise). R = Worley (F1, inverted: high at cell cores), G =
## Worley 2x, B = value-noise detail. Sampled instead of evaluating FBM per step,
## and the raymarch is per-frame jittered so FSR2's temporal pass resolves it (the
## same recipe EVE's Raymarched Volumetrics uses).
const NOISE3D_SIZE := 48
static var _noise3d: ImageTexture3D = null

var _layers: Array = []   ## each: {mesh, mat, inner, outer, outermost, rot_speed}
var _cloud_top_m: float = 1.0
var _core_m: float = 1.0
var _outer_approach_m: float = OUTER_APPROACH_FLOOR_M
var _rot_period_s: float = 86_400.0
var _below_cloud_tops: bool = false
var _coverage_tex: ImageTexture = null
var _frame: int = 0


func bind(model: GasGiantModel, haze_color: Color, rotation_period_s: float = 86_400.0) -> void:
	_cloud_top_m = model.cloud_top_radius_m
	_core_m = model.core_radius_m
	_outer_approach_m = maxf(_cloud_top_m * OUTER_APPROACH_FRAC, OUTER_APPROACH_FLOOR_M)
	_rot_period_s = maxf(absf(rotation_period_s), 1.0)

	_coverage_tex = ImageTexture.create_from_image(
		GAS_GIANT_MODEL.build_coverage_image(model.coverage_seed(), model.band_hint(),
			COVERAGE_W, COVERAGE_H))
	if _noise3d == null:
		_noise3d = _build_noise3d()

	# Jool's green is the ATMOSPHERE (G-dominant Rayleigh), not the clouds. Keep the
	# per-body hue but bias it green-dominant and desaturate a touch.
	var atmo := Color.from_hsv(haze_color.h, clampf(haze_color.s * 0.9, 0.3, 0.7),
		clampf(haze_color.v * 1.1, 0.4, 0.85))
	var atmo_beta := Vector3(atmo.r * 0.45 + 0.10, atmo.g * 0.9 + 0.20, atmo.b * 0.5 + 0.12)
	# Clouds are near-grey, faintly warm, tinted at runtime by the atmosphere skylight.
	var cloud_col := Color(0.86, 0.86, 0.84)
	var deep := Color.from_hsv(atmo.h, minf(atmo.s + 0.1, 1.0), atmo.v * 0.16)

	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 64
	sphere.rings = 32

	var decks: Array = model.decks()
	for i in decks.size():
		var d: Dictionary = decks[i]
		var outer: float = float(d["outer_m"])
		var inner: float = float(d["inner_m"])
		var kind: String = String(d.get("kind", "atmo"))
		var mi := MeshInstance3D.new()
		mi.name = "Deck%d_%s" % [i, kind]
		mi.mesh = sphere
		mi.scale = Vector3.ONE * outer
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mi.custom_aabb = AABB(Vector3.ONE * -1.5, Vector3.ONE * 3.0)
		mi.extra_cull_margin = 16384.0
		mi.visible = false
		var mat := ShaderMaterial.new()
		mat.shader = LAYER_SHADER
		mat.set_shader_parameter(&"u_core_radius", _core_m)
		mat.set_shader_parameter(&"u_cloud_top_radius", _cloud_top_m)
		mat.set_shader_parameter(&"u_rho_top", model.rho_top())
		mat.set_shader_parameter(&"u_falloff_k", model.falloff_k())
		mat.set_shader_parameter(&"u_layer_inner", inner)
		mat.set_shader_parameter(&"u_layer_outer", outer)
		mat.set_shader_parameter(&"u_steps", int(d.get("steps", 24)))
		mat.set_shader_parameter(&"u_density_mul", float(d.get("density_mul", 1.0)))
		mat.set_shader_parameter(&"u_atmo_beta", atmo_beta)
		mat.set_shader_parameter(&"u_cloud_color", cloud_col)
		mat.set_shader_parameter(&"u_deep_color", deep)
		mat.set_shader_parameter(&"u_is_cloud", 1.0 if kind == "cloud" else 0.0)
		mat.set_shader_parameter(&"u_coverage_tex", _coverage_tex)
		mat.set_shader_parameter(&"u_noise3d", _noise3d)
		mat.set_shader_parameter(&"u_deck_altitude", float(d.get("deck_altitude", 0.9)))
		mat.set_shader_parameter(&"u_deck_width", float(d.get("deck_width", 0.12)))
		mat.set_shader_parameter(&"u_detail", float(d.get("detail", 0.6)))
		mat.set_shader_parameter(&"u_flow", float(d.get("flow", 0.03)))
		mi.material_override = mat
		add_child(mi)
		_layers.append({
			"mesh": mi, "mat": mat, "inner": inner, "outer": outer,
			"outermost": i == 0, "rot_speed": float(d.get("rot_speed_mul", 1.0))})


## `center_render` = the body centre in the render frame; `sun_dir` unit toward the
## star; `observer_render` = the camera in the render frame; `time_s` = the sim
## clock (Frames.system_time_s) so the cloud coverage flows.
func sync(center_render: Vector3, sun_dir: Vector3, observer_render: Vector3,
		time_s: float = 0.0) -> void:
	global_position = center_render
	var cam_r: float = (observer_render - center_render).length()
	_below_cloud_tops = cam_r < _cloud_top_m
	var day_phase: float = TAU * (time_s / _rot_period_s)
	# R2 low-discrepancy sequence -> a per-frame sub-step jitter FSR2's temporal
	# pass integrates (fewer raymarch steps, no static grain).
	_frame += 1
	var jitter: float = fmod(0.5 + 0.7548776662 * float(_frame), 1.0)

	for layer: Dictionary in _layers:
		var mi: MeshInstance3D = layer["mesh"]
		var inner: float = layer["inner"]
		var outer: float = layer["outer"]
		var was_active: bool = mi.visible
		var above: float = _outer_approach_m if bool(layer["outermost"]) else APPROACH_M
		var below: float = KEEP_M
		if was_active:
			above += HYSTERESIS_M
			below += HYSTERESIS_M
		var active: bool = cam_r <= outer + above and cam_r >= inner - below
		mi.visible = active
		if active:
			var mat: ShaderMaterial = layer["mat"]
			mat.set_shader_parameter(&"u_body_center", center_render)
			mat.set_shader_parameter(&"u_sun_dir", sun_dir)
			mat.set_shader_parameter(&"u_cloud_phase", day_phase * float(layer["rot_speed"]))
			mat.set_shader_parameter(&"u_jitter", jitter)


func active_band_count() -> int:
	var n := 0
	for layer: Dictionary in _layers:
		if (layer["mesh"] as MeshInstance3D).visible:
			n += 1
	return n


func band_count() -> int:
	return _layers.size()


## The far-LOD cloud-top sphere is hidden only once the camera is inside the tops.
func suppresses_far_lod() -> bool:
	return _below_cloud_tops


## Tileable 3D noise volume (baked once, shared). R = Worley F1 inverted (dense at
## cell cores -> cloud puffs), G = a second Worley for variety, B = simplex detail.
## Made periodic by a tri-linear blend of the 8 wrapped noise evaluations per
## voxel, so `repeat_enable` sampling at incommensurate octave frequencies tiles
## without a visible seam.
static func _build_noise3d() -> ImageTexture3D:
	var n := NOISE3D_SIZE
	var wa := FastNoiseLite.new()
	wa.noise_type = FastNoiseLite.TYPE_CELLULAR
	wa.cellular_distance_function = FastNoiseLite.DISTANCE_EUCLIDEAN
	wa.cellular_return_type = FastNoiseLite.RETURN_DISTANCE
	wa.frequency = 3.0 / float(n)
	wa.seed = 1337
	var wb := FastNoiseLite.new()
	wb.noise_type = FastNoiseLite.TYPE_CELLULAR
	wb.cellular_distance_function = FastNoiseLite.DISTANCE_EUCLIDEAN
	wb.cellular_return_type = FastNoiseLite.RETURN_DISTANCE
	wb.frequency = 5.0 / float(n)
	wb.seed = 99
	var det := FastNoiseLite.new()
	det.noise_type = FastNoiseLite.TYPE_SIMPLEX
	det.frequency = 6.0 / float(n)
	det.fractal_octaves = 3
	det.seed = 4242

	var slices: Array[Image] = []
	for z in n:
		var img := Image.create(n, n, false, Image.FORMAT_RGBA8)
		for y in n:
			for x in n:
				var r: float = 1.0 - clampf(_periodic3(wa, x, y, z, n) * 0.5 + 0.5, 0.0, 1.0)
				var g: float = 1.0 - clampf(_periodic3(wb, x, y, z, n) * 0.5 + 0.5, 0.0, 1.0)
				var b: float = clampf(_periodic3(det, x, y, z, n) * 0.5 + 0.5, 0.0, 1.0)
				img.set_pixel(x, y, Color(r, g, b, 1.0))
		slices.append(img)

	var tex := ImageTexture3D.new()
	tex.create(Image.FORMAT_RGBA8, n, n, n, false, slices)
	return tex


## FastNoiseLite value at voxel (x,y,z) made periodic over `n` by tri-linearly
## blending the evaluation with its 7 wrapped neighbours (offsets 0 / -n per axis).
static func _periodic3(nz: FastNoiseLite, x: int, y: int, z: int, n: int) -> float:
	var fx := float(x) / float(n)
	var fy := float(y) / float(n)
	var fz := float(z) / float(n)
	var acc := 0.0
	for ox in [0, -n]:
		var wx: float = (1.0 - fx) if ox == 0 else fx
		for oy in [0, -n]:
			var wy: float = (1.0 - fy) if oy == 0 else fy
			for oz in [0, -n]:
				var wz: float = (1.0 - fz) if oz == 0 else fz
				acc += wx * wy * wz * nz.get_noise_3d(
					float(x + ox), float(y + oy), float(z + oz))
	return acc


