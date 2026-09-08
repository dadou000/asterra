extends Node
## GG2 (headless): the layered GasGiantShell builds its concentric band meshes +
## raymarch materials from a GasGiantModel, only lights up the band(s) near the
## observer, and the generator can reconstruct the model for any gas-giant body.
## Rendering itself is checked by the windowed gg2_shell_probe.
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

	# --- Model reconstruction is deterministic + gas-only -----------------
	var system: CelestialSystemDefinition = GENERATOR.generate(8571)
	var colossus: Resource = system.find_body("colossus")
	var rime: Resource = system.find_body("rime")
	_assert(GENERATOR.gas_giant_model_for(rime) == null, "no gas-giant model for a non-gas body")
	var model: GasGiantModel = GENERATOR.gas_giant_model_for(colossus)
	_assert(model != null, "the generator reconstructs colossus's GasGiantModel")
	_assert(is_equal_approx(model.core_radius_m, float(colossus.get(&"core_radius_m")))
			and is_equal_approx(model.cloud_top_radius_m, float(colossus.get(&"radius_m"))),
		"the reconstructed model matches the radii stamped on the body")

	# --- The shell builds concentric band meshes + raymarch materials ----
	var shell: GasGiantShell = GAS_GIANT_SHELL.new()
	add_child(shell)
	shell.bind(model, Color(0.5, 0.6, 0.35))

	var decks: Array = model.decks()
	_assert(decks.size() >= 3, "the envelope has several decks (%d)" % decks.size())
	_assert(shell.band_count() == decks.size(), "the shell builds one mesh per deck")
	var bands: Array = shell.get_children()
	var prev_outer: float = INF
	var rho_top: float = 0.0
	var k: float = 0.0
	var kinds: Array = []
	for i in bands.size():
		var mi: MeshInstance3D = bands[i]
		_assert(mi.mesh is SphereMesh, "deck %d mesh is a sphere" % i)
		_assert(not mi.visible, "deck %d starts hidden (dormant until the observer nears it)" % i)
		var mat: ShaderMaterial = mi.material_override as ShaderMaterial
		_assert(mat != null and mat.shader != null
				and mat.shader.resource_path.ends_with("gas_giant_shell.gdshader"),
			"deck %d runs the raymarch shader" % i)
		var inner: float = float(mat.get_shader_parameter(&"u_layer_inner"))
		var outer: float = float(mat.get_shader_parameter(&"u_layer_outer"))
		_assert(outer > inner and inner >= model.core_radius_m - 1.0,
			"deck %d covers a radial slice down to at least the core [%.0f, %.0f] km" % [i, inner / 1000.0, outer / 1000.0])
		_assert(outer <= prev_outer + 1.0, "decks are ordered outer -> inner")
		_assert(is_equal_approx(mi.scale.x, outer), "deck %d mesh is scaled to its outer radius" % i)
		_assert(int(mat.get_shader_parameter(&"u_steps")) >= 4
				and int(mat.get_shader_parameter(&"u_steps")) <= 24,
			"deck %d carries a bounded step count" % i)
		_assert(float(mat.get_shader_parameter(&"u_density_mul")) >= 1.0,
			"deck %d density multiplier is >= the smooth base" % i)
		prev_outer = outer
		rho_top = float(mat.get_shader_parameter(&"u_rho_top"))
		k = float(mat.get_shader_parameter(&"u_falloff_k"))
		kinds.append(mi.name)
	# The deck stack reaches from a thin band near the cloud tops down to the core,
	# and includes at least one banded "cloud" deck and the smooth "haze" base.
	var last_mat: ShaderMaterial = (bands[bands.size() - 1] as MeshInstance3D).material_override
	_assert(is_equal_approx(float(last_mat.get_shader_parameter(&"u_layer_inner")), model.core_radius_m),
		"the innermost deck's floor is the core")
	var joined := ",".join(PackedStringArray(kinds))
	_assert(joined.contains("cloud") and joined.contains("haze"),
		"the stack has both banded cloud decks and the smooth haze base (%s)" % joined)

	# --- The analytic density (rho_top * exp(k*(cloud_top - r))) reproduces
	#     GasGiantModel.density_at across the shell -----------------------
	_assert(rho_top > 0.0 and rho_top < 1.0 and k > 0.0, "the density profile constants are sane")
	for i in 33:
		var f: float = float(i) / 32.0
		var r: float = lerpf(model.core_radius_m, model.cloud_top_radius_m, f)
		var shader_rho: float = rho_top * exp(k * (model.cloud_top_radius_m - clampf(r, model.core_radius_m, model.cloud_top_radius_m)))
		_assert(absf(shader_rho - model.density_at(r)) < maxf(model.density_at(r) * 0.02, 0.01),
			"shader density matches GasGiantModel at f=%.2f (%.3f vs %.3f)" % [f, shader_rho, model.density_at(r)])
	_assert(absf(rho_top * exp(k * (model.cloud_top_radius_m - model.core_radius_m)) - GAS_GIANT_MODEL.WATER_DENSITY) < 2.0,
		"the profile reaches liquid-water density exactly at the core")

	# --- Only the band(s) near the observer light up -------------------
	var center := Vector3(1234.0, -56.0, 789.0)
	var sun := Vector3(0.3, 0.6, -0.74).normalized()

	# Far above the cloud tops: the whole envelope is dormant (far-LOD covers it).
	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m * 2.5, 0))
	_assert(shell.active_band_count() == 0, "no band raymarches from orbit")

	# Just under the cloud tops: only the top band or two.
	shell.sync(center, sun, center + Vector3(0, model.cloud_top_radius_m - 20_000.0, 0))
	var top_active := shell.active_band_count()
	_assert(top_active >= 1 and top_active <= 2,
		"near the cloud tops 1-2 bands raymarch, not the whole envelope (%d)" % top_active)
	_assert((shell.get_child(0) as MeshInstance3D).visible, "the outermost band is one of them")

	# Deep, near the core: still only a bounded number of bands, and a DIFFERENT
	# set than at the top.
	var deep_obs := center + Vector3(0, model.core_radius_m + 30_000.0, 0)
	shell.sync(center, sun, deep_obs)
	var deep_active := shell.active_band_count()
	_assert(deep_active >= 1 and deep_active <= 3,
		"deep in the envelope only a bounded number of bands raymarch (%d)" % deep_active)
	_assert(not (shell.get_child(0) as MeshInstance3D).visible,
		"the outermost band is dormant once the camera is near the core")

	# sync() fed the per-frame uniforms on the active band(s).
	for mi2: Node in shell.get_children():
		if (mi2 as MeshInstance3D).visible:
			var m2: ShaderMaterial = (mi2 as MeshInstance3D).material_override
			var bc: Vector3 = m2.get_shader_parameter(&"u_body_center")
			var sd: Vector3 = m2.get_shader_parameter(&"u_sun_dir")
			_assert(bc.is_equal_approx(center), "sync() pushed the body centre to an active band")
			_assert(sd.is_finite() and is_equal_approx(sd.length(), 1.0),
				"sync() pushed a unit sun direction to an active band")
	_assert(shell.global_position.is_equal_approx(center), "the shell sits at the body centre")

	shell.queue_free()

	if _failed:
		get_tree().quit(1)
		return
	print("GAS_GIANT_SHELL_OK  (%d bands; top %d active, deep %d active; core %.1f kg/m^3)"
		% [shell.band_count(), top_active, deep_active,
			rho_top * exp(k * (model.cloud_top_radius_m - model.core_radius_m))])
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("GAS_GIANT_SHELL_FAILED: %s" % message)
