class_name ProceduralSky
extends Node3D
## Procedural night-sky manager for 0.0.5 terrain.
##
## Stars are generated once into a MultiMesh and rendered in one instanced draw.
## Apparent radiance is fixed by generated apparent magnitude. Stellar colour is
## generated from a plausible naked-eye spectral-temperature distribution; colour
## never changes the assigned stellar luminance.

const FAR_FIELD_SCALE := 0.58
# Fixed point-spread footprint for every unresolved star. This is a rasterization
# footprint, not a brightness-dependent visual size.
const BASE_POINT_SIZE := 0.00105
const BRIGHTEST_MAGNITUDE := -1.5
const NAKED_EYE_LIMIT_MAGNITUDE := 6.5
const MIN_STELLAR_TEMPERATURE_K := 2400.0
const MAX_STELLAR_TEMPERATURE_K := 18000.0
const REFERENCE_CATALOGUE_COUNT := 9000
const DEBUG_PARAMETER_EPSILON := 0.0001

# Approximate all-sky cumulative naked-eye counts. Keep these as literal constant
# Arrays: PackedFloat32Array(...) is a constructor call and therefore is not a
# compile-time constant expression in GDScript.
const MAGNITUDE_KNOTS = [
	-1.5, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 6.5
]
const CUMULATIVE_STAR_COUNTS = [
	1.0, 2.0, 4.0, 15.0, 48.0, 171.0, 513.0, 1602.0, 4800.0, 9000.0
]

var observer: AsterraPlayer
var star_mesh: MultiMeshInstance3D
var star_quad: QuadMesh
var star_material: ShaderMaterial

# Debug overrides. Defaults are the calibrated rendering path. They are stored on
# the manager so controls can be changed before the MultiMesh material is built.
var star_radiance_scale := 1.0
var magnitude_flux_exponent := 1.0
var seeing_strength := 1.0
var colour_strength := 1.0
var chromatic_scintillation_strength := 1.0
var debug_controls_synchronized := true


func configure(p_observer: AsterraPlayer) -> void:
	if p_observer == null or observer == p_observer:
		return
	observer = p_observer
	_build_stars()


func _process(_delta: float) -> void:
	if observer == null or not is_instance_valid(observer):
		_try_bind_observer()
		return
	if observer.camera == null or star_material == null:
		return

	# Keep the catalogue camera-relative so floating-origin rebases never touch the
	# stellar transforms and there is no large-coordinate precision loss.
	global_position = observer.camera.global_position
	var far_radius := maxf(observer.camera.far * FAR_FIELD_SCALE, 5000.0)
	scale = Vector3.ONE * far_radius
	_set_star_shader_parameter("u_up", observer.up_dir(), false)
	_set_star_shader_parameter("u_camera_height", observer.altitude(), false)
	_set_star_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height, false)
	_set_star_shader_parameter("u_planet_radius", Planet.cfg.planet_radius, false)


func _try_bind_observer() -> void:
	var root := get_parent()
	if root == null:
		return
	for child in root.get_children():
		if child is AsterraPlayer:
			configure(child as AsterraPlayer)
			return


