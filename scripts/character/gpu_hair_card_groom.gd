extends "res://scripts/character/natural_facial_hair_groom.gd"

## Realtime guide/follower scalp groom.
##
## A compact set of 3D guide chains is solved with fixed-length and shape
## constraints, then uploaded as a float texture. Thousands of independently
## tapered follower fibers are rendered by one MultiMesh draw. Simulation and
## render density are deliberately decoupled: changing motion quality does not
## require rebuilding render geometry.

const HAIR_CARD_SHADER := preload("res://shaders/character_hair_cards.gdshader")
const SCALP_CAP_SHADER := preload("res://shaders/character_scalp_cap.gdshader")
const GUIDE_COUNT := 96
const GUIDE_SEGMENTS := 16
const CARD_SEGMENTS := 16
## Each card is one lock carrying a baked hair strip, not a bundle of modelled
## fibers. Coverage comes from the strip's alpha, so a lock costs 64 triangles
## instead of the 288 the fiber bundle needed.
const CARD_COLUMNS := 3
## Strip variants a card can draw, side by side in one texture.
const STRIP_VARIANTS := 3
const STRIP_VARIANT_WIDTH := 96
const STRIP_HEIGHT := 512
const STRIP_STRANDS := 32
## Collision resolution for the head. The skull, brow, nose, cheeks and chin
## are all radially outward from the head centre, so the whole head is a
## height field over a sphere: one float per direction, O(1) to query, and it
## fits the real mesh instead of approximating it with primitives.
const HEAD_FIELD_AZIMUTH := 96
const HEAD_FIELD_ELEVATION := 48
const SOLVER_ITERATIONS := 5
const MIN_PINNED_ROOT_SEGMENTS := 2
const MAX_PINNED_ROOT_SEGMENTS := 4
const FIXED_PHYSICS_STEP := 1.0 / 60.0
const LOD_CHECK_INTERVAL := 0.15

var _gpu_hair_instance: MultiMeshInstance3D
var _gpu_hair_material: ShaderMaterial
var _scalp_cap_instance: MeshInstance3D
var _scalp_cap_material: ShaderMaterial
var _hair_strip_texture: ImageTexture
var _guide_image: Image
var _guide_texture: ImageTexture
var _guide_positions := PackedVector3Array()
var _guide_previous_positions := PackedVector3Array()
var _guide_rest_positions := PackedVector3Array()
var _guide_reference_positions := PackedVector3Array()
var _guide_roots := PackedVector3Array()
var _guide_segment_lengths := PackedFloat32Array()
var _guide_width_axes := PackedVector3Array()
var _guide_flow_axes := PackedVector3Array()
var _guide_normal_axes := PackedVector3Array()
var _guide_lateral_bias := PackedFloat32Array()
var _head_field := PackedFloat32Array()
var _head_field_center := Vector3.ZERO
var _head_field_texture: ImageTexture
var _head_reach_squared := 0.0
var _collider_rx := 0.0
var _collider_rz := 0.0
var _neck_a := Vector3.ZERO
var _neck_b := Vector3.ZERO
var _neck_radius := 0.0
var _shoulder_a := Vector3.ZERO
var _shoulder_b := Vector3.ZERO
var _shoulder_radius := 0.0
var _torso_a := Vector3.ZERO
var _torso_b := Vector3.ZERO
var _torso_radius := 0.0
var _scalp_triangles: Array[Dictionary] = []
var _scalp_triangle_cdf := PackedFloat32Array()
var _scalp_surface_area := 0.0
var _card_count := 0
var _visible_card_count := 0
var _card_width := 0.009
var _render_fiber_width := 0.00045
var _build_ms := 0.0
var _average_tip_displacement := 0.0
var _peak_tip_displacement := 0.0
var _lod_elapsed := 0.0
var _physics_accumulator := 0.0
var _last_mount_transform := Transform3D.IDENTITY
var _last_mount_velocity := Vector3.ZERO
var _last_mount_angular_velocity := Vector3.ZERO
var _motion_initialized := false
var _pinned_root_segments := MIN_PINNED_ROOT_SEGMENTS


func _create_render_nodes() -> void:
	super._create_render_nodes()
	_hair_instance.visible = false
	_hair_instance.mesh = null
	_gpu_hair_material = ShaderMaterial.new()
	_gpu_hair_material.shader = HAIR_CARD_SHADER
	_gpu_hair_instance = MultiMeshInstance3D.new()
	_gpu_hair_instance.name = "GPUHairCardFollowers"
	_gpu_hair_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_mount.add_child(_gpu_hair_instance)

	# An opaque shell under the locks. Cards can never tile a scalp perfectly, and
	# without this every gap between locks shows skin.
	_scalp_cap_material = ShaderMaterial.new()
	_scalp_cap_material.shader = SCALP_CAP_SHADER
	_scalp_cap_instance = MeshInstance3D.new()
	_scalp_cap_instance.name = "GPUHairScalpCap"
	_scalp_cap_instance.material_override = _scalp_cap_material
	_scalp_cap_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_mount.add_child(_scalp_cap_instance)

	_bake_hair_strip()
	_create_guide_texture()


