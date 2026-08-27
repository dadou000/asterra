extends Node
## GPU-visible local window over the authoritative sparse terrain edit lattice.
##
## Deltas remains the persistent source of truth. This node mirrors the nearby
## edited height offsets into a pair of R32F textures. Rebuilds always write the
## inactive texture and then swap the material binding, so a deformation update
## never exposes a half-updated image to the terrain shader.

const RESOLUTION := 512
const SAMPLE_SPACING_M := 1.0
const HALF_EXTENT_M := float(RESOLUTION) * SAMPLE_SPACING_M * 0.5
const RECENTER_DISTANCE_M := 160.0

var ready_state := false
var texture: ImageTexture
var center_dir := Vector3(1.0, 0.0, 0.0)
var center_right := Vector3(0.0, 0.0, -1.0)
var center_up := Vector3(0.0, 1.0, 0.0)
var generation := 0

var _image: Image
var _textures: Array[ImageTexture] = []
var _active_texture_index := 0
var _center_valid := false
var _bound_material: ShaderMaterial
var _dirty_full := true


func _ready() -> void:
	process_priority = -4
	_image = Image.create(RESOLUTION, RESOLUTION, false, Image.FORMAT_RF)
	_image.fill(Color(0.0, 0.0, 0.0, 1.0))
	var first: ImageTexture = ImageTexture.create_from_image(_image)
	var second: ImageTexture = ImageTexture.create_from_image(_image)
	if first != null:
		_textures.append(first)
	if second != null:
		_textures.append(second)
	if _textures.size() == 2:
		texture = _textures[0]
		ready_state = true
	if Deltas.has_signal("region_changed"):
		Deltas.region_changed.connect(_on_region_changed)
	if Deltas.has_signal("all_changed"):
		Deltas.all_changed.connect(_on_all_changed)


func _process(_dt: float) -> void:
	if not ready_state or Planet.cfg == null or not Planet.ready_state:
		return
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_sync_renderer_binding()
		return
	var wp: Vec3D = Frames.to_world(camera.global_position)
	if wp.length_sq() > 1.0:
		var d: Vector3 = wp.normalized().to_v3()
		if not _center_valid or _surface_distance(center_dir, d) >= RECENTER_DISTANCE_M:
			_set_center(d)
	if _dirty_full and _center_valid:
		_rebuild_full()
	_sync_renderer_binding()


func _set_center(d: Vector3) -> void:
	center_dir = d.normalized()
	var basis: Array = CubeSphere.tangent_basis(center_dir)
	center_right = (basis[0] as Vector3).normalized()
	center_up = (basis[1] as Vector3).normalized()
	_center_valid = true
	_dirty_full = true


func _rebuild_full() -> void:
	if _textures.size() != 2:
		return
	var samples: PackedFloat32Array = Deltas.sample_tangent_patch(
		center_dir, center_right, center_up, RESOLUTION, SAMPLE_SPACING_M,
		Planet.cfg.planet_radius)
	if samples.size() != RESOLUTION * RESOLUTION:
		return
	var data: PackedByteArray = samples.to_byte_array()
	_image = Image.create_from_data(RESOLUTION, RESOLUTION, false, Image.FORMAT_RF, data)
	var staging_index: int = 1 - _active_texture_index
	var staging: ImageTexture = _textures[staging_index]
	staging.update(_image)
	_active_texture_index = staging_index
	texture = staging
	generation += 1
	_dirty_full = false


func _on_region_changed(edit_dir: Vector3, radius_m: float) -> void:
	if not _center_valid or Planet.cfg == null:
		_dirty_full = true
		return
	if _surface_distance(center_dir, edit_dir) > HALF_EXTENT_M + radius_m + 8.0:
		return
	_dirty_full = true


func _on_all_changed() -> void:
	_dirty_full = true


func _surface_distance(a: Vector3, b: Vector3) -> float:
	return acos(clampf(a.normalized().dot(b.normalized()), -1.0, 1.0)) * Planet.cfg.planet_radius


func sample_params() -> Dictionary:
	return {
		"texture": texture,
		"ready": ready_state and _center_valid,
		"center_dir": center_dir,
		"center_right": center_right,
		"center_up": center_up,
		"half_extent_m": HALF_EXTENT_M,
		"double_buffered": true,
		"generation": generation,
	}


func _sync_renderer_binding() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		return
	var value: Variant = terrain.get("_material")
	if not (value is ShaderMaterial):
		return
	var material := value as ShaderMaterial
	_bound_material = material
	material.set_shader_parameter("u_edit_delta", texture)
	material.set_shader_parameter("u_edit_ready", 1.0 if ready_state and _center_valid else 0.0)
	material.set_shader_parameter("u_edit_center_dir", center_dir)
	material.set_shader_parameter("u_edit_center_right", center_right)
	material.set_shader_parameter("u_edit_center_up", center_up)
	material.set_shader_parameter("u_edit_half_extent_m", HALF_EXTENT_M)
