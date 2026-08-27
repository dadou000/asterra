extends "res://scripts/terrain/gpu_terrain_edit_delta.gd"
## Persistent sparse edit mirror optimized for traversal.
##
## A 160 m recenter used to publish a complete 512x512 CPU mirror even when the new
## window contained no persistent edits. Once any edit existed elsewhere in the
## world that produced regular traversal stalls. Recenter now starts from a clean
## CPU image, samples only rectangles covered by nearby sparse Deltas tiles, and
## disables the edit texture entirely for empty local windows without uploading it.

const SPARSE_DENSE_FALLBACK_FRACTION := 0.42
const SPARSE_TILE_SIDE_CELLS := 64
const SPARSE_TILE_RADIUS_SCALE := 0.80
const SPARSE_RECT_MARGIN_M := 5.0
const SPARSE_RECT_MERGE_PAD_PX := 2

var _opt_bound_material: ShaderMaterial
var _opt_bound_generation := -1
var _opt_bound_ready := false
var _sparse_recenters := 0
var _dense_recenters := 0
var _empty_recenters := 0
var _last_recenter_sampled_pixels := 0
var _local_content := false


func _rebuild_full() -> void:
	if _textures.size() != 2 or Planet.cfg == null:
		return

	# Always clear the CPU image so a future partial edit can safely publish it
	# without carrying pixels from the previous tangent window.
	_image.fill(Color(0.0, 0.0, 0.0, 1.0))
	_last_recenter_sampled_pixels = 0

	if Deltas.is_empty():
		_finish_empty_recenter()
		return

	var planet_radius: float = Planet.cfg.planet_radius
	var tile_spacing_m: float = Deltas.sample_spacing(planet_radius)
	var tile_radius_m: float = tile_spacing_m * float(SPARSE_TILE_SIDE_CELLS) \
		* SPARSE_TILE_RADIUS_SCALE + SPARSE_RECT_MARGIN_M
	var window_radius_m: float = HALF_EXTENT_M * 1.45 + tile_radius_m
	var snapshot: Dictionary = Deltas.snapshot_for_bounds(
		center_dir, window_radius_m / maxf(planet_radius, 1.0))
	if snapshot.is_empty():
		_finish_empty_recenter()
		return

	var rects: Array[Rect2i] = _sparse_rects_for_snapshot(snapshot, tile_radius_m)
	if rects.is_empty():
		_finish_empty_recenter()
		return
	var total_pixels: int = 0
	for rect: Rect2i in rects:
		total_pixels += rect.size.x * rect.size.y

	var full_pixels: int = RESOLUTION * RESOLUTION
	if total_pixels > int(float(full_pixels) * SPARSE_DENSE_FALLBACK_FRACTION):
		_dense_recenters += 1
		_local_content = true
		super._rebuild_full()
		return

	for rect: Rect2i in rects:
		var samples: PackedFloat32Array = Deltas.sample_tangent_rect(
			center_dir, center_right, center_up, RESOLUTION, SAMPLE_SPACING_M,
			planet_radius, rect)
		var expected_samples: int = rect.size.x * rect.size.y
		if samples.size() != expected_samples:
			_dense_recenters += 1
			_local_content = true
			super._rebuild_full()
			return
		var patch_data: PackedByteArray = samples.to_byte_array()
		var patch: Image = Image.create_from_data(
			rect.size.x, rect.size.y, false, Image.FORMAT_RF, patch_data)
		_image.blit_rect(patch, Rect2i(Vector2i.ZERO, rect.size), rect.position)
		_last_recenter_sampled_pixels += expected_samples

	_local_content = true
	_publish_sparse_recenter()
	_sparse_recenters += 1


func _rebuild_partial() -> void:
	# A local edit is about to be written into the already-cleared CPU image. Mark
	# content before the base publishes so the new texture becomes visible atomically.
	_local_content = true
	super._rebuild_partial()


func _finish_empty_recenter() -> void:
	_local_content = false
	_dirty_full = false
	_dirty_partial = false
	_dirty_rect = Rect2i()
	# No texture bytes changed. Advance only the logical generation so renderer/query
	# bindings publish ready=false and the new centre without a 1 MiB GPU upload.
	generation += 1
	_empty_recenters += 1
	_sparse_recenters += 1


func _publish_sparse_recenter() -> void:
	_publish_cpu_mirror()
	_dirty_full = false
	_dirty_partial = false
	_dirty_rect = Rect2i()