## Bakes the hair strip every card samples: a few painted locks of real strands,
## with irregular ends so the silhouette does not read as a rectangle. This is
## what a card groom uses instead of modelling individual fibers.
##
## R tint per strand · G cross-strand roundness · B per-strand phase · A coverage
func _bake_hair_strip() -> void:
	var width := STRIP_VARIANT_WIDTH * STRIP_VARIANTS
	var image := Image.create(width, STRIP_HEIGHT, true, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	for variant in STRIP_VARIANTS:
		var origin := variant * STRIP_VARIANT_WIDTH
		for strand in STRIP_STRANDS:
			var seed_value := float(variant * 977 + strand * 31 + 7)
			# Most strands start on the scalp so the root end stays solid and hides
			# the cap seam; a few start late to break up the mass.
			var start_t := 0.0 if _hash01(seed_value * 1.7) < 0.78 else _hash01(seed_value * 5.3) * 0.35
			var end_t := lerpf(0.52, 1.0, pow(_hash01(seed_value * 2.9), 0.7))
			var base_x := lerpf(0.05, 0.95, _hash01(seed_value * 3.7))
			var drift := lerpf(-0.14, 0.14, _hash01(seed_value * 4.1))
			var wobble := lerpf(0.5, 2.4, _hash01(seed_value * 6.7))
			var phase := _hash01(seed_value * 8.9) * TAU
			var root_width := lerpf(0.8, 2.6, _hash01(seed_value * 9.3))
			var tint := _hash01(seed_value * 11.7)
			var strand_phase := _hash01(seed_value * 13.1)
			var row_start := int(start_t * float(STRIP_HEIGHT))
			var row_end := mini(int(end_t * float(STRIP_HEIGHT)), STRIP_HEIGHT - 1)
			for row in range(row_start, row_end + 1):
				var t := float(row) / float(STRIP_HEIGHT - 1)
				var span := maxf(end_t - start_t, 0.001)
				var local_t := clampf((t - start_t) / span, 0.0, 1.0)
				var centre := base_x + drift * local_t + sin(local_t * wobble * TAU + phase) * 0.035 * local_t
				var half := root_width * 0.5 * lerpf(1.0, 0.35, pow(local_t, 0.8))
				# Fade both ends so no strand terminates in a hard chopped line.
				var ends := smoothstep(0.0, 0.04, local_t) * (1.0 - smoothstep(0.72, 1.0, local_t))
				if ends <= 0.001:
					continue
				var centre_px := centre * float(STRIP_VARIANT_WIDTH)
				var first := maxi(int(floor(centre_px - half - 1.0)), 0)
				var last := mini(int(ceil(centre_px + half + 1.0)), STRIP_VARIANT_WIDTH - 1)
				for column in range(first, last + 1):
					var distance := absf(float(column) + 0.5 - centre_px)
					var coverage := clampf(1.0 - (distance - half), 0.0, 1.0) * ends
					if coverage <= 0.004:
						continue
					var existing := image.get_pixel(origin + column, row)
					if coverage <= existing.a:
						continue
					# Bright along the strand's spine, darker at its edge, so each
					# strand reads as a round fiber rather than a flat band.
					var roundness := 1.0 - clampf(distance / maxf(half, 0.001), 0.0, 1.0)
					image.set_pixel(origin + column, row, Color(tint, roundness, strand_phase, coverage))
	image.generate_mipmaps()
	_hair_strip_texture = ImageTexture.create_from_image(image)
	_gpu_hair_material.set_shader_parameter("hair_strip", _hair_strip_texture)
	_gpu_hair_material.set_shader_parameter("strip_variants", float(STRIP_VARIANTS))


func _apply_material_settings() -> void:
	super._apply_material_settings()
	if _gpu_hair_material == null:
		return
	var fallback := Color(hair_settings.get("color", Color("352218")))
	var root_color := Color(hair_settings.get("root_color", fallback))
	var tip_color := Color(hair_settings.get("tip_color", root_color.lightened(0.18)))
	_gpu_hair_material.set_shader_parameter("root_color", root_color)
	_gpu_hair_material.set_shader_parameter("tip_color", tip_color)
	_gpu_hair_material.set_shader_parameter("gradient_bias", clampf(float(hair_settings.get("gradient_bias", 1.35)), 0.2, 4.0))
	_gpu_hair_material.set_shader_parameter("curl", clampf(float(hair_settings.get("curl", 0.10)), 0.0, 1.0))
	_gpu_hair_material.set_shader_parameter("sheen", clampf(float(hair_settings.get("sheen", 0.55)), 0.0, 1.0))
	_gpu_hair_material.set_shader_parameter("sheen_roughness", clampf(float(hair_settings.get("sheen_roughness", 0.62)), 0.05, 1.0))
	if _guide_texture != null:
		_gpu_hair_material.set_shader_parameter("guide_motion_texture", _guide_texture)
	if _hair_strip_texture != null:
		_gpu_hair_material.set_shader_parameter("hair_strip", _hair_strip_texture)
		_gpu_hair_material.set_shader_parameter("strip_variants", float(STRIP_VARIANTS))
	if _scalp_cap_material != null:
		# The cap reads as roots in shadow, so it sits just below the root colour.
		_scalp_cap_material.set_shader_parameter("root_color", root_color.darkened(0.28))
		_scalp_cap_material.set_shader_parameter("sheen", clampf(float(hair_settings.get("sheen", 0.55)), 0.0, 1.0))
		_scalp_cap_material.set_shader_parameter("sheen_roughness", clampf(float(hair_settings.get("sheen_roughness", 0.52)), 0.05, 1.0))
		if _hair_strip_texture != null:
			_scalp_cap_material.set_shader_parameter("hair_strip", _hair_strip_texture)
			_scalp_cap_material.set_shader_parameter("strip_variants", float(STRIP_VARIANTS))


func rebuild_hair() -> void:
	if _gpu_hair_instance == null or _mount == null or _head.is_empty():
		return
	var enabled := bool(hair_settings.get("enabled", true))
	_gpu_hair_instance.visible = enabled
	if not enabled:
		_gpu_hair_instance.multimesh = null
		_card_count = 0
		_visible_card_count = 0
		if _scalp_cap_instance != null:
			_scalp_cap_instance.visible = false
		return

	var started := Time.get_ticks_usec()
	_prepare_scalp_surface()
	_build_head_field()
	if _scalp_triangles.is_empty():
		push_warning("GPU scalp groom: body mesh exposed no usable scalp triangles")
		_gpu_hair_instance.multimesh = null
		return

	var density := clampf(float(hair_settings.get("density", 0.72)), 0.05, 1.0)
	var base_length := clampf(float(hair_settings.get("length", 0.09)), 0.004, 0.55)
	var requested_fiber_width := clampf(float(hair_settings.get("width", 0.00032)), 0.00004, 0.0012)
	var tip_thickness := clampf(float(hair_settings.get("tip_thickness", 0.08)), 0.01, 0.65)
	var root_lift := clampf(float(hair_settings.get("root_lift", 0.0012)), 0.0, 0.010)
	var style := int(hair_settings.get("style", 0))
	var long_hair := smoothstep(0.07, 0.18, base_length)
	_pinned_root_segments = clampi(
		MIN_PINNED_ROOT_SEGMENTS + roundi(long_hair * float(MAX_PINNED_ROOT_SEGMENTS - MIN_PINNED_ROOT_SEGMENTS)),
		MIN_PINNED_ROOT_SEGMENTS,
		MAX_PINNED_ROOT_SEGMENTS
	)
	# Card grooms use locks tens of millimetres wide, each carrying a painted strip
	# of strands. The old 4 mm cards were narrower than the highlight they were
	# meant to catch, which is why the sheen aliased into sparkle instead of
	# forming a band, and why 1700 of them still failed to cover the scalp.
	_card_width = clampf(base_length * lerpf(0.22, 0.12, long_hair), 0.010, 0.032)
	# Still used as the collision margin for the guide chains.
	_render_fiber_width = clampf(requested_fiber_width * 1.35, 0.00018, 0.00072)
	var volume := clampf(float(hair_settings.get("volume", 0.38)), 0.0, 1.0)
	var target_cards := clampi(int(round(lerpf(200.0, 560.0, density))), 140, 640)
	# Production grooms build the scalp coverage from a dense layer of short, WIDE
	# cards and only then add thinner cards on top to break the silhouette. One
	# uniform layer leaves the base mesh showing wherever the locks separate.
	var base_cards := int(round(float(target_cards) * 0.62))
	var total_cards := target_cards + base_cards
	_build_guide_rest_state(base_length, root_lift, style)

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_custom_data = true
	multimesh.mesh = _build_card_mesh(tip_thickness, base_length)
	multimesh.instance_count = total_cards
	multimesh.visible_instance_count = total_cards
	var produced := 0
	# Base layer first so distance LOD trims the silhouette cards, never the
	# coverage the scalp depends on.
	for layer in 2:
		var is_base := layer == 0
		var wanted := base_cards if is_base else target_cards
		var made := 0
		var salt := 7919 if is_base else 13
		for attempt in wanted * 8:
			if made >= wanted:
				break
			var sample := _sample_scalp(salt + attempt * 3)
			if sample.is_empty():
				continue
			var root_world: Vector3 = sample["position"]
			var normal_world: Vector3 = sample["normal"]
			var region := _scalp_region(root_world)
			var seed_value := float(salt + attempt)
			if not _accepts_root(region, seed_value):
				continue
			var length_scale := 0.0
			var width_scale := 0.0
			if is_base:
				# Short enough to stay against the scalp, wide enough that a handful
				# of them close the gaps the long locks leave.
				length_scale = lerpf(0.20, 0.34, _hash01(seed_value * 19.73 + 4.1))
				width_scale = lerpf(1.35, 1.95, _hash01(seed_value * 7.91 + 2.7))
			else:
				length_scale = lerpf(0.82, 1.18, _hash01(seed_value * 19.73 + 4.1)) * _root_length_scale(region)
				width_scale = lerpf(0.72, 1.16, _hash01(seed_value * 7.91 + 2.7))
			width_scale *= _part_narrowing(region)
			var root_local := _mount.to_local(root_world + normal_world * root_lift)
			var guide_index := _nearest_guide(root_local)
			# A follower must use the same complete frame as its guide. Applying a
			# guide-space bend through an independently estimated follower frame is
			# almost harmless for short hair, but becomes a radial fan at long lengths.
			var flow_local := _guide_flow_axes[guide_index]
			var width_axis := _guide_width_axes[guide_index]
			var card_normal := _guide_normal_axes[guide_index]
			var follower_basis := Basis(
				width_axis * (_card_width * width_scale),
				flow_local * (base_length * length_scale),
				card_normal * (_card_width * width_scale)
			)
			multimesh.set_instance_transform(produced, Transform3D(follower_basis, root_local))
			# The shader needs both scales to read the guide correctly: a card that
			# covers a third of the guide must sample a third of it, and undo the
			# basis scaling the offset picks up on the way through.
			multimesh.set_instance_custom_data(produced, Color(
				float(clampi(guide_index, 0, GUIDE_COUNT - 1)) / float(GUIDE_COUNT - 1),
				length_scale,
				0.5 + signf(region.x) * 0.5,
				width_scale
			))
			produced += 1
			made += 1

	var target_cards_total := total_cards
	if produced < target_cards_total:
		multimesh.visible_instance_count = produced
	_card_count = produced
	_visible_card_count = produced
	var reach := base_length * 1.35 + maxf(float(_head["rx"]), float(_head["rz"]))
	multimesh.custom_aabb = AABB(
		Vector3(-reach, -reach, -reach) + _mount.to_local(Vector3(_head["center"])),
		Vector3.ONE * reach * 2.0
	)
	_gpu_hair_instance.multimesh = multimesh
	_build_scalp_cap(root_lift, style)
	_gpu_hair_material.set_shader_parameter("guide_count", float(GUIDE_COUNT))
	_gpu_hair_material.set_shader_parameter("guide_segment_count", float(GUIDE_SEGMENTS))
	_apply_material_settings()
	_reset_guide_motion()
	_build_ms = float(Time.get_ticks_usec() - started) / 1000.0


func _process(delta: float) -> void:
	super._process(delta)
	if _gpu_hair_instance == null or not _gpu_hair_instance.visible or _card_count <= 0:
		return
	if _gpu_hair_material != null and _mount != null:
		_gpu_hair_material.set_shader_parameter("world_to_mount", Transform3D(_mount.global_transform.affine_inverse()))
	_update_guide_motion(delta)
	_lod_elapsed += delta
	if _lod_elapsed >= LOD_CHECK_INTERVAL:
		_lod_elapsed = 0.0
		_update_card_lod()


## One hair card: a tapered lock with a rounded cross-section, 64 triangles.
##
## The old card modelled nine ribbons of individual fibers, which cost 288
## triangles and still rendered as a spray because each fiber was sub-pixel. A
## card groom puts that detail in the strip texture instead, so the geometry only
## has to carry the lock's shape and its curvature.
func _build_card_mesh(tip_thickness: float, _base_length: float) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	for row in CARD_SEGMENTS + 1:
		var t := float(row) / float(CARD_SEGMENTS)
		# Locks are widest just off the scalp and taper to a point.
		var half := 0.5 * lerpf(1.0, tip_thickness, pow(t, 0.62)) * lerpf(0.88, 1.0, sin(t * PI * 0.6))
		var bulge := half * 0.34
		for column in CARD_COLUMNS:
			var u := float(column) / float(CARD_COLUMNS - 1)
			var offset := (u - 0.5) * 2.0
			var depth := bulge * (1.0 - offset * offset)
			vertices.append(Vector3(offset * half, t, depth))
			# Surface normal of the curved cross-section, so the lock shades round
			# instead of like a flat sheet of paper.
			normals.append(Vector3(bulge * 8.0 * (u - 0.5), 0.0, 2.0 * half).normalized())
			uvs.append(Vector2(u, t))
	for row in CARD_SEGMENTS:
		for column in CARD_COLUMNS - 1:
			var a := row * CARD_COLUMNS + column
			var b := a + 1
			var c := a + CARD_COLUMNS
			var d := c + 1
			indices.append_array([a, c, b, b, c, d])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, _gpu_hair_material)
	return mesh