func _star_count() -> int:
	# Quality tiers are magnitude-limited catalogues: lower presets discard only
	# the faint tail. The bright stars and their fluxes are identical on all tiers.
	match GraphicsQuality.sanitize(AppSettings.graphics_quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 3500
		GraphicsQuality.Preset.BALANCED:
			return 5000
		GraphicsQuality.Preset.ULTRA:
			return REFERENCE_CATALOGUE_COUNT
		_:
			return 7000


func _build_stars() -> void:
	if Planet.cfg == null:
		return
	if star_mesh != null:
		star_mesh.queue_free()
		star_mesh = null

	star_quad = QuadMesh.new()
	star_quad.size = Vector2.ONE

	star_material = ShaderMaterial.new()
	star_material.shader = load("res://shaders/procedural_stars.gdshader")

	# Bind the exact same material to both possible geometry paths. The override is
	# authoritative, while the QuadMesh material makes the relationship explicit and
	# prevents a future refactor from silently bypassing the live debug material.
	star_quad.material = star_material

	var star_count := _star_count()
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_custom_data = true
	multimesh.mesh = star_quad
	multimesh.instance_count = star_count

	var rng := RandomNumberGenerator.new()
	# Deterministic celestial catalogue with a salt independent from terrain noise.
	rng.seed = int(Planet.cfg.world_seed) ^ 0x5A17B1E

	for i in star_count:
		var direction := _random_unit_vector(rng)
		# The catalogue is ordered brightest to faintest. A sub-rank jitter avoids
		# visibly quantized magnitude bands while preserving cumulative star counts.
		var catalogue_rank := clampf(
			float(i) + rng.randf_range(0.35, 1.0),
			1.0,
			float(REFERENCE_CATALOGUE_COUNT)
		)
		var magnitude := _magnitude_from_catalogue_rank(catalogue_rank)
		var magnitude_norm := inverse_lerp(
			BRIGHTEST_MAGNITUDE,
			NAKED_EYE_LIMIT_MAGNITUDE,
			magnitude
		)

		# Encode actual colour temperature rather than an arbitrary warm/cool slider.
		# The shader converts this to blackbody-like chromaticity and then explicitly
		# normalizes luminance, preserving the magnitude-derived radiance.
		var temperature_k := _sample_stellar_temperature_k(rng)
		var temperature_norm := inverse_lerp(
			MIN_STELLAR_TEMPERATURE_K,
			MAX_STELLAR_TEMPERATURE_K,
			temperature_k
		)

		# These are stochastic atmospheric turbulence spectra, not literal periodic
		# blinking animations. All are approximately zero-mean around fixed radiance.
		var scintillation_pattern := _sample_scintillation_pattern(rng)
		var phase := rng.randf()

		# All unresolved stars use the same sampling footprint. Their prominence is
		# exclusively a consequence of magnitude-derived radiance and the renderer.
		var basis := Basis.IDENTITY.scaled(Vector3.ONE * BASE_POINT_SIZE)
		var xform := Transform3D(basis, direction)
		multimesh.set_instance_transform(i, xform)
		multimesh.set_instance_custom_data(i, Color(
			magnitude_norm,
			temperature_norm,
			float(scintillation_pattern) / 4.0,
			phase
		))

	star_mesh = MultiMeshInstance3D.new()
	star_mesh.name = "InstancedStars"
	star_mesh.multimesh = multimesh
	star_mesh.material_override = star_material
	star_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	star_mesh.visibility_range_end = 0.0
	star_mesh.extra_cull_margin = 100000000.0
	add_child(star_mesh)

	# Apply after every rendered reference exists. In particular, a zero-radiance
	# diagnostic also hard-disables the geometry, so the test cannot be defeated by
	# stale shader uniforms or an internally duplicated material resource.
	_apply_debug_parameters()


func _magnitude_from_catalogue_rank(rank: float) -> float:
	var last_count_index := CUMULATIVE_STAR_COUNTS.size() - 1
	var safe_rank := clampf(
		rank,
		float(CUMULATIVE_STAR_COUNTS[0]),
		float(CUMULATIVE_STAR_COUNTS[last_count_index])
	)
	for i in range(1, MAGNITUDE_KNOTS.size()):
		var c1 := float(CUMULATIVE_STAR_COUNTS[i])
		if safe_rank <= c1:
			var c0 := float(CUMULATIVE_STAR_COUNTS[i - 1])
			# Counts are approximately exponential with magnitude, so interpolate in
			# log-count space rather than linearly in population.
			var denom := maxf(log(c1) - log(c0), 1e-6)
			var t := (log(safe_rank) - log(c0)) / denom
			return lerpf(
				float(MAGNITUDE_KNOTS[i - 1]),
				float(MAGNITUDE_KNOTS[i]),
				clampf(t, 0.0, 1.0)
			)
	return NAKED_EYE_LIMIT_MAGNITUDE


func _sample_stellar_temperature_k(rng: RandomNumberGenerator) -> float:
	# Naked-eye catalogue weighting, not the underlying galactic population. Cool
	# M dwarfs are intrinsically common but usually too faint to enter a naked-eye
	# catalogue, while luminous A/B stars are strongly overrepresented.
	var roll := rng.randf()
	if roll < 0.06:
		return rng.randf_range(10000.0, 18000.0) # B: blue-white
	if roll < 0.18:
		return rng.randf_range(7500.0, 10000.0) # A: white / blue-white
	if roll < 0.36:
		return rng.randf_range(6000.0, 7500.0) # F: warm white
	if roll < 0.57:
		return rng.randf_range(5200.0, 6000.0) # G: solar white-yellow
	if roll < 0.88:
		return rng.randf_range(3700.0, 5200.0) # K: pale orange
	return rng.randf_range(2400.0, 3700.0) # M: orange-red


func _sample_scintillation_pattern(rng: RandomNumberGenerator) -> int:
	# Every stellar point is physically susceptible to scintillation, but some
	# lines of sight are much calmer. Pattern 0 is nearly steady; 4 is the rare
	# strongly turbulent / bursty case that becomes conspicuous near the horizon.
	var roll := rng.randf()
	if roll < 0.10:
		return 0
	if roll < 0.43:
		return 1
	if roll < 0.74:
		return 2
	if roll < 0.94:
		return 3
	return 4


func set_star_radiance_scale(value: float) -> void:
	star_radiance_scale = clampf(value, 0.0, 2.0)
	_apply_debug_parameters()


func set_magnitude_flux_exponent(value: float) -> void:
	magnitude_flux_exponent = clampf(value, 0.5, 1.5)
	_apply_debug_parameters()


func set_seeing_strength(value: float) -> void:
	seeing_strength = clampf(value, 0.0, 2.0)
	_apply_debug_parameters()


func set_colour_strength(value: float) -> void:
	colour_strength = clampf(value, 0.0, 1.0)
	_apply_debug_parameters()


func set_chromatic_scintillation_strength(value: float) -> void:
	chromatic_scintillation_strength = clampf(value, 0.0, 2.0)
	_apply_debug_parameters()


func reset_debug_calibration() -> void:
	star_radiance_scale = 1.0
	magnitude_flux_exponent = 1.0
	seeing_strength = 1.0
	colour_strength = 1.0
	chromatic_scintillation_strength = 1.0
	_apply_debug_parameters()


func _apply_debug_parameters() -> void:
	# Always reassert the material relationship before writing controls. This makes
	# every non-zero slider value just as authoritative as the zero hard-hide test.
	_ensure_live_material_binding()

	# Zero is also a hard geometry-off diagnostic. This is deliberately redundant
	# with the shader scale: if any stars remain while this node is hidden, they are
	# being produced by another renderer and not by this ProceduralSky catalogue.
	if star_mesh != null and is_instance_valid(star_mesh):
		star_mesh.visible = star_radiance_scale > 0.00001

	debug_controls_synchronized = true
	debug_controls_synchronized = _set_star_shader_parameter(
		"u_star_radiance_scale", star_radiance_scale, true
	) and debug_controls_synchronized
	debug_controls_synchronized = _set_star_shader_parameter(
		"u_magnitude_flux_exponent", magnitude_flux_exponent, true
	) and debug_controls_synchronized
	debug_controls_synchronized = _set_star_shader_parameter(
		"u_seeing_strength", seeing_strength, true
	) and debug_controls_synchronized
	debug_controls_synchronized = _set_star_shader_parameter(
		"u_colour_strength", colour_strength, true
	) and debug_controls_synchronized
	debug_controls_synchronized = _set_star_shader_parameter(
		"u_chromatic_scintillation_strength",
		chromatic_scintillation_strength,
		true
	) and debug_controls_synchronized

	if Planet.cfg != null:
		_set_star_shader_parameter("u_planet_radius", Planet.cfg.planet_radius, false)
		_set_star_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height, false)


