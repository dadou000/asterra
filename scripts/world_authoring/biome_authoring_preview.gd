class_name BiomeAuthoringPreview
extends Node
## High-resolution local runtime mirror for Planet Studio biome paint.
##
## The immutable GPUPlanetContext biome map is only the coarse generated field
## (192 cells/face in the canonical Asterra preset). Authoring brushes can be tens
## of metres wide, so baking them into that map would turn a small edit into a
## multi-kilometre block. This node instead rasterizes nearby sparse biome strokes
## into a camera-centred categorical override texture consumed by the terrain
## shader. The saved source of truth remains the authoring layer's spherical
## strokes; this texture is disposable and is rebuilt after recenter/history/body
## changes.

const RESOLUTION: int = 512
const SAMPLE_SPACING_M: float = 2.0
const HALF_EXTENT_M: float = float(RESOLUTION) * SAMPLE_SPACING_M * 0.5
const RECENTER_DISTANCE_M: float = 192.0
const BIOME_NORMALIZATION: float = 19.0
const MIN_DENOM: float = 0.01

var _session: WorldAuthoringSession
var _world_host: Node
var _texture: ImageTexture
var _image: Image
var _center_dir: Vector3 = Vector3(1.0, 0.0, 0.0)
var _center_right: Vector3 = Vector3(0.0, 0.0, -1.0)
var _center_up: Vector3 = Vector3(0.0, 1.0, 0.0)
var _center_valid: bool = false
var _dirty: bool = true
var _has_content: bool = false
var _bound_material: ShaderMaterial
var _rasterized_strokes: int = 0
var _rasterized_pixels: int = 0


func bind(session: WorldAuthoringSession, world_host: Node) -> void:
	_session = session
	_world_host = world_host


func _ready() -> void:
	process_priority = 8
	_image = Image.create(RESOLUTION, RESOLUTION, false, Image.FORMAT_RG8)
	_image.fill(Color(0.0, 0.0, 0.0, 1.0))
	_texture = ImageTexture.create_from_image(_image)
	if _session != null:
		if not _session.changed.is_connected(_on_authoring_changed):
			_session.changed.connect(_on_authoring_changed)
		if not _session.preset_loaded.is_connected(_on_preset_loaded):
			_session.preset_loaded.connect(_on_preset_loaded)
	_dirty = true


func _process(_delta: float) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		_set_shader_ready(false)
		return
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_shader_ready(false)
		return
	var world_position: Vec3D = Frames.to_world(camera.global_position)
	if world_position.length_sq() <= 1.0:
		_set_shader_ready(false)
		return
	var observer_dir: Vector3 = world_position.normalized().to_v3()
	if not _center_valid or _surface_distance(_center_dir, observer_dir) >= RECENTER_DISTANCE_M:
		_set_center(observer_dir)
	if _dirty:
		_rebuild()
	_sync_shader_binding()


func mark_dirty() -> void:
	_dirty = true


func _on_authoring_changed(_dirty_state: bool, _apply_scope: int) -> void:
	_dirty = true


func _on_preset_loaded(_path: String) -> void:
	_dirty = true


func _set_center(direction: Vector3) -> void:
	_center_dir = direction.normalized()
	var basis: Array = CubeSphere.tangent_basis(_center_dir)
	_center_right = (basis[0] as Vector3).normalized()
	_center_up = (basis[1] as Vector3).normalized()
	_center_valid = true
	_dirty = true