func _sparse_rects_for_snapshot(snapshot: Dictionary, tile_radius_m: float) -> Array[Rect2i]:
	var rects: Array[Rect2i] = []
	var half_pixel: float = (float(RESOLUTION) - 1.0) * 0.5
	var pad_px: int = int(ceil(tile_radius_m / SAMPLE_SPACING_M)) + 2
	var keys: Array = snapshot.keys()
	for key_value: Variant in keys:
		var key: int = int(key_value)
		var face: int = (key >> 30) & 0x7
		var tile_i: int = key & 0x7FFF
		var tile_j: int = (key >> 15) & 0x7FFF
		var grid_i: float = float(tile_i * SPARSE_TILE_SIDE_CELLS) \
			+ float(SPARSE_TILE_SIDE_CELLS) * 0.5
		var grid_j: float = float(tile_j * SPARSE_TILE_SIDE_CELLS) \
			+ float(SPARSE_TILE_SIDE_CELLS) * 0.5
		var tile_dir: Vector3 = Deltas.lattice_to_dir(face, grid_i, grid_j)
		var denom: float = tile_dir.dot(center_dir)
		if denom <= 0.01:
			continue
		var scale_m: float = Planet.cfg.planet_radius / denom
		var plane_x_m: float = tile_dir.dot(center_right) * scale_m
		var plane_y_m: float = tile_dir.dot(center_up) * scale_m
		if absf(plane_x_m) > HALF_EXTENT_M + tile_radius_m \
				or absf(plane_y_m) > HALF_EXTENT_M + tile_radius_m:
			continue
		var px: int = int(round(plane_x_m / SAMPLE_SPACING_M + half_pixel))
		var py: int = int(round(plane_y_m / SAMPLE_SPACING_M + half_pixel))
		var x0: int = clampi(px - pad_px, 0, RESOLUTION)
		var y0: int = clampi(py - pad_px, 0, RESOLUTION)
		var x1: int = clampi(px + pad_px + 1, 0, RESOLUTION)
		var y1: int = clampi(py + pad_px + 1, 0, RESOLUTION)
		if x1 > x0 and y1 > y0:
			rects.append(Rect2i(x0, y0, x1 - x0, y1 - y0))
	return _merge_sparse_rects(rects)


func _merge_sparse_rects(input_rects: Array[Rect2i]) -> Array[Rect2i]:
	var merged: Array[Rect2i] = []
	for source_rect: Rect2i in input_rects:
		var candidate: Rect2i = source_rect
		var merged_again := true
		while merged_again:
			merged_again = false
			for index: int in range(merged.size() - 1, -1, -1):
				var existing: Rect2i = merged[index]
				if existing.grow(SPARSE_RECT_MERGE_PAD_PX).intersects(candidate):
					candidate = existing.merge(candidate)
					merged.remove_at(index)
					merged_again = true
		merged.append(candidate)
	return merged


func sample_params() -> Dictionary:
	return {
		"texture": texture,
		"ready": ready_state and _center_valid and _local_content,
		"center_dir": center_dir,
		"center_right": center_right,
		"center_up": center_up,
		"half_extent_m": HALF_EXTENT_M,
		"double_buffered": true,
		"partial_updates": true,
		"sparse_recenters": true,
		"generation": generation,
	}


func _sync_renderer_binding() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		return
	var value: Variant = terrain.get("_material")
	if not (value is ShaderMaterial):
		return
	var material: ShaderMaterial = value as ShaderMaterial
	var ready_now: bool = ready_state and _center_valid and _local_content
	var material_changed: bool = material != _opt_bound_material
	var generation_changed: bool = generation != _opt_bound_generation
	var ready_changed: bool = ready_now != _opt_bound_ready
	if not material_changed and not generation_changed and not ready_changed:
		return
	_opt_bound_material = material
	material.set_shader_parameter("u_edit_delta", texture)
	material.set_shader_parameter("u_edit_ready", 1.0 if ready_now else 0.0)
	material.set_shader_parameter("u_edit_center_dir", center_dir)
	material.set_shader_parameter("u_edit_center_right", center_right)
	material.set_shader_parameter("u_edit_center_up", center_up)
	material.set_shader_parameter("u_edit_half_extent_m", HALF_EXTENT_M)
	_opt_bound_generation = generation
	_opt_bound_ready = ready_now


func stats() -> Dictionary:
	return {
		"sparse_recenters": _sparse_recenters,
		"dense_recenters": _dense_recenters,
		"empty_recenters": _empty_recenters,
		"local_content": _local_content,
		"last_recenter_sampled_pixels": _last_recenter_sampled_pixels,
		"full_window_pixels": RESOLUTION * RESOLUTION,
	}
