class_name ProceduralSky
extends Node3D
## Procedural night-sky manager for 0.0.5 terrain.
##
## Stars are generated once into a MultiMesh and rendered in one instanced draw.
## Large-scale galactic/nebular structure remains in the sky shader where it is
## effectively free compared with creating geometry for diffuse features.

const STAR_COUNT := 7000
const TWINKLE_FRACTION := 0.16
const FAR_FIELD_SCALE := 0.58
const BASE_POINT_SIZE := 0.00115

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
	star_material.set_shader_parameter("u_sun_dir", Frames.helion_dir.normalized())
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

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_custom_data = true
	multimesh.mesh = quad
	multimesh.instance_count = STAR_COUNT

	var rng := RandomNumberGenerator.new()
	# The sky is deterministic for a world seed. A separate salt ensures changing
	# terrain generation does not accidentally correlate stars with terrain noise.
	rng.seed = int(Planet.cfg.world_seed) ^ 0x5A17B1E

	for i in STAR_COUNT:
		var direction := _random_unit_vector(rng)
		# Strong faint-star bias approximates the naked-eye magnitude distribution:
		# many threshold stars, few bright ones, and a very sparse standout tail.
		var magnitude_bias := pow(rng.randf(), 3.0)
		var brightness := lerpf(0.010, 0.42, magnitude_bias)
		if rng.randf() < 0.008:
			brightness = lerpf(0.42, 0.72, rng.randf())

		var temperature := clampf(0.50 + rng.randfn(0.0, 0.22), 0.0, 1.0)
		var twinkles := rng.randf() < TWINKLE_FRACTION
		var pattern := 0
		if twinkles:
			pattern = rng.randi_range(1, 4)
		var phase := rng.randf()

		# Most stars remain unresolved sub-pixel points. A tiny bright tail grows
		# slightly so temporal AA retains them without producing oversized discs.
		var angular_size := BASE_POINT_SIZE * lerpf(0.70, 1.65, sqrt(brightness / 0.72))
		var basis := Basis.IDENTITY.scaled(Vector3.ONE * angular_size)
		var xform := Transform3D(basis, direction)
		multimesh.set_instance_transform(i, xform)
		multimesh.set_instance_custom_data(i, Color(
			brightness,
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


func _random_unit_vector(rng: RandomNumberGenerator) -> Vector3:
	var z := rng.randf_range(-1.0, 1.0)
	var a := rng.randf_range(0.0, TAU)
	var r := sqrt(maxf(1.0 - z * z, 0.0))
	return Vector3(r * cos(a), z, r * sin(a))