func _ensure_live_material_binding() -> void:
	if star_material == null:
		return
	if star_quad != null and star_quad.material != star_material:
		star_quad.material = star_material
	if star_mesh != null and is_instance_valid(star_mesh):
		if star_mesh.material_override != star_material:
			star_mesh.material_override = star_material
		if star_mesh.multimesh != null and star_mesh.multimesh.mesh != star_quad:
			star_mesh.multimesh.mesh = star_quad


func _set_star_shader_parameter(
	parameter: StringName,
	value: Variant,
	verify_numeric: bool = false
) -> bool:
	if star_material == null:
		return false

	star_material.set_shader_parameter(parameter, value)

	# Keep every possible rendered reference synchronized even if Godot or an editor
	# operation duplicates a material resource behind our back.
	if star_quad != null and star_quad.material is ShaderMaterial:
		var quad_material := star_quad.material as ShaderMaterial
		if quad_material != star_material:
			quad_material.set_shader_parameter(parameter, value)

	if star_mesh != null and is_instance_valid(star_mesh):
		var override_material := star_mesh.material_override
		if override_material is ShaderMaterial and override_material != star_material:
			(override_material as ShaderMaterial).set_shader_parameter(parameter, value)

	if not verify_numeric:
		return true
	var applied: Variant = star_material.get_shader_parameter(parameter)
	var applied_type := typeof(applied)
	if applied_type != TYPE_FLOAT and applied_type != TYPE_INT:
		push_warning("ProceduralSky: shader parameter '%s' did not return a numeric value" % parameter)
		return false
	var matches := absf(float(applied) - float(value)) <= DEBUG_PARAMETER_EPSILON
	if not matches:
		push_warning(
			"ProceduralSky: shader parameter '%s' requested %.4f but material reports %.4f"
			% [parameter, float(value), float(applied)]
		)
	return matches


func debug_get_applied_parameter(parameter: StringName) -> Variant:
	if star_material == null:
		return null
	return star_material.get_shader_parameter(parameter)


func debug_star_instance_count() -> int:
	if star_mesh == null or not is_instance_valid(star_mesh):
		return 0
	if star_mesh.multimesh == null:
		return 0
	return star_mesh.multimesh.instance_count


func debug_star_mesh_visible() -> bool:
	return star_mesh != null and is_instance_valid(star_mesh) and star_mesh.visible


func _random_unit_vector(rng: RandomNumberGenerator) -> Vector3:
	var z := rng.randf_range(-1.0, 1.0)
	var a := rng.randf_range(0.0, TAU)
	var r := sqrt(maxf(1.0 - z * z, 0.0))
	return Vector3(r * cos(a), z, r * sin(a))
