class_name GasGiantShell
extends Node3D
## Layered raymarched envelope for one gas giant, structured Jool-style: a few
## thin cloud DECKS near the cloud tops over a smooth haze base that runs to the
## core (GasGiantModel.decks()). Only the deck(s) the camera is inside or within a
## fixed margin of are made visible, so the per-frame raymarch cost stays bounded
## no matter how thick the envelope is or how many gas giants are resident. Above
## the cloud tops every deck is dormant and the far-LOD sphere carries the look.
##
## One instance per gas-giant BodyRuntime that is the resident (active) body or a
## concurrently-rendered one; fed each frame by main._sync_gas_giant_shells().

const LAYER_SHADER := preload("res://shaders/gas_giant_shell.gdshader")
const GAS_GIANT_MODEL := preload("res://scripts/gen/gas_giant_model.gd")

## The outermost deck renders from this far ABOVE its top so the atmosphere fades
## in on a descent approach. Scaled to the body in bind() -- 450 km is a floor.
const OUTER_APPROACH_FLOOR_M := 450_000.0
const OUTER_APPROACH_FRAC := 0.06
## Fixed transition margins -- start a deck this far before the camera crosses its
## outer edge, keep it this far after the camera drops below its inner edge. NOT
## scaled by deck thickness (deep decks span tens of thousands of km). Keep >
## approach so a slow crossing never flickers.
const APPROACH_M := 300_000.0
const KEEP_M := 600_000.0
const HYSTERESIS_M := 250_000.0

var _layers: Array = []   ## each: {mesh, mat, inner, outer, outermost, is_cloud, rot_speed}
var _cloud_top_m: float = 1.0
var _core_m: float = 1.0
var _outer_approach_m: float = OUTER_APPROACH_FLOOR_M
var _rot_period_s: float = 86_400.0   ## body sidereal day; drives cloud rotation
## True once the camera has sunk below the cloud tops -- the far-LOD sphere (which
## draws AT the cloud tops) should be suppressed only then, not merely because the
## shell node exists (a gas giant seen from orbit must still show its cloud-top ball).
var _below_cloud_tops: bool = false


func bind(model: GasGiantModel, haze_color: Color, rotation_period_s: float = 86_400.0) -> void:
	_cloud_top_m = model.cloud_top_radius_m
	_core_m = model.core_radius_m
	_outer_approach_m = maxf(_cloud_top_m * OUTER_APPROACH_FRAC, OUTER_APPROACH_FLOOR_M)
	_rot_period_s = maxf(absf(rotation_period_s), 1.0)

	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 64
	sphere.rings = 32

	# Deep-envelope tint: the haze hue, more saturated and very dark, so the march
	# fades from lit haze at the tops to near-black near the core.
	var deep := Color.from_hsv(haze_color.h, minf(haze_color.s + 0.12, 1.0),
		haze_color.v * 0.10)

	var decks: Array = model.decks()
	for i in decks.size():
		var d: Dictionary = decks[i]
		var outer: float = float(d["outer_m"])
		var inner: float = float(d["inner_m"])
		var kind: String = String(d.get("kind", "envelope"))
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
		mat.set_shader_parameter(&"u_steps", int(d.get("steps", 40)))
		mat.set_shader_parameter(&"u_density_mul", float(d.get("density_mul", 1.0)))
		mat.set_shader_parameter(&"u_haze_color", _shift_hue(haze_color, float(d.get("tint_hue_shift", 0.0))))
		mat.set_shader_parameter(&"u_deep_color", deep)
		mat.set_shader_parameter(&"u_is_cloud", 1.0 if kind == "cloud" else 0.0)
		mat.set_shader_parameter(&"u_band_count", float(d.get("band_count", 7)))
		mat.set_shader_parameter(&"u_band_contrast", float(d.get("band_contrast", 0.6)))
		mat.set_shader_parameter(&"u_noise_freq", float(d.get("noise_freq", 46.0)))
		mat.set_shader_parameter(&"u_warp", float(d.get("warp", 0.6)))
		mat.set_shader_parameter(&"u_detail", float(d.get("detail", 0.8)))
		mat.set_shader_parameter(&"u_coverage", float(d.get("coverage", 0.5)))
		mi.material_override = mat
		add_child(mi)
		_layers.append({
			"mesh": mi, "mat": mat, "inner": inner, "outer": outer,
			"outermost": i == 0,
			"rot_speed": float(d.get("rot_speed_mul", 1.0))})


## `center_render` = the body centre in the current render frame; `sun_dir` unit
## toward the star; `observer_render` = the camera position in the render frame;
## `time_s` = the sim clock (Frames.system_time_s) so the cloud decks rotate.
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


## Decks currently raymarching this frame (for the FPS-budget test / HUD).
func active_band_count() -> int:
	var n := 0
	for layer: Dictionary in _layers:
		if (layer["mesh"] as MeshInstance3D).visible:
			n += 1
	return n


func band_count() -> int:
	return _layers.size()


## The far-LOD cloud-top sphere should be hidden only once the camera is inside
## the envelope; above the cloud tops the gas giant is still that sphere.
func suppresses_far_lod() -> bool:
	return _below_cloud_tops


static func _shift_hue(c: Color, dh: float) -> Color:
	if is_zero_approx(dh):
		return c
	return Color.from_hsv(fposmod(c.h + dh, 1.0), c.s, c.v, c.a)
