extends "res://scripts/terrain/ground_geometry_clipmap.gd"
## Minimal VERTEX_ID/triangle-strip clipmap plus partial GPU height uploads.
##
## The terrain shader reconstructs logical grid coordinates from VERTEX_ID, so
## the CPU mesh contains only a zero-valued position stream and a compact strip
## index stream. Height storage is seven persistent DrawableTexture2D resources.
## Newly exposed toroidal rows/columns are uploaded as small rectangles.
##
## Rendering is batched into exactly two MultiMesh submissions: one instance for
## the solid L0 centre and five instances for L1-L5 rings. INSTANCE_CUSTOM carries
## clip level and outer-ring state, removing per-level MeshInstance draw overhead
## without leaving Godot's normal spatial/shadow pipeline.

var _gpu_height_textures: Array[DrawableTexture2D] = []
var _gpu_blit_calls: int = 0
var _gpu_blit_texels: int = 0
var _gpu_full_blits: int = 0
var _gpu_partial_blits: int = 0

var _center_batch: MultiMeshInstance3D
var _ring_batch: MultiMeshInstance3D


func _reset_frame(center: Vector3) -> void:
	_clear_gpu_height_textures()
	super._reset_frame(center)


func _on_world_ready(fields: PlanetFields) -> void:
	_clear_gpu_height_textures()
	super._on_world_ready(fields)


## Override the base Texture2DArray publication path. The CPU toroidal Images are
## still authoritative mirrors, but only changed rectangles are copied to VRAM.
func _publish_height_update(built: Dictionary, centers: Array, origins: Array,
		replacements: Array, updates: Array) -> void:
	if _gpu_height_textures.size() != STORAGE_LEVELS:
		var initial_images: Array[Image] = []
		for level: int in STORAGE_LEVELS:
			var initial_value: Variant = replacements[level]
			if not (initial_value is Image):
				return
			initial_images.append(initial_value)
		if not _create_gpu_height_textures(initial_images):
			return
	else:
		for level: int in STORAGE_LEVELS:
			var replacement_value: Variant = replacements[level]
			if replacement_value is Image:
				_images[level] = replacement_value
				_blit_level_rect(level,
					Rect2i(0, 0, HEIGHT_TEX_SIZE, HEIGHT_TEX_SIZE))
				_gpu_full_blits += 1
				continue

			var updates_value: Variant = updates[level]
			if not (updates_value is PackedVector3Array):
				continue
			var level_updates: PackedVector3Array = updates_value
			if level_updates.is_empty():
				continue

			var image: Image = _images[level]
			for sample: Vector3 in level_updates:
				var sx: int = int(sample.x)
				var sy: int = int(sample.y)
				if sx < 0 or sx >= HEIGHT_TEX_SIZE or sy < 0 or sy >= HEIGHT_TEX_SIZE:
					continue
				image.set_pixel(sx, sy, Color(sample.z, 0.0, 0.0, 1.0))

			var rects: Array[Rect2i] = _updates_to_rects(level_updates)
			for rect: Rect2i in rects:
				_blit_level_rect(level, rect)
				if rect.size.x == HEIGHT_TEX_SIZE and rect.size.y == HEIGHT_TEX_SIZE:
					_gpu_full_blits += 1
				else:
					_gpu_partial_blits += 1

	for level: int in STORAGE_LEVELS:
		var new_center: Vector2 = centers[level]
		var new_origin: Vector2i = origins[level]
		_layer_centers[level] = new_center
		_layer_origins[level] = new_origin

	var built_center_value: Variant = built.get("mesh_center", _published_center)
	if built_center_value is Vector2:
		_published_center = built_center_value

	# The inherited streamer uses `_height_texture != null` only as its ready flag.
	# Keep a 1x1 marker so coverage/cutout logic can remain shared; actual terrain
	# heights come exclusively from u_height0..u_height6.
	_ensure_height_ready_marker()
	_material.set_shader_parameter("u_height_ready", 1.0)
	_sync_height_layout_uniforms()
	_set_visible(_active)
	_sync_global_cutout(_active)