func _create_guide_texture() -> void:
	_guide_positions.resize(GUIDE_COUNT * GUIDE_SEGMENTS)
	_guide_previous_positions.resize(GUIDE_COUNT * GUIDE_SEGMENTS)
	_guide_rest_positions.resize(GUIDE_COUNT * GUIDE_SEGMENTS)
	_guide_reference_positions.resize(GUIDE_COUNT * GUIDE_SEGMENTS)
	_guide_roots.resize(GUIDE_COUNT)
	_guide_segment_lengths.resize(GUIDE_COUNT)
	_guide_width_axes.resize(GUIDE_COUNT)
	_guide_flow_axes.resize(GUIDE_COUNT)
	_guide_normal_axes.resize(GUIDE_COUNT)
	_guide_lateral_bias.resize(GUIDE_COUNT)
	_guide_image = Image.create(GUIDE_COUNT, GUIDE_SEGMENTS, false, Image.FORMAT_RGBAF)
	_guide_image.fill(Color(0.0, 0.0, 0.0, 1.0))
	_guide_texture = ImageTexture.create_from_image(_guide_image)
	_gpu_hair_material.set_shader_parameter("guide_motion_texture", _guide_texture)


func _reset_guide_motion() -> void:
	for index in _guide_positions.size():
		_guide_positions[index] = _guide_rest_positions[index]
		_guide_previous_positions[index] = _guide_rest_positions[index]
	if _guide_image != null:
		_guide_image.fill(Color(0.0, 0.0, 0.0, 1.0))
		_guide_texture.update(_guide_image)
	_physics_accumulator = 0.0
	_last_mount_angular_velocity = Vector3.ZERO
	_motion_initialized = false


func _update_guide_motion(delta: float) -> void:
	if _guide_image == null or _guide_texture == null or _mount == null or delta <= 0.0:
		return
	var mount_transform := _mount.global_transform
	if not _motion_initialized or delta > 0.12:
		_last_mount_transform = mount_transform
		_last_mount_velocity = Vector3.ZERO
		_last_mount_angular_velocity = Vector3.ZERO
		_motion_initialized = true
		_upload_guide_motion()
		return
	var world_velocity := (mount_transform.origin - _last_mount_transform.origin) / maxf(delta, 0.0001)
	var world_acceleration := (world_velocity - _last_mount_velocity) / maxf(delta, 0.0001)
	var local_acceleration := mount_transform.basis.inverse() * world_acceleration.limit_length(35.0)
	var rotation_signal := _last_mount_transform.basis.x.cross(mount_transform.basis.x) \
		+ _last_mount_transform.basis.y.cross(mount_transform.basis.y) \
		+ _last_mount_transform.basis.z.cross(mount_transform.basis.z)
	var angular_velocity := mount_transform.basis.inverse() * rotation_signal / maxf(delta, 0.0001)
	angular_velocity = angular_velocity.limit_length(12.0)
	var local_angular_acceleration := (angular_velocity - _last_mount_angular_velocity) / maxf(delta, 0.0001)
	local_angular_acceleration = local_angular_acceleration.limit_length(45.0)
	_last_mount_transform = mount_transform
	_last_mount_velocity = world_velocity
	_last_mount_angular_velocity = angular_velocity

	var physics_enabled := bool(hair_settings.get("physics_enabled", true))
	if not physics_enabled:
		for index in _guide_positions.size():
			_guide_positions[index] = _guide_rest_positions[index]
			_guide_previous_positions[index] = _guide_rest_positions[index]
		_upload_guide_motion()
		return

	_physics_accumulator = minf(_physics_accumulator + delta, FIXED_PHYSICS_STEP * 3.0)
	while _physics_accumulator >= FIXED_PHYSICS_STEP:
		_simulate_guides(FIXED_PHYSICS_STEP, local_acceleration, angular_velocity, local_angular_acceleration)
		_physics_accumulator -= FIXED_PHYSICS_STEP
	_upload_guide_motion()


func _simulate_guides(step: float, local_acceleration: Vector3, angular_velocity: Vector3, local_angular_acceleration: Vector3) -> void:
	var stiffness := clampf(float(hair_settings.get("physics_stiffness", 0.68)), 0.0, 1.0)
	var damping := clampf(float(hair_settings.get("physics_damping", 0.92)), 0.70, 0.995)
	var gravity_amount := clampf(float(hair_settings.get("physics_gravity", 0.55)), 0.0, 1.5)
	var wind_response := clampf(float(hair_settings.get("wind_response", 0.12)), 0.0, 1.0)
	var down_local := (_mount.global_basis.inverse() * Vector3.DOWN).normalized()
	var gravity_acceleration := down_local * 9.81 * gravity_amount
	var velocity_decay := pow(damping, step * 60.0)
	var now := Time.get_ticks_msec() * 0.001

	for guide in GUIDE_COUNT:
		var root_index := guide * GUIDE_SEGMENTS
		var root := _guide_rest_positions[root_index]
		_guide_positions[root_index] = root
		_guide_previous_positions[root_index] = root
		var phase := float(guide) * 2.399963
		var wind := Vector3(
			sin(now * 0.83 + phase),
			0.18 * sin(now * 0.51 + phase * 0.37),
			cos(now * 0.71 + phase * 0.61)
		) * wind_response * 1.8
		for segment in range(_pinned_root_segments, GUIDE_SEGMENTS):
			var index := root_index + segment
			var guide_position := _guide_positions[index]
			var previous := _guide_previous_positions[index]
			var velocity := (guide_position - previous) * velocity_decay
			_guide_previous_positions[index] = guide_position
			var relative := guide_position - root
			var inertial := -local_acceleration * 0.72
			inertial -= local_angular_acceleration.cross(relative) * 0.88
			inertial -= angular_velocity.cross(angular_velocity.cross(relative)) * 1.10
			inertial = inertial.limit_length(28.0)
			var free_weight := pow(float(segment) / float(GUIDE_SEGMENTS - 1), 1.65)
			guide_position += velocity + (gravity_acceleration + inertial + wind) * step * step * free_weight
			_guide_positions[index] = guide_position

	# Length and collision constraints carry structural stability. Style
	# preservation must stay light or five iterations erase all visible motion.
	var shape_strength := lerpf(0.0015, 0.022, stiffness)
	# Length constraints alone allow the first free joint to fold into the head;
	# radial collision then sends different guides along different escape paths.
	# Preserve only the first few authored bends strongly. The shaft and tip stay
	# free for secondary motion.
	var root_bend_strength := lerpf(0.075, 0.16, stiffness)
	for iteration in SOLVER_ITERATIONS:
		# Collision only needs to hold on the iterations that produce the final
		# positions; running it on all five costs 5x for no visible difference.
		var collide := iteration >= SOLVER_ITERATIONS - 2
		for guide in GUIDE_COUNT:
			var root_index := guide * GUIDE_SEGMENTS
			for pinned_segment in _pinned_root_segments:
				var pinned_index := root_index + pinned_segment
				_guide_positions[pinned_index] = _guide_rest_positions[pinned_index]
				_guide_previous_positions[pinned_index] = _guide_rest_positions[pinned_index]
			var segment_length := _guide_segment_lengths[guide]
			for segment in range(1, GUIDE_SEGMENTS):
				var previous_index := root_index + segment - 1
				var index := root_index + segment
				if segment < _pinned_root_segments:
					continue
				var a := _guide_positions[previous_index]
				var b := _guide_positions[index]
				var difference := b - a
				var distance := difference.length()
				if distance > 0.000001:
					var correction := difference * ((distance - segment_length) / distance)
					if segment == 1:
						b -= correction
					else:
						a += correction * 0.5
						b -= correction * 0.5
						_guide_positions[previous_index] = a
				var rest := _guide_rest_positions[index]
				var root_weight := pow(float(segment) / float(GUIDE_SEGMENTS - 1), 1.15)
				b = b.lerp(rest, shape_strength * (1.0 - root_weight * 0.86))
				var root_bend_weight := 1.0 - smoothstep(1.0, 5.5, float(segment))
				b = b.lerp(rest, root_bend_strength * root_bend_weight)
				var projected := b
				if collide:
					var resolved := _project_character_collision(b, _render_fiber_width * 2.0, _guide_lateral_bias[guide])
					# Same cap as the rest builder: a collision may push a particle by
					# at most one segment, so no frame can fling a strand off the head.
					projected = b + (resolved - b).limit_length(segment_length)
					if projected.distance_squared_to(b) > 0.00000001:
						# Collision projection must not be read as free velocity on the
						# next Verlet step, or long styles gain energy.
						_guide_previous_positions[index] = _guide_previous_positions[index].lerp(projected, 0.78)
				_guide_positions[index] = projected
			for pinned_segment in _pinned_root_segments:
				_guide_positions[root_index + pinned_segment] = _guide_rest_positions[root_index + pinned_segment]


