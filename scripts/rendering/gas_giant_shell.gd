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

var _layers: Array = []   ## each: {mesh, mat, inner, outer, outermost, rot_speed}
var _cloud_top_m: float = 1.0
var _core_m: float = 1.0
var _outer_approach_m: float = OUTER_APPROACH_FLOOR_M
var _rot_period_s: float = 86_400.0
var _below_cloud_tops: bool = false
var _coverage_tex: ImageTexture = null


func bind(model: GasGiantModel, haze_color: Color, rotation_period_s: float = 86_400.0) -> void:
	_cloud_top_m = model.cloud_top_radius_m
	_core_m = model.core_radius_m
	_outer_approach_m = maxf(_cloud_top_m * OUTER_APPROACH_FRAC, OUTER_APPROACH_FLOOR_M)
	_rot_period_s = maxf(absf(rotation_period_s), 1.0)

	var cov_seed: int = int(model.rho_top() * 1.0e7) ^ int(model.cloud_top_radius_m)
	_coverage_tex = _build_coverage_texture(cov_seed, int(model.decks()[1].get("band_count", 6)))

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


## Procedural equirect cloud-coverage map, generated once per gas giant (seeded):
##   R = large-scale coverage: irregular latitudinal bands + domain-warped swirls
##   G = mid-scale detail for erosion
##   B = storm/spot mask (bright ovals)
## Sampled in the shader by (longitude, latitude) with an animated flow offset.
func _build_coverage_texture(seed_v: int, band_hint: int) -> ImageTexture:
	var img := Image.create(COVERAGE_W, COVERAGE_H, false, Image.FORMAT_RGBA8)
	var rng := RandomNumberGenerator.new()
	rng.seed = seed_v

	var warp := FastNoiseLite.new()
	warp.seed = seed_v ^ 0x11
	warp.noise_type = FastNoiseLite.TYPE_SIMPLEX
	warp.frequency = 0.9
	warp.fractal_octaves = 2
	var swirl := FastNoiseLite.new()
	swirl.seed = seed_v ^ 0x22
	swirl.noise_type = FastNoiseLite.TYPE_SIMPLEX
	swirl.frequency = 1.8
	swirl.fractal_octaves = 4
	var detail := FastNoiseLite.new()
	detail.seed = seed_v ^ 0x33
	detail.noise_type = FastNoiseLite.TYPE_SIMPLEX
	detail.frequency = 5.5
	detail.fractal_octaves = 3

	# Irregular bands: a few latitude sinusoids of different rate / phase.
	var nb: int = maxi(band_hint, 4) + (rng.randi() % 3)
	var amp := PackedFloat32Array()
	var frq := PackedFloat32Array()
	var phs := PackedFloat32Array()
	for _i in nb:
		amp.append(rng.randf_range(0.4, 1.0))
		frq.append(rng.randf_range(2.0, 4.5) * float(2 + (rng.randi() % 4)))
		phs.append(rng.randf() * TAU)
	var band_amp: float = rng.randf_range(0.5, 0.75)
	var swirl_amp: float = rng.randf_range(0.35, 0.55)
	var lo: float = rng.randf_range(0.32, 0.42)
	var hi: float = lo + rng.randf_range(0.24, 0.34)

	for y in COVERAGE_H:
		var lat: float = (float(y) / float(COVERAGE_H - 1) - 0.5) * PI
		var slat: float = sin(lat)
		var clat: float = cos(lat)
		for x in COVERAGE_W:
			var lon: float = float(x) / float(COVERAGE_W) * TAU
			var p := Vector3(clat * cos(lon), slat, clat * sin(lon))
			var wv := Vector3(
				warp.get_noise_3d(p.x * 1.7 + 3.0, p.y * 1.7, p.z * 1.7),
				warp.get_noise_3d(p.x * 1.7, p.y * 1.7 + 5.0, p.z * 1.7),
				warp.get_noise_3d(p.x * 1.7, p.y * 1.7, p.z * 1.7 + 7.0)) * 0.4
			var wl: float = slat + wv.y * 0.55
			var band: float = 0.0
			for i in nb:
				band += amp[i] * sin(wl * frq[i] + phs[i])
			band /= float(nb)
			var s: float = swirl.get_noise_3d(p.x + wv.x, p.y + wv.y, p.z + wv.z)
			var cov: float = 0.5 + band_amp * band + swirl_amp * s
			cov = smoothstep(lo, hi, clampf(cov, 0.0, 1.0))
			var det: float = 0.5 + 0.5 * detail.get_noise_3d(
				p.x + wv.x * 0.5, p.y + wv.y * 0.5, p.z + wv.z * 0.5)
			var storm: float = clampf((swirl.get_noise_3d(
				p.x * 0.55 + 11.0, p.y * 0.55, p.z * 0.55) - 0.62) * 3.0, 0.0, 1.0)
			img.set_pixel(x, y, Color(cov, det, storm, 1.0))

	return ImageTexture.create_from_image(img)