func _create_gpu_height_textures(initial_images: Array[Image]) -> bool:
	_clear_gpu_height_textures()
	_images.clear()

	for level: int in STORAGE_LEVELS:
		var image: Image = initial_images[level]
		if image == null or image.get_width() != HEIGHT_TEX_SIZE \
				or image.get_height() != HEIGHT_TEX_SIZE:
			_clear_gpu_height_textures()
			return false

		var target := DrawableTexture2D.new()
		target.setup(HEIGHT_TEX_SIZE, HEIGHT_TEX_SIZE,
			DrawableTexture2D.DRAWABLE_FORMAT_RGBAF, Color(0.0, 0.0, 0.0, 1.0), false)
		_gpu_height_textures.append(target)
		_images.append(image)
		_material.set_shader_parameter("u_height%d" % level, target)

	for level: int in STORAGE_LEVELS:
		_blit_level_rect(level, Rect2i(0, 0, HEIGHT_TEX_SIZE, HEIGHT_TEX_SIZE))
		_gpu_full_blits += 1
	return true


func _clear_gpu_height_textures() -> void:
	for level: int in STORAGE_LEVELS:
		if _material != null:
			_material.set_shader_parameter("u_height%d" % level, null)
	_gpu_height_textures.clear()


func _ensure_height_ready_marker() -> void:
	if _height_texture != null:
		return
	var marker_image := Image.create(1, 1, false, Image.FORMAT_RF)
	marker_image.set_pixel(0, 0, Color(1.0, 0.0, 0.0, 1.0))
	var marker_images: Array[Image] = [marker_image]
	var marker := Texture2DArray.new()
	if marker.create_from_images(marker_images) == OK:
		_height_texture = marker


## Upload a rectangle from the CPU mirror into one persistent GPU level. The
## staging texture is exactly the size of the changed rectangle, so crossing one
## 0.75 m cell typically uploads a narrow row/column rather than all 69x69 texels.
func _blit_level_rect(level: int, rect: Rect2i) -> void:
	if level < 0 or level >= _gpu_height_textures.size():
		return
	if rect.size.x <= 0 or rect.size.y <= 0:
		return

	var clipped: Rect2i = rect.intersection(Rect2i(0, 0, HEIGHT_TEX_SIZE, HEIGHT_TEX_SIZE))
	if clipped.size.x <= 0 or clipped.size.y <= 0:
		return

	var patch := Image.create(clipped.size.x, clipped.size.y, false, Image.FORMAT_RF)
	patch.blit_rect(_images[level], clipped, Vector2i.ZERO)
	var source: ImageTexture = ImageTexture.create_from_image(patch)
	if source == null:
		return
	_gpu_height_textures[level].blit_rect(clipped, source)
	_gpu_blit_calls += 1
	_gpu_blit_texels += clipped.size.x * clipped.size.y


## Convert an arbitrary set of changed storage texels into a small set of solid
## rectangles. A one-cell toroidal X shift becomes one vertical rectangle; an X+Y
## shift normally becomes a few rectangles rather than individual texel uploads.
static func _updates_to_rects(updates: PackedVector3Array) -> Array[Rect2i]:
	var rects: Array[Rect2i] = []
	if updates.is_empty():
		return rects

	var changed := PackedByteArray()
	changed.resize(HEIGHT_TEX_SIZE * HEIGHT_TEX_SIZE)
	for sample: Vector3 in updates:
		var x: int = int(sample.x)
		var y: int = int(sample.y)
		if x >= 0 and x < HEIGHT_TEX_SIZE and y >= 0 and y < HEIGHT_TEX_SIZE:
			changed[y * HEIGHT_TEX_SIZE + x] = 1

	for y: int in HEIGHT_TEX_SIZE:
		var x: int = 0
		while x < HEIGHT_TEX_SIZE:
			if changed[y * HEIGHT_TEX_SIZE + x] == 0:
				x += 1
				continue

			var width: int = 1
			while x + width < HEIGHT_TEX_SIZE \
					and changed[y * HEIGHT_TEX_SIZE + x + width] != 0:
				width += 1

			var height: int = 1
			while y + height < HEIGHT_TEX_SIZE:
				var row_complete := true
				for xx: int in range(x, x + width):
					if changed[(y + height) * HEIGHT_TEX_SIZE + xx] == 0:
						row_complete = false
						break
				if not row_complete:
					break
				height += 1

			var out_rect := Rect2i(x, y, width, height)
			rects.append(out_rect)
			for yy: int in range(y, y + height):
				for xx: int in range(x, x + width):
					changed[yy * HEIGHT_TEX_SIZE + xx] = 0
			x += width
	return rects