func _upload_guide_motion() -> void:
	var tip_displacement_sum := 0.0
	var tip_displacement_peak := 0.0
	for guide in GUIDE_COUNT:
		for segment in GUIDE_SEGMENTS:
			var index := guide * GUIDE_SEGMENTS + segment
			var local_motion := _guide_positions[index] - _guide_reference_positions[index]
			var normalized_motion := Vector3(
				local_motion.dot(_guide_width_axes[guide]) / maxf(_card_width, 0.0001),
				local_motion.dot(_guide_flow_axes[guide]) / maxf(_guide_segment_lengths[guide] * float(GUIDE_SEGMENTS - 1), 0.0001),
				local_motion.dot(_guide_normal_axes[guide]) / maxf(_card_width, 0.0001)
			)
			_guide_image.set_pixel(guide, segment, Color(normalized_motion.x, normalized_motion.y, normalized_motion.z, 1.0))
		var tip_index := guide * GUIDE_SEGMENTS + GUIDE_SEGMENTS - 1
		var tip_displacement := _guide_positions[tip_index].distance_to(_guide_rest_positions[tip_index])
		tip_displacement_sum += tip_displacement
		tip_displacement_peak = maxf(tip_displacement_peak, tip_displacement)
	_average_tip_displacement = tip_displacement_sum / float(GUIDE_COUNT)
	_peak_tip_displacement = tip_displacement_peak
	_guide_texture.update(_guide_image)


func _build_guide_rest_state(base_length: float, root_lift: float, style: int) -> void:
	var accepted := 0
	var attempt := 0
	while accepted < GUIDE_COUNT and attempt < GUIDE_COUNT * 160:
		var sample := _sample_scalp(100003 + attempt * 13)
		attempt += 1
		if sample.is_empty():
			continue
		var root_world: Vector3 = sample["position"]
		var normal_world: Vector3 = sample["normal"]
		var region := _scalp_region(root_world)
		if not _accepts_root(region, float(attempt) * 3.1):
			continue
		var root_local := _mount.to_local(root_world + normal_world * root_lift)
		var normal_local := (_mount.global_basis.inverse() * normal_world).normalized()
		var flow_world := _growth_flow(normal_world, region, style)
		var flow_local := (_mount.global_basis.inverse() * flow_world).normalized()
		_set_guide_rest(accepted, root_local, normal_local, flow_local, base_length, style, region)
		accepted += 1

	if accepted == 0:
		var center_local := _mount.to_local(Vector3(_head["center"]))
		var root := center_local + Vector3.UP * float(_head["ry"])
		_set_guide_rest(0, root, Vector3.UP, Vector3(0.0, 0.15, -_front_sign).normalized(), base_length, style)
		accepted = 1
	# `accepted % accepted` is always 0, so a scalp that yields fewer than
	# GUIDE_COUNT roots used to clone guide 0 into every remaining slot and comb a
	# whole region in one direction.
	var built := maxi(accepted, 1)
	while accepted < GUIDE_COUNT:
		var source := accepted % built
		for segment in GUIDE_SEGMENTS:
			var target_index := accepted * GUIDE_SEGMENTS + segment
			var source_index := source * GUIDE_SEGMENTS + segment
			_guide_rest_positions[target_index] = _guide_rest_positions[source_index]
			_guide_reference_positions[target_index] = _guide_reference_positions[source_index]
		_guide_roots[accepted] = _guide_roots[source]
		_guide_segment_lengths[accepted] = _guide_segment_lengths[source]
		_guide_width_axes[accepted] = _guide_width_axes[source]
		_guide_flow_axes[accepted] = _guide_flow_axes[source]
		_guide_normal_axes[accepted] = _guide_normal_axes[source]
		_guide_lateral_bias[accepted] = _guide_lateral_bias[source]
		accepted += 1


