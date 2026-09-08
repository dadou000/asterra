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
## in on a descent approach (m).
const OUTER_APPROACH_M := 450_000.0
## Fixed transition margins -- start a deck this far before the camera crosses its
## outer edge, keep it this far after the camera drops below its inner edge. NOT
## scaled by deck thickness (deep decks span tens of thousands of km). Keep >
## approach so a slow crossing never flickers.
const APPROACH_M := 300_000.0
const KEEP_M := 600_000.0
const HYSTERESIS_M := 250_000.0

var _layers: Array = []   ## each: {mesh, mat, inner, outer, outermost}
var _cloud_top_m: float = 1.0
var _core_m: float = 1.0


func bind(model: GasGiantModel, haze_color: Color) -> void:
	_cloud_top_m = model.cloud_top_radius_m
	_core_m = model.core_radius_m

	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 48
	sphere.rings = 24

	var decks: Array = model.decks()
	for i in decks.size():
		var d: Dictionary = decks[i]
		var outer: float = float(d["outer_m"])
		var inner: float = float(d["inner_m"])
		var mi := MeshInstance3D.new()
		mi.name = "Deck%d_%s" % [i, d.get("kind", "haze")]
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
		mat.set_shader_parameter(&"u_steps", int(d.get("steps", 14)))
		mat.set_shader_parameter(&"u_density_mul", float(d.get("density_mul", 1.0)))
		mat.set_shader_parameter(&"u_haze_color", _shift_hue(haze_color, float(d.get("tint_hue_shift", 0.0))))
		mi.material_override = mat
		add_child(mi)
		_layers.append({
			"mesh": mi, "mat": mat, "inner": inner, "outer": outer,
			"outermost": i == 0})


## `center_render` = the body centre in the current render frame; `sun_dir` unit
## toward the star; `observer_render` = the camera position in the render frame.
func sync(center_render: Vector3, sun_dir: Vector3, observer_render: Vector3) -> void:
	global_position = center_render
	var cam_r: float = (observer_render - center_render).length()

	for layer: Dictionary in _layers:
		var mi: MeshInstance3D = layer["mesh"]
		var inner: float = layer["inner"]
		var outer: float = layer["outer"]
		var was_active: bool = mi.visible

		var above: float = OUTER_APPROACH_M if bool(layer["outermost"]) else APPROACH_M
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


## Decks currently raymarching this frame (for the FPS-budget test / HUD).
func active_band_count() -> int:
	var n := 0
	for layer: Dictionary in _layers:
		if (layer["mesh"] as MeshInstance3D).visible:
			n += 1
	return n


func band_count() -> int:
	return _layers.size()


static func _shift_hue(c: Color, dh: float) -> Color:
	if is_zero_approx(dh):
		return c
	return Color.from_hsv(fposmod(c.h + dh, 1.0), c.s, c.v, c.a)