func gpu_stream_stats() -> Dictionary:
	return {
		"textures": _gpu_height_textures.size(),
		"blit_calls": _gpu_blit_calls,
		"blit_texels": _gpu_blit_texels,
		"full_blits": _gpu_full_blits,
		"partial_blits": _gpu_partial_blits,
		"draw_batches": 2 if _center_batch != null and _ring_batch != null else 0,
	}


## Two rendering submissions total. The centre and rings need different topology,
## so they cannot share one MultiMesh, but all five annular levels can be instanced
## together. Their identity transforms are intentional: spherical placement is
## reconstructed entirely in the vertex shader.
func _build_ring_nodes() -> void:
	var full: ArrayMesh = _build_strip_mesh(false)
	var ring: ArrayMesh = _build_strip_mesh(true)
	var bounds := AABB(
		Vector3(-100000.0, -100000.0, -100000.0),
		Vector3(200000.0, 200000.0, 200000.0))

	_center_batch = _make_clipmap_batch("GroundClipmapCenter", full, 1, bounds)
	_center_batch.multimesh.set_instance_custom_data(0, Color(0.0, 0.0, 0.0, 0.0))
	add_child(_center_batch)

	_ring_batch = _make_clipmap_batch("GroundClipmapRings", ring, RENDER_LEVELS - 1, bounds)
	for instance_index: int in range(RENDER_LEVELS - 1):
		var level: int = instance_index + 1
		var outer: float = 1.0 if level == RENDER_LEVELS - 1 else 0.0
		_ring_batch.multimesh.set_instance_custom_data(instance_index,
			Color(float(level), outer, 0.0, 0.0))
	add_child(_ring_batch)


func _make_clipmap_batch(node_name: String, mesh: ArrayMesh, count: int,
		bounds: AABB) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	# Formats/feature flags must be set before instance_count allocates buffers.
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.custom_aabb = bounds
	mm.instance_count = count
	mm.visible_instance_count = count
	for instance_index: int in count:
		mm.set_instance_transform(instance_index, Transform3D.IDENTITY)

	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.material_override = _material
	return batch


## Base clipmap visibility iterates an Array[MeshInstance3D]. The batched renderer
## intentionally leaves that array empty and owns visibility here instead.
func _set_visible(value: bool) -> void:
	if _center_batch != null:
		_center_batch.visible = value
	if _ring_batch != null:
		_ring_batch.visible = value


static func _build_strip_mesh(with_hole: bool) -> ArrayMesh:
	var vertex_count: int = GRID_VERTS * GRID_VERTS
	var vertices := PackedVector3Array()
	vertices.resize(vertex_count)

	var indices := PackedInt32Array()
	if with_hole:
		_build_ring_strip_indices(indices)
	else:
		_build_full_strip_indices(indices)

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLE_STRIP, arrays)
	return mesh


static func _build_full_strip_indices(indices: PackedInt32Array) -> void:
	for y: int in GRID_CELLS:
		_append_row_segment(indices, y, 0, GRID_CELLS)


static func _build_ring_strip_indices(indices: PackedInt32Array) -> void:
	var inner_min: int = GRID_CELLS >> 2
	var inner_max: int = GRID_CELLS - inner_min
	for y: int in GRID_CELLS:
		if y < inner_min or y >= inner_max:
			_append_row_segment(indices, y, 0, GRID_CELLS)
		else:
			_append_row_segment(indices, y, 0, inner_min)
			_append_row_segment(indices, y, inner_max, GRID_CELLS)


static func _append_row_segment(indices: PackedInt32Array,
		y: int, x0: int, x1: int) -> void:
	if x1 <= x0:
		return
	var first: int = y * GRID_VERTS + x0
	if not indices.is_empty():
		var previous_last: int = indices[indices.size() - 1]
		indices.append(previous_last)
		indices.append(first)

	for x: int in range(x0, x1 + 1):
		indices.append(y * GRID_VERTS + x)
		indices.append((y + 1) * GRID_VERTS + x)
