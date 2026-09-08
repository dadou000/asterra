extends Node
## GG2/GG3 (headless): the GasGiantShell builds a cheap full-envelope "fog" deck
## + a bounded detailed "cloud" slab from a GasGiantModel, raymarches them only
## near/inside the cloud tops, carries the Jool band / swirl params, rotates with
## the sim clock, and the generator reconstructs the model for any gas-giant body.
## Rendering is checked by the windowed gg2_shell_probe / gg_look_4k probes.
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

	# --- fog deck (whole envelope, cheap) + cloud slab (bounded, detailed) ---
	var decks: Array = model.decks()
	_assert(decks.size() == 2, "the envelope is a fog deck + a cloud slab (%d)" % decks.size())
	_assert(String(decks[0].get("kind")) == "fog" and String(decks[1].get("kind")) == "cloud",
		"deck 0 is the fog deck, deck 1 the cloud slab")
	_assert(shell.band_count() == 2, "the shell builds one mesh per deck")

	var fog: MeshInstance3D = shell.get_child(0)
	var cloud: MeshInstance3D = shell.get_child(1)
	var fog_mat: ShaderMaterial = fog.material_override
	var cloud_mat: ShaderMaterial = cloud.material_override
	for m: ShaderMaterial in [fog_mat, cloud_mat]:
		_assert(m != null and m.shader != null
				and m.shader.resource_path.ends_with("gas_giant_shell.gdshader"),
			"the deck runs the raymarch shader")
	_assert(not fog.visible and not cloud.visible, "both decks start hidden (dormant above the tops)")

	# The fog deck spans the whole gas body down to the core -> a ray inside the
	# envelope is never left transparent (no black seams / no stars).
	_assert(is_equal_approx(float(fog_mat.get_shader_parameter(&"u_layer_inner")), model.core_radius_m),
		"the fog deck floor is the solid core")
	_assert(float(fog_mat.get_shader_parameter(&"u_layer_outer")) >= model.cloud_top_radius_m,
		"the fog deck ceiling is at/above the cloud tops")
	_assert(is_zero_approx(float(fog_mat.get_shader_parameter(&"u_is_cloud"))),
		"the fog deck runs the cheap no-noise path (u_is_cloud 0)")

	# The cloud slab is a bounded slice in the UPPER envelope.
	var slab_inner: float = float(cloud_mat.get_shader_parameter(&"u_layer_inner"))
	var slab_outer: float = float(cloud_mat.get_shader_parameter(&"u_layer_outer"))
	_assert(slab_inner > model.core_radius_m and slab_inner < model.cloud_top_radius_m,
		"the cloud slab floor is well above the core (%.0f km)" % (slab_inner / 1000.0))
	_assert(slab_outer >= model.cloud_top_radius_m and slab_outer - slab_inner < model.cloud_top_radius_m - model.core_radius_m,
		"the cloud slab is a bounded slice, not the whole envelope")
	_assert(float(cloud_mat.get_shader_parameter(&"u_is_cloud")) > 0.5,
		"the cloud slab runs the detailed noise path (u_is_cloud 1)")
	_assert(float(cloud_mat.get_shader_parameter(&"u_band_count")) >= 3.0
			and float(cloud_mat.get_shader_parameter(&"u_band_contrast")) > 0.0
			and float(cloud_mat.get_shader_parameter(&"u_noise_freq")) > 1.0
			and float(cloud_mat.get_shader_parameter(&"u_warp")) > 0.0,
		"the cloud slab carries band / swirl / warp params")

	var rho_top: float = float(fog_mat.get_shader_parameter(&"u_rho_top"))
	var k: float = float(fog_mat.get_shader_parameter(&"u_falloff_k"))

	# Two different gas giants get visibly different cloud structure.
	var m2: GasGiantModel = GAS_GIANT_MODEL.from_body(52_000_000.0, 5.0e15, 424242)
	var a: Dictionary = model.decks()[1]
	var b: Dictionary = m2.decks()[1]
	_assert(not is_equal_approx(float(a.get("noise_freq")), float(b.get("noise_freq")))
			or not is_equal_approx(float(a.get("coverage")), float(b.get("coverage")))
			or not is_equal_approx(float(a.get("band_count")), float(b.get("band_count"))),
		"two gas giants get distinct cloud-slab parameters")

	# --- The analytic density reproduces GasGiantModel.density_at ---------
	_assert(rho_top > 0.0 and rho_top < 1.0 and k > 0.0, "the density profile constants are sane")
	for i in 33:
		var f: float = float(i) / 32.0
		var r: float = lerpf(model.core_radius_m, model.cloud_top_radius_m, f)
		var shader_rho: float = rho_top * exp(k * (model.cloud_top_radius_m - clampf(r, model.core_radius_m, model.cloud_top_radius_m)))
		_assert(absf(shader_rho - model.density_at(r)) < maxf(model.density_at(r) * 0.02, 0.01),
			"shader density matches GasGiantModel at f=%.2f (%.3f vs %.3f)" % [f, shader_rho, model.density_at(r)])
	_assert(absf(rho_top * exp(k * (model.cloud_top_radius_m - model.core_radius_m)) - GAS_GIANT_MODEL.WATER_DENSITY) < 2.0,
		"the profile reaches liquid-water density exactly at the core")

	# --- Dormant from orbit; near the tops both decks; deep only the fog ----
	var center := Vector3(1234.0, -56.0, 789.0)
	var sun := Vector3(0.3, 0.6, -0.74).normalized()

	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m * 2.5, 0))
	_assert(shell.active_band_count() == 0, "both decks dormant from orbit (far-LOD carries it)")
	_assert(not shell.suppresses_far_lod(), "far-LOD is NOT suppressed above the cloud tops")

	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m - 20_000.0, 0), 12_345.0)
	_assert(shell.active_band_count() == 2, "near the cloud tops the fog deck + cloud slab raymarch")
	_assert(shell.suppresses_far_lod(), "far-LOD IS suppressed once below the cloud tops")
	_assert(fog.visible, "the fog deck is active inside the tops")

	shell.sync(center, sun, center + Vector3(0, model.core_radius_m + 30_000.0, 0))
	_assert(shell.active_band_count() == 1, "deep near the core ONLY the cheap fog deck raymarches")
	_assert(fog.visible and not cloud.visible, "deep, the fog deck is the one still active")

	# sync() fed the per-frame uniforms and rotated the cloud slab with the sim clock.
	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m - 20_000.0, 0), 50_000.0)
	_assert((cloud_mat.get_shader_parameter(&"u_body_center") as Vector3).is_equal_approx(center),
		"sync() pushed the body centre to the cloud slab")
	var sd: Vector3 = cloud_mat.get_shader_parameter(&"u_sun_dir")
	_assert(sd.is_finite() and is_equal_approx(sd.length(), 1.0),
		"sync() pushed a unit sun direction")
	_assert(absf(float(cloud_mat.get_shader_parameter(&"u_cloud_phase"))) > 1.0e-4,
		"the cloud slab bands rotate with the sim clock")
	_assert(shell.global_position.is_equal_approx(center), "the shell sits at the body centre")

	shell.queue_free()

	if _failed:
		get_tree().quit(1)
		return
	print("GAS_GIANT_SHELL_OK  (fog deck + cloud slab; core %.1f kg/m^3)"
		% [rho_top * exp(k * (model.cloud_top_radius_m - model.core_radius_m))])
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("GAS_GIANT_SHELL_FAILED: %s" % message)