func _rebuild() -> void:
	_dirty = false
	_has_content = false
	_rasterized_strokes = 0
	_rasterized_pixels = 0
	if _image == null:
		return
	_image.fill(Color(0.0, 0.0, 0.0, 1.0))
	if _session == null or Planet.cfg == null or not _center_valid:
		_publish()
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_publish()
		return
	var layers_value: Variant = terrain.get(&"biome_override_layers")
	if not (layers_value is Array):
		_publish()
		return
	var layers: Array = (layers_value as Array).duplicate()
	layers.sort_custom(func(a: Variant, b: Variant) -> bool:
		var ar: Resource = a as Resource
		var br: Resource = b as Resource
		if ar == null:
			return true
		if br == null:
			return false
		return int(ar.get(&"priority")) < int(br.get(&"priority"))
	)
	for layer_value: Variant in layers:
		var layer: Resource = layer_value as Resource
		if layer == null or not bool(layer.get(&"enabled")):
			continue
		var layer_opacity: float = clampf(float(layer.get(&"opacity")), 0.0, 1.0)
		if layer_opacity <= 0.0001:
			continue
		var blend_mode: int = int(layer.get(&"blend_mode"))
		var strokes_value: Variant = layer.get(&"strokes")
		if not (strokes_value is Array):
			continue
		var strokes: Array = strokes_value as Array
		for stroke_index: int in strokes.size():
			var stroke_value: Variant = strokes[stroke_index]
			if not (stroke_value is Dictionary):
				continue
			_rasterize_stroke(stroke_value as Dictionary, layer_opacity, blend_mode, stroke_index)
	_publish()


func _rasterize_stroke(stroke: Dictionary, layer_opacity: float,
		blend_mode: int, stroke_index: int) -> void:
	var center_value: Variant = stroke.get("center_dir", Vector3.ZERO)
	if not (center_value is Vector3):
		return
	var stroke_dir: Vector3 = (center_value as Vector3).normalized()
	if stroke_dir.length_squared() < 0.99:
		return
	var radius_m: float = maxf(float(stroke.get("radius_m", 0.0)), 0.0)
	if radius_m <= 0.0:
		return
	var denom: float = stroke_dir.dot(_center_dir)
	if denom <= MIN_DENOM:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	var projection_scale: float = planet_radius / denom
	var plane_x_m: float = stroke_dir.dot(_center_right) * projection_scale
	var plane_y_m: float = stroke_dir.dot(_center_up) * projection_scale
	if absf(plane_x_m) > HALF_EXTENT_M + radius_m + SAMPLE_SPACING_M * 2.0 \
			or absf(plane_y_m) > HALF_EXTENT_M + radius_m + SAMPLE_SPACING_M * 2.0:
		return

	var half_pixel: float = (float(RESOLUTION) - 1.0) * 0.5
	var center_px: float = plane_x_m / SAMPLE_SPACING_M + half_pixel
	# Image Y runs down while the tangent up vector runs up.
	var center_py: float = half_pixel - plane_y_m / SAMPLE_SPACING_M
	var radius_px: int = int(ceil(radius_m / SAMPLE_SPACING_M)) + 3
	var x0: int = clampi(int(floor(center_px)) - radius_px, 0, RESOLUTION - 1)
	var y0: int = clampi(int(floor(center_py)) - radius_px, 0, RESOLUTION - 1)
	var x1: int = clampi(int(ceil(center_px)) + radius_px, 0, RESOLUTION - 1)
	var y1: int = clampi(int(ceil(center_py)) + radius_px, 0, RESOLUTION - 1)
	if x1 < x0 or y1 < y0:
		return

	var hardness: float = clampf(float(stroke.get("hardness", 1.0)), 0.0, 0.999)
	var stroke_opacity: float = clampf(float(stroke.get("opacity", 1.0)), 0.0, 1.0)
	var biome_id: int = clampi(int(stroke.get("biome_id", 0)), 0, 17)
	var wrote_any: bool = false
	for image_y: int in range(y0, y1 + 1):
		var oy_m: float = (half_pixel - float(image_y)) * SAMPLE_SPACING_M
		for image_x: int in range(x0, x1 + 1):
			var ox_m: float = (float(image_x) - half_pixel) * SAMPLE_SPACING_M
			var sample_dir: Vector3 = (
				_center_dir
				+ _center_right * (ox_m / planet_radius)
				+ _center_up * (oy_m / planet_radius)
			).normalized()
			var distance_m: float = acos(clampf(sample_dir.dot(stroke_dir), -1.0, 1.0)) * planet_radius
			if distance_m > radius_m:
				continue
			var normalized_distance: float = distance_m / maxf(radius_m, 0.001)
			var falloff: float = 1.0
			if normalized_distance > hardness:
				var edge_t: float = (normalized_distance - hardness) / maxf(1.0 - hardness, 0.001)
				falloff = 1.0 - smoothstep(0.0, 1.0, edge_t)
			var influence: float = clampf(falloff * stroke_opacity * layer_opacity, 0.0, 1.0)
			if influence <= 0.0001 or _stable_threshold(sample_dir, stroke_index, biome_id) > influence:
				continue
			if blend_mode == 1: # BiomePaintLayer.BlendMode.ERASE without a static dependency.
				_image.set_pixel(image_x, image_y, Color(0.0, 0.0, 0.0, 1.0))
			else:
				_image.set_pixel(image_x, image_y,
					Color(float(biome_id) / BIOME_NORMALIZATION, 1.0, 0.0, 1.0))
			wrote_any = true
			_rasterized_pixels += 1
	if wrote_any:
		_has_content = true
		_rasterized_strokes += 1