func _set_guide_rest(guide: int, root: Vector3, normal: Vector3, flow: Vector3, base_length: float, style: int, region: Vector3 = Vector3.ZERO) -> void:
	var segment_length := base_length / float(GUIDE_SEGMENTS - 1)
	var down_local := (_mount.global_basis.inverse() * Vector3.DOWN).normalized()
	var fall_control := clampf(float(hair_settings.get("gravity", 0.38)), 0.0, 1.0)
	var length_fall := smoothstep(0.035, 0.18, base_length)
	var fall_strength := clampf(fall_control * 0.62 + length_fall * 0.76, 0.0, 1.0)
	if style == 4:
		fall_strength *= 0.12
	var width_axis := flow.cross(normal)
	if width_axis.length_squared() < 0.0001:
		width_axis = flow.cross(Vector3.RIGHT)
	width_axis = width_axis.normalized()
	var guide_normal := width_axis.cross(flow).normalized()
	var curl_amount := clampf(float(hair_settings.get("curl", 0.10)), 0.0, 1.0)
	var long_hair := smoothstep(0.07, 0.18, base_length)
	var volume := clampf(float(hair_settings.get("volume", 0.38)), 0.0, 1.0)
	# A constant normal term is a radial force — every root leans away from the
	# skull and the groom fans. A bump that rises and returns is not: the strand
	# lifts off the scalp and comes back down, which is what reads as volume.
	# Without it every card lies flat on the skull and the hairline looks shaved.
	var root_normal_volume := lerpf(0.16, 0.10, long_hair) * (0.25 + volume * 1.75)
	var phase := float(guide) * 2.399963
	var root_index := guide * GUIDE_SEGMENTS
	# The side of the head this guide grows from, so that when its strand reaches
	# the face it parts the same way every frame instead of chattering across the
	# centre line. Scaled small: it only breaks ties near the sagittal plane.
	var head_center := _mount.to_local(Vector3(_head["center"]))
	var root_relative := root - head_center
	var lateral_bias := root_relative.x * 0.35
	if int(hair_settings.get("part_style", 0)) == 1:
		# Under a part the collider must agree with the comb, or a strand combed
		# left gets pushed right the moment it touches the face.
		lateral_bias = _part_side(region) * maxf(absf(lateral_bias), 0.004)
	elif absf(lateral_bias) < 0.0005:
		lateral_bias = 0.0005 if fmod(float(guide), 2.0) < 1.0 else -0.0005
	_guide_roots[guide] = root
	_guide_segment_lengths[guide] = segment_length
	_guide_width_axes[guide] = width_axis
	_guide_flow_axes[guide] = flow
	_guide_normal_axes[guide] = guide_normal
	_guide_lateral_bias[guide] = lateral_bias
	_guide_rest_positions[root_index] = root
	_guide_reference_positions[root_index] = root
	var cursor := root
	var adhered_fraction := clampf(0.032 / maxf(base_length, 0.001), 0.08, 0.42)
	var fall_start_fraction := clampf(0.012 / maxf(base_length, 0.001), 0.025, 0.22)
	var fall_end_fraction := clampf(0.068 / maxf(base_length, 0.001), 0.16, 0.84)
	# A strand rooted at the forehead has to travel back over the crown before
	# gravity takes it, or a long style hangs a curtain down the face. Delaying
	# gravity alone is not enough — the strand then leaves the curved skull on a
	# straight tangent and stands up. What actually keeps combed-back hair on the
	# head is that it stays in contact with it, so extend the adhered run instead
	# and delay the fall only mildly.
	var front_weight := smoothstep(-0.05, 0.55, region.z)
	var swept_back := front_weight * long_hair
	adhered_fraction = lerpf(adhered_fraction, 0.55, swept_back)
	var fall_delay := lerpf(1.0, 2.0, swept_back)
	fall_start_fraction = minf(fall_start_fraction * fall_delay, 0.34)
	fall_end_fraction = minf(maxf(fall_end_fraction * fall_delay, fall_start_fraction + 0.18), 0.90)
	for segment in range(1, GUIDE_SEGMENTS):
		var t := float(segment) / float(GUIDE_SEGMENTS - 1)
		var fall_t := smoothstep(fall_start_fraction, fall_end_fraction, t) * fall_strength
		var tangent := flow.lerp(down_local, fall_t).normalized()
		tangent += width_axis * sin(t * TAU * (0.65 + curl_amount * 1.4) + phase) * curl_amount * 0.24
		# Peaks around a quarter of the way up, gone by 45%.
		tangent += normal * sin(clampf(t / 0.45, 0.0, 1.0) * PI) * root_normal_volume
		cursor += tangent.normalized() * segment_length
		# Gluing the first third of every strand to the scalp is what flattens the
		# front. Keep enough adhesion to hold the roots down, not the whole shaft.
		var adhesion := (1.0 - smoothstep(0.0, adhered_fraction, t)) * lerpf(1.0, 0.45, volume)
		var resolved := cursor
		# Adhesion snaps to the head surface with no shield and no deflection, so
		# over the face it would glue the strand to the skin and override the
		# collider entirely. Hand those segments to the collider instead.
		var over_face := _face_weight((cursor - _head_field_center).normalized())
		var surface_pull := adhesion * (1.0 - over_face)
		if surface_pull > 0.0:
			resolved = cursor.lerp(_scalp_surface_point(cursor, 0.0025), surface_pull * 0.94)
			resolved = _project_head_field(resolved, _render_fiber_width * 2.0, lateral_bias, smoothstep(0.04, 0.28, t))
		else:
			# The adhered root already follows the scalp chart. Sending it through
			# the generic radial ellipsoid collision immediately afterwards rotates
			# it away from the requested comb direction.
			resolved = _project_character_collision(cursor, _render_fiber_width * 2.0, lateral_bias)
		# A surface correction larger than the step that produced it is not a
		# collision response, it is a teleport, and it is what turns a comb field
		# into a starburst. Cap it so the chain can never outrun its own arc.
		cursor = cursor + (resolved - cursor).limit_length(segment_length)
		var index := root_index + segment
		_guide_rest_positions[index] = cursor
		_guide_reference_positions[index] = root + flow * segment_length * float(segment)


func _nearest_guide(root: Vector3) -> int:
	var best := 0
	var best_distance := INF
	for guide in GUIDE_COUNT:
		var distance := root.distance_squared_to(_guide_roots[guide])
		if distance < best_distance:
			best_distance = distance
			best = guide
	return best


## Precomputes every collider once per rebuild.
##
## These are all constants of the character, but they used to be rebuilt from
## dictionary lookups and a to_local() on every call — and the solver makes about
## 17,000 calls a frame, which cost more than everything else in the groom put
## together.
func _prepare_colliders() -> void:
	var center := _head_field_center
	_collider_rx = float(_head["rx"])
	_collider_rz = float(_head["rz"])
	var ry := float(_head["ry"])
	var height := _character_height
	var reach := 0.0
	for radius in _head_field:
		reach = maxf(reach, radius)
	# Widest the head collider can ever be: the field plus a full face shield.
	_head_reach_squared = pow(reach + _collider_rz * 0.30 + 0.02, 2.0)
	_neck_a = center + Vector3.DOWN * ry * 0.72
	_neck_b = center + Vector3.DOWN * ry * 1.82
	_neck_radius = _collider_rx * 0.52
	var shoulder_y := center.y - ry * 2.02
	var shoulder_z := center.z - _front_sign * _collider_rz * 0.10
	_shoulder_a = Vector3(center.x - height * 0.155, shoulder_y, shoulder_z)
	_shoulder_b = Vector3(center.x + height * 0.155, shoulder_y, shoulder_z)
	_shoulder_radius = height * 0.064
	# `_character_bottom` is a WORLD height, but every other quantity here is in
	# the mount's local space. Using it raw put the torso capsule 0.73 m ABOVE the
	# head instead of 0.85 m below it, wrapping a 0.20 m radius cylinder around a
	# 0.10 m skull and flinging every strand out onto its surface.
	var head_center_world: Vector3 = _head["center"]
	var floor_local_y := center.y - (head_center_world.y - _character_bottom)
	_torso_a = Vector3(center.x, shoulder_y - height * 0.02, shoulder_z)
	_torso_b = Vector3(center.x, minf(floor_local_y + height * 0.42, shoulder_y - height * 0.02), shoulder_z)
	_torso_radius = height * 0.115


## Body collision for one strand point, in the head mount's local space.
##
## The head is the baked mesh field; neck, shoulders and torso stay primitives
## because nothing about them needs to be face-accurate.
func _project_character_collision(point: Vector3, margin: float, lateral_bias: float = 0.0) -> Vector3:
	var result := point
	# Most free segments on a long style hang well clear of the skull. Rejecting
	# them on one squared length keeps the field's atan2/acos off the hot path.
	if (point - _head_field_center).length_squared() < _head_reach_squared:
		result = _project_head_field(point, margin, lateral_bias)
	result = _project_capsule(result, _neck_a, _neck_b, _neck_radius + margin)
	result = _project_capsule(result, _shoulder_a, _shoulder_b, _shoulder_radius + margin)
	result = _project_capsule(result, _torso_a, _torso_b, _torso_radius + margin)
	return result


## Puts a point exactly on the head surface. Used to hold the adhered part of a
## strand against the scalp; against the mesh field this is now exact, so it no
## longer teleports a root off the head the way the ellipsoid fit did.
func _scalp_surface_point(point: Vector3, margin: float) -> Vector3:
	if _head_field.is_empty():
		return point
	var relative := point - _head_field_center
	var distance := relative.length()
	if distance < 0.0001:
		return point
	var direction := relative / distance
	return _head_field_center + direction * (_sample_head_field(direction) + margin)


func _build_head_field() -> void:
	if not _head_field.is_empty() or _scalp_triangles.is_empty() or _mount == null:
		return
	var cells := HEAD_FIELD_AZIMUTH * HEAD_FIELD_ELEVATION
	_head_field.resize(cells)
	for index in cells:
		_head_field[index] = 0.0
	_head_field_center = _mount.to_local(Vector3(_head["center"]))
	# Vertices, edge midpoints and centroid: enough that a triangle never skips a
	# cell it spans.
	var barycentric := [
		Vector3(1.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0), Vector3(0.0, 0.0, 1.0),
		Vector3(0.5, 0.5, 0.0), Vector3(0.0, 0.5, 0.5), Vector3(0.5, 0.0, 0.5),
		Vector3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)
	]
	for triangle in _scalp_triangles:
		var a := _mount.to_local(triangle["p0"])
		var b := _mount.to_local(triangle["p1"])
		var c := _mount.to_local(triangle["p2"])
		for weight: Vector3 in barycentric:
			var sample: Vector3 = a * weight.x + b * weight.y + c * weight.z
			var relative := sample - _head_field_center
			var radius := relative.length()
			if radius < 0.0001:
				continue
			var index := _head_field_index(relative / radius)
			if radius > _head_field[index]:
				_head_field[index] = radius
	_fill_head_field()
	_upload_head_field()
	_prepare_colliders()


