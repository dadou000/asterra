class_name ProceduralSky
extends Node3D
## Procedural night-sky manager for 0.0.5 terrain.
##
## Stars are generated once into a MultiMesh and rendered in one instanced draw.
## Their apparent radiance is fixed from generated apparent magnitude. Nothing in
## this manager raises stellar brightness at night; visibility must emerge from
## atmospheric radiance, extinction, eye adaptation, tonemapping and bloom.

const TWINKLE_FRACTION := 0.16
const FAR_FIELD_SCALE := 0.58
# Fixed point-spread footprint for every unresolved star. This is a rasterization
# footprint, not a brightness-dependent visual size.
const BASE_POINT_SIZE := 0.00105
const BRIGHTEST_MAGNITUDE := -1.5
const NAKED_EYE_LIMIT_MAGNITUDE := 6.5

var observer: AsterraPlayer
var star_mesh: MultiMeshInstance3D
var star_material: ShaderMaterial


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
	star_material.set_shader_parameter("u_up", observer.up_dir())
	star_material.set_shader_parameter("u_camera_height", observer.altitude())
	star_material.set_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height)


func _try_bind_observer() -> void:
	var root := get_parent()
	if root == null:
		return
	for child in root.get_children():
		if child is AsterraPlayer:
			configure(child as AsterraPlayer)
			return


func _star_count() -> int:
	# One draw call on every preset; only catalogue density changes. Lower tiers
	# remove stars rather than changing the radiance of the stars that remain.
	match GraphicsQuality.sanitize(AppSettings.graphics_quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 3500
		GraphicsQuality.Preset.BALANCED:
			return 5000
		GraphicsQuality.Preset.ULTRA:
			return 10000
		_:
			return 7000


func _build_stars() -> void:
	if Planet.cfg == null:
		return
	if star_mesh != null:
		star_mesh.queue_free()

	var quad := QuadMesh.new()
	quad.size = Vector2.ONE

	star_material = ShaderMaterial.new()
	star_material.shader = load("res://shaders/procedural_stars.gdshader")
	quad.material = star_material

	var star_count := _star_count()
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_custom_data = true
	multimesh.mesh = quad
	multimesh.instance_count = star_count

	var rng := RandomNumberGenerator.new()
	# Deterministic celestial catalogue with a salt independent from terrain noise.
	rng.seed = int(Planet.cfg.world_seed) ^ 0x5A17B1E

	for i in star_count:
		var direction := _random_unit_vector(rng)
		var magnitude := _sample_apparent_magnitude(rng)
		var magnitude_norm := inverse_lerp(
			BRIGHTEST_MAGNITUDE,
			NAKED_EYE_LIMIT_MAGNITUDE,
			magnitude
		)

		var temperature := clampf(0.50 + rng.randfn(0.0, 0.22), 0.0, 1.0)
		var twinkles := rng.randf() < TWINKLE_FRACTION
		var pattern := 0
		if twinkles:
			pattern = rng.randi_range(1, 4)
		var phase := rng.randf()

		# All unresolved stars use the same sampling footprint. Their prominence is
		# exclusively a consequence of magnitude-derived radiance and the renderer.
		var basis := Basis.IDENTITY.scaled(Vector3.ONE * BASE_POINT_SIZE)
		var xform := Transform3D(basis, direction)
		multimesh.set_instance_transform(i, xform)
		multimesh.set_instance_custom_data(i, Color(
			magnitude_norm,
			temperature,
			float(pattern) / 4.0,
			phase
		))

	star_mesh = MultiMeshInstance3D.new()
	star_mesh.multimesh = multimesh
	star_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	star_mesh.visibility_range_end = 0.0
	star_mesh.extra_cull_margin = 100000000.0
	add_child(star_mesh)


func _sample_apparent_magnitude(rng: RandomNumberGenerator) -> float:
	# Approximate naked-eye cumulative star counts with log N(<m) proportional to
	# 10^(0.6m). This naturally gives many faint stars and very few bright ones.
	# Crucially, it generates a physical magnitude; no later stage brightens stars
	# according to time of day or desired appearance.
	var bright_count := pow(10.0, 0.6 * BRIGHTEST_MAGNITUDE)
	var faint_count := pow(10.0, 0.6 * NAKED_EYE_LIMIT_MAGNITUDE)
	var cumulative := lerpf(bright_count, faint_count, rng.randf())
	return log(cumulative) / (0.6 * log(10.0))


func _random_unit_vector(rng: RandomNumberGenerator) -> Vector3:
	var z := rng.randf_range(-1.0, 1.0)
	var a := rng.randf_range(0.0, TAU)
	var r := sqrt(maxf(1.0 - z * z, 0.0))
	return Vector3(r * cos(a), z, r * sin(a))
