extends Node
## GG2/GG3 (headless): the GasGiantShell builds a cheap green "atmo" deck + a
## grey texture-coverage "cloud" deck from a GasGiantModel, raymarches them only
## near/inside the cloud tops, generates a procedural equirect coverage texture,
## and the generator reconstructs the model for any gas-giant body. Rendering is
## checked by the windowed gg2_shell_probe / gg_look_4k probes.
##   godot --headless --path . res://tests/validate_gas_giant_shell.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GAS_GIANT_MODEL := preload("res://scripts/gen/gas_giant_model.gd")
const GAS_GIANT_SHELL := preload("res://scripts/rendering/gas_giant_shell.gd")

var _failed := false
var _frames := 0


func _process(_dt: float) -> void:
	_frames += 1
	if _frames > 1800:
		push_error("GAS_GIANT_SHELL_FAILED: timed out")
		get_tree().quit(1)


func _ready() -> void:
	await get_tree().process_frame

	var system: CelestialSystemDefinition = GENERATOR.generate(8571)
	var colossus: Resource = system.find_body("colossus")
	var rime: Resource = system.find_body("rime")
	_assert(GENERATOR.gas_giant_model_for(rime) == null, "no gas-giant model for a non-gas body")
	var model: GasGiantModel = GENERATOR.gas_giant_model_for(colossus)
	_assert(model != null, "the generator reconstructs colossus's GasGiantModel")
	_assert(is_equal_approx(model.core_radius_m, float(colossus.get(&"core_radius_m")))
			and is_equal_approx(model.cloud_top_radius_m, float(colossus.get(&"radius_m"))),
		"the reconstructed model matches the radii stamped on the body")

	var shell: GasGiantShell = GAS_GIANT_SHELL.new()
	add_child(shell)
	shell.bind(model, Color(0.5, 0.6, 0.35))

	var decks: Array = model.decks()
	_assert(decks.size() == 2, "the envelope is an atmo deck + a cloud deck (%d)" % decks.size())
	_assert(String(decks[0].get("kind")) == "atmo" and String(decks[1].get("kind")) == "cloud",
		"deck 0 is the atmosphere deck, deck 1 the cloud deck")
	_assert(shell.band_count() == 2, "the shell builds one mesh per deck")

	var atmo: MeshInstance3D = shell.get_child(0)
	var cloud: MeshInstance3D = shell.get_child(1)
	var atmo_mat: ShaderMaterial = atmo.material_override
	var cloud_mat: ShaderMaterial = cloud.material_override
	for m: ShaderMaterial in [atmo_mat, cloud_mat]:
		_assert(m != null and m.shader != null
				and m.shader.resource_path.ends_with("gas_giant_shell.gdshader"),
			"the deck runs the raymarch shader")
	_assert(not atmo.visible and not cloud.visible, "both decks start hidden (dormant above the tops)")

	# The atmo deck spans the whole gas body -> a ray inside the envelope is never
	# left transparent (no black seams / no stars).
	_assert(is_equal_approx(float(atmo_mat.get_shader_parameter(&"u_layer_inner")), model.core_radius_m),
		"the atmo deck floor is the solid core")
	_assert(float(atmo_mat.get_shader_parameter(&"u_layer_outer")) >= model.cloud_top_radius_m,
		"the atmo deck ceiling is at/above the cloud tops")
	_assert(is_zero_approx(float(atmo_mat.get_shader_parameter(&"u_is_cloud"))),
		"the atmo deck runs the cheap analytic green path (u_is_cloud 0)")
	var beta: Vector3 = atmo_mat.get_shader_parameter(&"u_atmo_beta")
	_assert(beta.is_finite() and beta.y > beta.x and beta.y > beta.z,
		"the atmosphere tint is green-dominant (Jool-style Rayleigh)")

	# The cloud deck is a bounded slice in the upper envelope, texture-driven.
	var slab_inner: float = float(cloud_mat.get_shader_parameter(&"u_layer_inner"))
	var slab_outer: float = float(cloud_mat.get_shader_parameter(&"u_layer_outer"))
	_assert(slab_inner > model.core_radius_m and slab_inner < model.cloud_top_radius_m
			and slab_outer - slab_inner < model.cloud_top_radius_m - model.core_radius_m,
		"the cloud deck is a bounded upper slice (%.0f..%.0f km)" % [slab_inner / 1000.0, slab_outer / 1000.0])
	_assert(float(cloud_mat.get_shader_parameter(&"u_is_cloud")) > 0.5,
		"the cloud deck runs the textured grey-cloud path (u_is_cloud 1)")
	var cov_tex: Texture2D = cloud_mat.get_shader_parameter(&"u_coverage_tex")
	_assert(cov_tex != null and cov_tex.get_width() >= 256 and cov_tex.get_height() >= 128,
		"the cloud deck has a procedural equirect coverage texture (%s)"
			% ("null" if cov_tex == null else "%dx%d" % [cov_tex.get_width(), cov_tex.get_height()]))
	_assert(float(cloud_mat.get_shader_parameter(&"u_deck_width")) > 0.0
			and float(cloud_mat.get_shader_parameter(&"u_flow")) > 0.0
			and float(cloud_mat.get_shader_parameter(&"u_deck_altitude")) > 0.5,
		"the cloud deck carries altitude-profile + flow params")

	# The coverage texture has real spatial variation (bands / swirls), not flat.
	var img := cov_tex.get_image()
	var lo := 1.0
	var hi := 0.0
	for i in 400:
		var px := img.get_pixel((i * 37) % img.get_width(), (i * 53) % img.get_height())
		lo = minf(lo, px.r)
		hi = maxf(hi, px.r)
	_assert(hi - lo > 0.3, "the coverage map varies across the sphere (span %.2f)" % (hi - lo))

	var rho_top: float = float(atmo_mat.get_shader_parameter(&"u_rho_top"))
	var k: float = float(atmo_mat.get_shader_parameter(&"u_falloff_k"))

	# Two different gas giants get a visibly different coverage texture.
	var s2: GasGiantShell = GAS_GIANT_SHELL.new()
	add_child(s2)
	var m2: GasGiantModel = GAS_GIANT_MODEL.from_body(52_000_000.0, 5.0e15, 424242)
	s2.bind(m2, Color(0.5, 0.6, 0.35))
	var img2: Image = ((s2.get_child(1) as MeshInstance3D).material_override as ShaderMaterial) \
		.get_shader_parameter(&"u_coverage_tex").get_image()
	var same := 0
	for i in 300:
		var x := (i * 41) % img.get_width()
		var y := (i * 29) % img.get_height()
		if absf(img.get_pixel(x, y).r - img2.get_pixel(x, y).r) < 0.02:
			same += 1
	_assert(same < 240, "two gas giants get distinct coverage maps (%d/300 pixels matched)" % same)
	s2.queue_free()

	# --- The analytic density reproduces GasGiantModel.density_at ---------
	_assert(rho_top > 0.0 and rho_top < 1.0 and k > 0.0, "the density profile constants are sane")
	for i in 33:
		var f: float = float(i) / 32.0
		var r: float = lerpf(model.core_radius_m, model.cloud_top_radius_m, f)
		var shader_rho: float = rho_top * exp(k * (model.cloud_top_radius_m - clampf(r, model.core_radius_m, model.cloud_top_radius_m)))
		_assert(absf(shader_rho - model.density_at(r)) < maxf(model.density_at(r) * 0.02, 0.01),
			"shader density matches GasGiantModel at f=%.2f" % f)
	_assert(absf(rho_top * exp(k * (model.cloud_top_radius_m - model.core_radius_m)) - GAS_GIANT_MODEL.WATER_DENSITY) < 2.0,
		"the profile reaches liquid-water density exactly at the core")

	# --- Dormant from orbit; near the tops both decks; deep only the atmo ---
	var center := Vector3(1234.0, -56.0, 789.0)
	var sun := Vector3(0.3, 0.6, -0.74).normalized()

	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m * 2.5, 0))
	_assert(shell.active_band_count() == 0, "both decks dormant from orbit (far-LOD carries it)")
	_assert(not shell.suppresses_far_lod(), "far-LOD is NOT suppressed above the cloud tops")

	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m - 20_000.0, 0), 12_345.0)
	_assert(shell.active_band_count() == 2, "near the cloud tops the atmo deck + cloud deck raymarch")
	_assert(shell.suppresses_far_lod(), "far-LOD IS suppressed once below the cloud tops")
	_assert(atmo.visible, "the atmo deck is active inside the tops")

	shell.sync(center, sun, center + Vector3(0, model.core_radius_m + 30_000.0, 0))
	_assert(shell.active_band_count() == 1, "deep near the core ONLY the cheap atmo deck raymarches")
	_assert(atmo.visible and not cloud.visible, "deep, the atmo deck is the one still active")

	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m - 20_000.0, 0), 50_000.0)
	_assert((cloud_mat.get_shader_parameter(&"u_body_center") as Vector3).is_equal_approx(center),
		"sync() pushed the body centre to the cloud deck")
	_assert(absf(float(cloud_mat.get_shader_parameter(&"u_cloud_phase"))) > 1.0e-4,
		"the cloud coverage flows with the sim clock")
	_assert(shell.global_position.is_equal_approx(center), "the shell sits at the body centre")

	shell.queue_free()

	if _failed:
		get_tree().quit(1)
		return
	print("GAS_GIANT_SHELL_OK  (green atmo deck + textured grey cloud deck; core %.1f kg/m^3)"
		% [rho_top * exp(k * (model.cloud_top_radius_m - model.core_radius_m))])
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("GAS_GIANT_SHELL_FAILED: %s" % message)