## The guides are 96 chains; the render is hundreds of cards, each tracing its own
## path from its own root. Colliding only the guides therefore leaves the cards
## free to sweep across the face, which is exactly what they did. Hand the same
## field to the vertex shader so every card collides.
func _upload_head_field() -> void:
	var image := Image.create(HEAD_FIELD_AZIMUTH, HEAD_FIELD_ELEVATION, false, Image.FORMAT_RF)
	for row in HEAD_FIELD_ELEVATION:
		for column in HEAD_FIELD_AZIMUTH:
			var radius := _head_field[row * HEAD_FIELD_AZIMUTH + column]
			image.set_pixel(column, row, Color(radius, 0.0, 0.0, 1.0))
	_head_field_texture = ImageTexture.create_from_image(image)
	if _gpu_hair_material != null:
		_gpu_hair_material.set_shader_parameter("head_field_texture", _head_field_texture)
		_gpu_hair_material.set_shader_parameter("head_field_center", _head_field_center)
		_gpu_hair_material.set_shader_parameter("head_field_margin", _render_fiber_width * 2.0)
		_gpu_hair_material.set_shader_parameter("head_face_shield", float(_head["rz"]) * 0.30)
		_gpu_hair_material.set_shader_parameter("head_lateral_push", float(_head["rx"]) * 0.60)
		_gpu_hair_material.set_shader_parameter("front_sign", _front_sign)


func _head_field_index(direction: Vector3) -> int:
	var azimuth := (atan2(direction.x, direction.z) + PI) / TAU
	var elevation := acos(clampf(direction.y, -1.0, 1.0)) / PI
	var column := clampi(int(azimuth * float(HEAD_FIELD_AZIMUTH)), 0, HEAD_FIELD_AZIMUTH - 1)
	var row := clampi(int(elevation * float(HEAD_FIELD_ELEVATION)), 0, HEAD_FIELD_ELEVATION - 1)
	return row * HEAD_FIELD_AZIMUTH + column


## A cell no triangle reached would read as radius zero and swallow any strand
## passing over it, so grow the neighbours into it.
func _fill_head_field() -> void:
	for _iteration in 8:
		var holes := 0
		var previous := _head_field.duplicate()
		for row in HEAD_FIELD_ELEVATION:
			for column in HEAD_FIELD_AZIMUTH:
				var index := row * HEAD_FIELD_AZIMUTH + column
				if previous[index] > 0.0:
					continue
				var total := 0.0
				var count := 0
				for row_offset: int in [-1, 0, 1]:
					var neighbour_row := row + row_offset
					if neighbour_row < 0 or neighbour_row >= HEAD_FIELD_ELEVATION:
						continue
					for column_offset: int in [-1, 0, 1]:
						var neighbour_column := wrapi(column + column_offset, 0, HEAD_FIELD_AZIMUTH)
						var value := previous[neighbour_row * HEAD_FIELD_AZIMUTH + neighbour_column]
						if value > 0.0:
							total += value
							count += 1
				if count > 0:
					_head_field[index] = total / float(count)
				else:
					holes += 1
		if holes == 0:
			break
	# Any direction the head mesh never covers falls back well inside the
	# ellipsoid, so an empty cell can never push a strand outward.
	var radii := Vector3(float(_head["rx"]), float(_head["ry"]), float(_head["rz"]))
	for row in HEAD_FIELD_ELEVATION:
		var elevation := (float(row) + 0.5) / float(HEAD_FIELD_ELEVATION) * PI
		for column in HEAD_FIELD_AZIMUTH:
			var index := row * HEAD_FIELD_AZIMUTH + column
			if _head_field[index] > 0.0:
				continue
			var azimuth := (float(column) + 0.5) / float(HEAD_FIELD_AZIMUTH) * TAU - PI
			var direction := Vector3(sin(azimuth) * sin(elevation), cos(elevation), cos(azimuth) * sin(elevation))
			var scaled := Vector3(direction.x / radii.x, direction.y / radii.y, direction.z / radii.z)
			_head_field[index] = 0.55 / maxf(scaled.length(), 0.0001)


func _sample_head_field(direction: Vector3) -> float:
	if _head_field.is_empty():
		return 0.0
	var azimuth := (atan2(direction.x, direction.z) + PI) / TAU * float(HEAD_FIELD_AZIMUTH) - 0.5
	var elevation := acos(clampf(direction.y, -1.0, 1.0)) / PI * float(HEAD_FIELD_ELEVATION) - 0.5
	var column := floori(azimuth)
	var row := floori(elevation)
	var column_fraction := azimuth - float(column)
	var row_fraction := elevation - float(row)
	var row_a := clampi(row, 0, HEAD_FIELD_ELEVATION - 1)
	var row_b := clampi(row + 1, 0, HEAD_FIELD_ELEVATION - 1)
	var column_a := wrapi(column, 0, HEAD_FIELD_AZIMUTH)
	var column_b := wrapi(column + 1, 0, HEAD_FIELD_AZIMUTH)
	var top := lerpf(_head_field[row_a * HEAD_FIELD_AZIMUTH + column_a], _head_field[row_a * HEAD_FIELD_AZIMUTH + column_b], column_fraction)
	var bottom := lerpf(_head_field[row_b * HEAD_FIELD_AZIMUTH + column_a], _head_field[row_b * HEAD_FIELD_AZIMUTH + column_b], column_fraction)
	return lerpf(top, bottom, row_fraction)


## How much of a face this direction looks at: strongest from eye level down
## across the front, zero on the forehead and above.
func _face_weight(direction: Vector3) -> float:
	var forward := direction.z * _front_sign
	# Starting the ramp on the forehead pushed the fringe off the skull and left a
	# crease across it, and exposed the cap underneath. Nothing above the brow is
	# a face as far as hair is concerned.
	var below_brow := smoothstep(0.15, -0.17, direction.y)
	return smoothstep(0.10, 0.66, forward) * below_brow


## Head collision against the baked field, with the face held clear.
##
## `lateral_bias` decides which way a strand parts. A collider on its own only
## stops hair passing THROUGH the face — it will still drape flat across it,
## because the shortest way out of a face is straight forwards. Real hair parts
## because it slides off to one side, so the face carries a sideways push as well
## as an outward one, and each guide keeps the side it was rooted on.
func _project_head_field(point: Vector3, margin: float, lateral_bias: float, face_scale: float = 1.0) -> Vector3:
	if _head_field.is_empty():
		return point
	var relative := point - _head_field_center
	var distance := relative.length()
	if distance < 0.0001:
		return _head_field_center + Vector3.UP * (_sample_head_field(Vector3.UP) + margin)
	var direction := relative / distance
	var face := _face_weight(direction) * face_scale
	# A shield standing off the face, so hair is deflected before it reaches skin
	# rather than coming to rest on it.
	var shield := face * _collider_rz * 0.30
	var surface := _sample_head_field(direction) + margin + shield
	if distance >= surface:
		return point
	var result := _head_field_center + direction * surface
	if face > 0.001:
		var side := 1.0 if (relative.x + lateral_bias) >= 0.0 else -1.0
		var lateral := Vector3(side, 0.0, 0.0)
		lateral -= direction * lateral.dot(direction)
		if lateral.length_squared() > 0.0001:
			# Scaled by penetration depth so a grazing contact is not shoved as hard
			# as a buried one, and bounded per step by the caller's one-segment cap,
			# so it accumulates into a slide around the face rather than a jump.
			var depth := clampf((surface - distance) / maxf(_collider_rz * 0.30, 0.001), 0.0, 1.0)
			result += lateral.normalized() * face * _collider_rx * 0.60 * depth
	return result


func _project_capsule(point: Vector3, a: Vector3, b: Vector3, radius: float) -> Vector3:
	var axis := b - a
	var axis_length_squared := axis.length_squared()
	var t := clampf((point - a).dot(axis) / maxf(axis_length_squared, 0.000001), 0.0, 1.0)
	var closest := a + axis * t
	var offset := point - closest
	var distance := offset.length()
	if distance >= radius:
		return point
	if distance < 0.00001:
		offset = Vector3(0.0, 0.0, _front_sign)
	else:
		offset /= distance
	return closest + offset * radius


func _update_card_lod() -> void:
	if _gpu_hair_instance.multimesh == null:
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return
	var distance := camera.global_position.distance_to(_mount.global_position)
	var wanted := _card_count
	if distance > 15.0:
		wanted = maxi(128, _card_count >> 2)
	elif distance > 6.0:
		wanted = maxi(256, _card_count >> 1)
	if wanted != _visible_card_count:
		_visible_card_count = wanted
		_gpu_hair_instance.multimesh.visible_instance_count = wanted