func _stable_threshold(direction: Vector3, stroke_index: int, biome_id: int) -> float:
	# Quantized spherical direction makes opacity/falloff dithering stable when the
	# camera-centred texture recentres. This is categorical paint: stochastic edge
	# coverage preserves partial opacity without inventing fractional biome IDs.
	var qx: int = int(floor((direction.x + 1.0) * 32767.5))
	var qy: int = int(floor((direction.y + 1.0) * 32767.5))
	var qz: int = int(floor((direction.z + 1.0) * 32767.5))
	var hash_value: int = qx * 73856093
	hash_value = hash_value ^ (qy * 19349663)
	hash_value = hash_value ^ (qz * 83492791)
	hash_value = hash_value ^ (stroke_index * 2654435761)
	hash_value = hash_value ^ (biome_id * 97531)
	return float(hash_value & 0xFFFF) / 65535.0


func _publish() -> void:
	if _texture == null:
		_texture = ImageTexture.create_from_image(_image)
	else:
		_texture.update(_image)
	_sync_shader_binding()


func _sync_shader_binding() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		return
	var value: Variant = terrain.get("_material")
	if not (value is ShaderMaterial):
		return
	var material: ShaderMaterial = value as ShaderMaterial
	_bound_material = material
	material.set_shader_parameter("u_author_biome_override", _texture)
	material.set_shader_parameter("u_author_biome_center", _center_dir)
	material.set_shader_parameter("u_author_biome_right", _center_right)
	material.set_shader_parameter("u_author_biome_up", _center_up)
	material.set_shader_parameter("u_author_biome_half_extent_m", HALF_EXTENT_M)
	material.set_shader_parameter("u_author_biome_planet_radius",
		maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1.0)
	material.set_shader_parameter("u_author_biome_ready",
		1.0 if _center_valid and _has_content and _texture != null else 0.0)


func _set_shader_ready(value: bool) -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		return
	var material_value: Variant = terrain.get("_material")
	if material_value is ShaderMaterial:
		(material_value as ShaderMaterial).set_shader_parameter(
			"u_author_biome_ready", 1.0 if value else 0.0)


func _surface_distance(a: Vector3, b: Vector3) -> float:
	if Planet.cfg == null:
		return INF
	return acos(clampf(a.normalized().dot(b.normalized()), -1.0, 1.0)) \
		* maxf(float(Planet.cfg.planet_radius), 1.0)


func stats() -> Dictionary:
	return {
		"resolution": RESOLUTION,
		"spacing_m": SAMPLE_SPACING_M,
		"half_extent_m": HALF_EXTENT_M,
		"center_valid": _center_valid,
		"has_content": _has_content,
		"rasterized_strokes": _rasterized_strokes,
		"rasterized_pixels": _rasterized_pixels,
		"categorical_dither": true,
		"non_destructive": true,
	}
