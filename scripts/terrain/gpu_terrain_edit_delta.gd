extends Node
## GPU-visible local window over the authoritative sparse terrain edit lattice.
##
## Deltas remains the persistent source of truth. This node mirrors nearby edited
## height offsets into a pair of R32F textures. Re-centres rebuild the whole local
## window, but ordinary deformation updates only re-sample the dirty texel rectangle
## before publishing the completed CPU mirror into the inactive GPU texture.

const RESOLUTION := 512
const SAMPLE_SPACING_M := 1.0
const HALF_EXTENT_M := float(RESOLUTION) * SAMPLE_SPACING_M * 0.5
const RECENTER_DISTANCE_M := 160.0
const DIRTY_TEXEL_MARGIN := 3

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
var _dirty_partial := false
var _dirty_rect := Rect2i()


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
	elif _dirty_partial and _center_valid:
		_rebuild_partial()
	_sync_renderer_binding()


func _set_center(d: Vector3) -> void:
	center_dir = d.normalized()
	var basis: Array = CubeSphere.tangent_basis(center_dir)
	center_right = (basis[0] as Vector3).normalized()
	center_up = (basis[1] as Vector3).normalized()
	_center_valid = true
	_dirty_full = true
	_dirty_partial = false
	_dirty_rect = Rect2i()


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
	_publish_cpu_mirror()
	_dirty_full = false
	_dirty_partial = false
	_dirty_rect = Rect2i()


func _rebuild_partial() -> void:
	if _textures.size() != 2 or _dirty_rect.size.x <= 0 or _dirty_rect.size.y <= 0:
		_dirty_partial = false
		_dirty_rect = Rect2i()
		return
	var rect: Rect2i = _dirty_rect
	var samples: PackedFloat32Array = Deltas.sample_tangent_rect(
		center_dir, center_right, center_up, RESOLUTION, SAMPLE_SPACING_M,
		Planet.cfg.planet_radius, rect)
	var expected_samples: int = rect.size.x * rect.size.y
	if samples.size() != expected_samples:
		_dirty_full = true
		_dirty_partial = false
		_dirty_rect = Rect2i()
		return
	var patch_data: PackedByteArray = samples.to_byte_array()
	var patch: Image = Image.create_from_data(
		rect.size.x, rect.size.y, false, Image.FORMAT_RF, patch_data)
	_image.blit_rect(patch, Rect2i(Vector2i.ZERO, rect.size), rect.position)
	_publish_cpu_mirror()
	_dirty_partial = false
	_dirty_rect = Rect2i()


func _publish_cpu_mirror() -> void:
	var staging_index: int = 1 - _active_texture_index
	var staging: ImageTexture = _textures[staging_index]
	staging.update(_image)
	_active_texture_index = staging_index
	texture = staging
	generation += 1


func _on_region_changed(edit_dir: Vector3, radius_m: float) -> void:
	if not _center_valid or Planet.cfg == null:
		_dirty_full = true
		return
	if _surface_distance(center_dir, edit_dir) > HALF_EXTENT_M + radius_m + 8.0:
		return
	if _dirty_full:
		return
	var rect: Rect2i = _dirty_rect_for_region(edit_dir, radius_m)
	if rect.size.x <= 0 or rect.size.y <= 0:
		return
	if not _dirty_partial:
		_dirty_rect = rect
		_dirty_partial = true
		return
	var min_x: int = mini(_dirty_rect.position.x, rect.position.x)
	var min_y: int = mini(_dirty_rect.position.y, rect.position.y)
	var max_x: int = maxi(_dirty_rect.position.x + _dirty_rect.size.x,
		rect.position.x + rect.size.x)
	var max_y: int = maxi(_dirty_rect.position.y + _dirty_rect.size.y,
		rect.position.y + rect.size.y)
	_dirty_rect = Rect2i(min_x, min_y, max_x - min_x, max_y - min_y)


func _dirty_rect_for_region(edit_dir: Vector3, radius_m: float) -> Rect2i:
	var d: Vector3 = edit_dir.normalized()
	var denom: float = d.dot(center_dir)
	if denom <= 0.01:
		return Rect2i()
	var planet_radius: float = Planet.cfg.planet_radius
	var plane_x_m: float = d.dot(center_right) / denom * planet_radius
	var plane_y_m: float = d.dot(center_up) / denom * planet_radius
	var half_pixel: float = (float(RESOLUTION) - 1.0) * 0.5
	var center_x: float = plane_x_m / SAMPLE_SPACING_M + half_pixel
	var center_y: float = plane_y_m / SAMPLE_SPACING_M + half_pixel
	var pad: int = int(ceil(maxf(radius_m, 0.0) / SAMPLE_SPACING_M)) + DIRTY_TEXEL_MARGIN
	var x0: int = clampi(int(floor(center_x)) - pad, 0, RESOLUTION)
	var y0: int = clampi(int(floor(center_y)) - pad, 0, RESOLUTION)
	var x1: int = clampi(int(ceil(center_x)) + pad + 1, 0, RESOLUTION)
	var y1: int = clampi(int(ceil(center_y)) + pad + 1, 0, RESOLUTION)
	if x1 <= x0 or y1 <= y0:
		return Rect2i()
	return Rect2i(x0, y0, x1 - x0, y1 - y0)


func _on_all_changed() -> void:
	_dirty_full = true
	_dirty_partial = false
	_dirty_rect = Rect2i()


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
		"partial_updates": true,
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