func _prepare_scalp_surface() -> void:
	if not _scalp_triangles.is_empty() or _body_mesh == null or _body_mesh.mesh == null:
		return
	_scalp_triangle_cdf.clear()
	_scalp_surface_area = 0.0
	var center: Vector3 = _head["center"]
	var ry := maxf(float(_head["ry"]), 0.001)
	for surface in _body_mesh.mesh.get_surface_count():
		var arrays := _body_mesh.mesh.surface_get_arrays(surface)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL] if arrays.size() > Mesh.ARRAY_NORMAL and arrays[Mesh.ARRAY_NORMAL] != null else PackedVector3Array()
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX] if arrays.size() > Mesh.ARRAY_INDEX and arrays[Mesh.ARRAY_INDEX] != null else PackedInt32Array()
		var triangle_count := floori(float(indices.size()) / 3.0) if not indices.is_empty() else floori(float(vertices.size()) / 3.0)
		for triangle_i in triangle_count:
			var i0 := indices[triangle_i * 3] if not indices.is_empty() else triangle_i * 3
			var i1 := indices[triangle_i * 3 + 1] if not indices.is_empty() else triangle_i * 3 + 1
			var i2 := indices[triangle_i * 3 + 2] if not indices.is_empty() else triangle_i * 3 + 2
			if i0 < 0 or i1 < 0 or i2 < 0 or i0 >= vertices.size() or i1 >= vertices.size() or i2 >= vertices.size():
				continue
			var p0 := _body_mesh.to_global(vertices[i0])
			var p1 := _body_mesh.to_global(vertices[i1])
			var p2 := _body_mesh.to_global(vertices[i2])
			var centroid := (p0 + p1 + p2) / 3.0
			if centroid.y < center.y - ry * 1.02:
				continue
			var cross := (p1 - p0).cross(p2 - p0)
			var area := cross.length() * 0.5
			if area < 0.00000001:
				continue
			var face_normal := cross.normalized()
			if face_normal.dot(centroid - center) < 0.0:
				face_normal = -face_normal
			var n0 := _body_mesh.global_basis * normals[i0] if i0 < normals.size() else face_normal
			var n1 := _body_mesh.global_basis * normals[i1] if i1 < normals.size() else face_normal
			var n2 := _body_mesh.global_basis * normals[i2] if i2 < normals.size() else face_normal
			if n0.dot(centroid - center) < 0.0: n0 = -n0
			if n1.dot(centroid - center) < 0.0: n1 = -n1
			if n2.dot(centroid - center) < 0.0: n2 = -n2
			_scalp_surface_area += area
			_scalp_triangles.append({"p0": p0, "p1": p1, "p2": p2, "n0": n0.normalized(), "n1": n1.normalized(), "n2": n2.normalized()})
			_scalp_triangle_cdf.append(_scalp_surface_area)


func _sample_scalp(sample_index: int) -> Dictionary:
	if _scalp_triangles.is_empty() or _scalp_surface_area <= 0.0:
		return {}
	var area_target := _hash01(float(sample_index) * 17.71 + 0.31) * _scalp_surface_area
	var low := 0
	var high := _scalp_triangle_cdf.size() - 1
	while low < high:
		var middle := (low + high) >> 1
		if _scalp_triangle_cdf[middle] < area_target:
			low = middle + 1
		else:
			high = middle
	var triangle: Dictionary = _scalp_triangles[low]
	var r1 := sqrt(_hash01(float(sample_index) * 29.23 + 1.7))
	var r2 := _hash01(float(sample_index) * 43.91 + 7.3)
	var b0 := 1.0 - r1
	var b1 := r1 * (1.0 - r2)
	var b2 := r1 * r2
	var sampled_position: Vector3 = triangle["p0"] * b0 + triangle["p1"] * b1 + triangle["p2"] * b2
	var normal: Vector3 = (triangle["n0"] * b0 + triangle["n1"] * b1 + triangle["n2"] * b2).normalized()
	return {"position": sampled_position, "normal": normal}


func _scalp_region(point: Vector3) -> Vector3:
	var center: Vector3 = _head["center"]
	var relative := point - center
	return Vector3(
		relative.x / maxf(float(_head["rx"]), 0.001),
		relative.y / maxf(float(_head["ry"]), 0.001),
		relative.z / maxf(float(_head["rz"]), 0.001) * _front_sign
	)


func _passes_hairline(region: Vector3) -> bool:
	return _hairline_margin(region) >= 0.0


## Hairline test plus frontal thinning. Long styles root at the hairline and hang
## their tips over the eyes; thinning the front is how a groom fixes that without
## dragging the hairline backwards, which would just look like a receding one.
func _accepts_root(region: Vector3, seed_value: float) -> bool:
	if _hairline_margin(region) < 0.0:
		return false
	if int(hair_settings.get("part_style", 0)) == 1:
		# A part is read as a line of exposed scalp, not as a change of direction.
		# Diverting the comb on both sides is not enough on its own: the locks
		# simply meet over the top. The line has to be a gap in the roots.
		var part_offset := clampf(float(hair_settings.get("part_offset", 0.0)), -1.0, 1.0)
		var part_strength := clampf(float(hair_settings.get("part_strength", 0.6)), 0.0, 1.0)
		var from_part := absf(region.x - part_offset)
		var gap := lerpf(0.03, 0.30, part_strength) * smoothstep(-0.30, 0.35, region.y) 			* smoothstep(-0.85, -0.20, region.z)
		if from_part < gap:
			return false
	var front_density := clampf(float(hair_settings.get("front_density", 1.0)), 0.0, 1.0)
	if front_density >= 0.999:
		return true
	var frontal := smoothstep(0.10, 0.72, region.z) * smoothstep(0.85, 0.25, absf(region.x))
	return _hash01(seed_value * 5.317 + 0.77) >= frontal * (1.0 - front_density)


## How much of the full length a strand rooted here should get. Front strands are
## the ones that reach the eyes, so they are the ones to shorten.
func _root_length_scale(region: Vector3) -> float:
	var front_length := clampf(float(hair_settings.get("front_length", 0.62)), 0.15, 1.0)
	var frontal := smoothstep(0.14, 0.78, region.z) * smoothstep(0.90, 0.30, absf(region.x))
	return lerpf(1.0, front_length, frontal)


## Cards next to the part line have to be narrow. A part is only a few
## millimetres of scalp, and a card wider than the gap simply overhangs it, which
## is why widening the gap alone never makes a part appear.
func _part_narrowing(region: Vector3) -> float:
	if int(hair_settings.get("part_style", 0)) != 1:
		return 1.0
	var part_offset := clampf(float(hair_settings.get("part_offset", 0.0)), -1.0, 1.0)
	var part_strength := clampf(float(hair_settings.get("part_strength", 0.6)), 0.0, 1.0)
	var from_part := absf(region.x - part_offset)
	var near := 1.0 - smoothstep(0.10, 0.85, from_part)
	return lerpf(1.0, lerpf(1.0, 0.34, part_strength), near)


## Which side of the part a scalp point belongs to, and how strongly. Frozen at
## build time: a strand that recomputes its side every frame chatters across the
## part line the moment the head moves.
func _part_side(region: Vector3) -> float:
	var offset := clampf(float(hair_settings.get("part_offset", 0.0)), -1.0, 1.0)
	var distance := region.x - offset
	if absf(distance) < 0.0001:
		return 1.0 if fposmod(region.z * 977.0, 2.0) < 1.0 else -1.0
	return signf(distance)


## How far above the hairline a scalp point sits, in region units. The cap needs
## the signed distance rather than the boolean so its edge can feather instead of
## ending in a swim-cap seam.
func _hairline_margin(region: Vector3) -> float:
	var front_start := clampf(float(hair_settings.get("front_hairline", hair_settings.get("hairline", 0.52))), 0.0, 1.0)
	var side_start := clampf(float(hair_settings.get("side_hairline", 0.68)), 0.0, 1.0)
	var back_start := clampf(float(hair_settings.get("back_hairline", 0.76)), 0.0, 1.0)
	var front_weight := smoothstep(0.05, 0.68, region.z)
	var back_weight := smoothstep(0.05, 0.68, -region.z)
	var front_limit := lerpf(0.78, -0.06, front_start)
	var side_limit := lerpf(0.64, -0.30, side_start)
	var back_limit := lerpf(0.58, -0.62, back_start)
	var required_y := lerpf(side_limit, front_limit, front_weight)
	required_y = lerpf(required_y, back_limit, back_weight)
	var temple := smoothstep(0.40, 0.82, absf(region.x)) * smoothstep(0.15, 0.78, region.z)
	required_y += temple * 0.10
	# A hairline that follows the analytic curve exactly cuts a blunt arc across
	# the forehead — the bowl-cut edge. Real ones are ragged, so wobble the
	# boundary. Cards and cap read the same function, so they stay in register.
	var ragged := sin(region.x * 21.7 + region.z * 4.3) * 0.55 		+ sin(region.x * 9.1 - region.z * 13.7 + 1.9) * 0.30 		+ sin(region.x * 47.3 + region.z * 31.1) * 0.15
	required_y += ragged * 0.045 * smoothstep(-0.15, 0.35, region.z)
	return region.y - required_y


## An opaque shell of the scalp mesh itself, lifted just under the locks. Hair
## cards cannot tile a curved scalp without gaps, so without this the skin shows
## between locks and the crown reads as balding however dense the groom is.
func _build_scalp_cap(root_lift: float, style: int) -> void:
	if _scalp_cap_instance == null:
		return
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var tangents := PackedFloat32Array()
	var uvs := PackedVector2Array()
	var colors := PackedColorArray()
	var lift := root_lift + 0.0016
	var inverse_basis := _mount.global_basis.inverse()
	# Tile the strip roughly every 35 mm along the comb, so the cap carries strands
	# at the same physical scale as the cards lying on top of it.
	var tile := 1.0 / 0.035
	for triangle in _scalp_triangles:
		var margins := [
			_hairline_margin(_scalp_region(triangle["p0"])),
			_hairline_margin(_scalp_region(triangle["p1"])),
			_hairline_margin(_scalp_region(triangle["p2"]))
		]
		# Keep triangles that straddle the hairline so the edge has something to
		# break up across.
		if maxf(float(margins[0]), maxf(float(margins[1]), float(margins[2]))) < -0.05:
			continue
		for corner in 3:
			var point: Vector3 = triangle["p%d" % corner]
			var normal_world: Vector3 = triangle["n%d" % corner]
			var region := _scalp_region(point)
			# The cap is a hair surface, not a coloured shell: its strands have to
			# run along the same comb field the cards do, or it reads as a smooth
			# painted mask wherever a lock parts.
			var flow_world := _growth_flow(normal_world, region, style)
			var local_point := _mount.to_local(point + normal_world * lift)
			var local_normal := (inverse_basis * normal_world).normalized()
			var local_flow := (inverse_basis * flow_world).normalized()
			var local_width := local_flow.cross(local_normal)
			if local_width.length_squared() < 0.0001:
				local_width = local_flow.cross(Vector3.RIGHT)
			local_width = local_width.normalized()
			vertices.append(local_point)
			normals.append(local_normal)
			# The tangent is the comb direction, so the anisotropic highlight on the
			# cap lines up with the highlight on the cards above it.
			tangents.append_array([local_flow.x, local_flow.y, local_flow.z, 1.0])
			var relative := local_point
			uvs.append(Vector2(relative.dot(local_width) * tile, relative.dot(local_flow) * tile))
			# Cards only exist above the hairline, so the cap must not either, or it
			# reads as a brown band across the forehead with nothing over it.
			colors.append(Color(1.0, 1.0, 1.0, smoothstep(0.015, 0.11, float(margins[corner]))))
	if vertices.is_empty():
		_scalp_cap_instance.mesh = null
		_scalp_cap_instance.visible = false
		return
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TANGENT] = tangents
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_COLOR] = colors
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	_scalp_cap_instance.mesh = mesh
	_scalp_cap_instance.visible = true


func _growth_flow(normal: Vector3, region: Vector3, style: int) -> Vector3:
	var front := Vector3(0.0, 0.0, _front_sign)
	var back := -front
	var length := clampf(float(hair_settings.get("length", 0.09)), 0.004, 0.55)
	var long_hair := smoothstep(0.07, 0.18, length)
	var desired := Vector3.DOWN * 0.72 + back * 0.28
	if region.z > 0.12:
		# Short hair can rise from the forehead; long hair must leave the
		# hairline toward the crown before gravity takes over, otherwise every
		# front guide forms the same vertical fountain arc.
		desired = (Vector3.UP * 0.84 + back * 0.58).lerp(Vector3.UP * 0.20 + back * 0.96, long_hair)
	elif absf(region.x) > 0.52:
		desired = Vector3.DOWN * 0.88 + back * 0.18
	if style == 0 and long_hair > 0.0:
		# Long procedural grooms need a coherent comb field. Projecting gravity
		# independently at every scalp normal creates the radial dandelion fan.
		var crown_weight := smoothstep(0.08, 0.72, region.y)
		var coherent_flow := (Vector3.DOWN * 0.96 + back * 0.18).lerp(back * 0.98 + Vector3.DOWN * 0.12, crown_weight)
		desired = desired.lerp(coherent_flow, long_hair)
	match style:
		1: desired = back * 0.92 + (Vector3.UP * 0.72 if region.z > 0.12 else Vector3.DOWN * 0.16)
		2: desired = Vector3(-0.82, 0.48 if region.z > 0.12 else -0.24, -0.28 * _front_sign)
		3: desired = Vector3(0.82, 0.48 if region.z > 0.12 else -0.24, -0.28 * _front_sign)
		4: return normal
	# For long natural hair, a full tangent projection is the wrong constraint:
	# it rotates one common comb direction differently at every scalp normal and
	# recreates a radial fan. Remove only the component that enters the head and
	# cap excessive lift, while preserving the shared back/down direction.
	if style == 0 and long_hair > 0.001:
		var comb := desired.normalized()
		# Natural long hair is authored as one back/down comb lane. Avoidance is
		# solved in the sagittal plane so asymmetric vertex normals cannot inject
		# left/right directions and recreate a crown star.
		var sagittal_normal := Vector3(0.0, normal.y, normal.z)
		if sagittal_normal.length_squared() > 0.0001:
			sagittal_normal = sagittal_normal.normalized()
		var normal_component := comb.dot(sagittal_normal)
		if normal_component < 0.0:
			comb -= sagittal_normal * normal_component
		elif normal_component > 0.10:
			comb -= sagittal_normal * (normal_component - 0.10)
		comb.x = 0.0
		if comb.length_squared() > 0.0001:
			var tangent_strength := smoothstep(0.25, 0.92, long_hair)
			desired = desired.lerp(comb.normalized(), tangent_strength)
			if tangent_strength > 0.995:
				return desired.normalized()
	# A part is authored here, in the comb field, not left to the solver. Hair
	# closest to the part line sweeps most sharply sideways; by the temple it is
	# just falling. Both photo references are this one field with different
	# strength: 0 is swept straight back, high is a clean two-sided part.
	if int(hair_settings.get("part_style", 0)) == 1:
		var part_offset := clampf(float(hair_settings.get("part_offset", 0.0)), -1.0, 1.0)
		var part_strength := clampf(float(hair_settings.get("part_strength", 0.6)), 0.0, 1.0)
		var from_part := absf(region.x - part_offset)
		var part_weight := (1.0 - smoothstep(0.05, 0.80, from_part)) * smoothstep(0.05, 0.55, region.y)
		if part_weight > 0.0:
			desired += Vector3(_part_side(region), 0.0, 0.0) * part_weight * part_strength
			desired = desired.normalized()

	# Everything above decides how far back and down a strand combs, but nothing
	# decides which side of the face it passes. Hair rooted forward of the ear
	# falls OUTSIDE the cheek, because the skull is widest at the temple and the
	# face is inset from it; without this term the front-side strands comb straight
	# down across the eyes and nose.
	var face_escape := smoothstep(0.02, 0.52, region.z) * smoothstep(0.10, 0.58, absf(region.x))
	if face_escape > 0.0:
		desired += Vector3(signf(region.x), 0.0, 0.0) * face_escape * 0.85
		desired = desired.normalized()

	var tangent := desired - normal * desired.dot(normal)
	if tangent.length_squared() < 0.0001:
		tangent = back - normal * back.dot(normal)
	if tangent.length_squared() < 0.0001:
		tangent = normal.cross(Vector3.RIGHT)
	return tangent.normalized()


func diagnostics() -> String:
	var triangles := _visible_card_count * CARD_SEGMENTS * (CARD_COLUMNS - 1) * 2
	return "%s • %d/%d hair cards @ %.0f mm wide • %d tris • %d×%d strip • 96×16 PBD guides • motion %.0f/%.0f mm avg/peak • body collision • one draw • static geometry %.1f ms" % [
		super.diagnostics(),
		_visible_card_count,
		_card_count,
		_card_width * 1000.0,
		triangles,
		STRIP_VARIANT_WIDTH * STRIP_VARIANTS,
		STRIP_HEIGHT,
		_average_tip_displacement * 1000.0,
		_peak_tip_displacement * 1000.0,
		_build_ms
	]
