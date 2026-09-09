extends Node
## Renderer-mode numerical gate for sparse atlas -> shared dynamic-water cache.
## Full texture readback is test-only.

const CAPACITY := 3
const TILE_RES := 8
const LEVEL := 8
const PLANET_RADIUS := 100000.0
const TIMEOUT_FRAMES := 1200

var _atlas: SparseHydroAtlasGPU
var _recon: SparseHydroSurfaceReconstructionGPU
var _source_key: HydroTileKey
var _east_key: HydroTileKey
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_SURFACE_RECONSTRUCTION: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return
	if WaterSystem.dynamic_surface_available():
		_begin()
	else:
		WaterSystem.dynamic_surface_ready.connect(_begin, CONNECT_ONE_SHOT)


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _begin() -> void:
	WaterSystem.set_dynamic_surface_render_enabled(false)
	var side := 1 << LEVEL
	var half_side := side >> 1
	_source_key = HydroTileKey.new(CubeSphere.FACE_PX, LEVEL,
		half_side + 7, half_side - 5)
	var east := HydroTileTopology.neighbor(_source_key, HydroTileTopology.DIR_EAST)
	_require(not east.is_empty(), "east neighbor did not resolve")
	if _finished:
		return
	_east_key = east["key"] as HydroTileKey

	var dx := HydroMetricGrid.compatible_cell_size_m(PLANET_RADIUS, TILE_RES, LEVEL)
	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_tile(state, 0, 5.0, Vector2(1.0, -0.5), -3.0) # eta=2
	_fill_tile(state, 1, 7.0, Vector2(-0.25, 1.25), -4.0) # eta=3
	_fill_tile(state, 2, 80.0, Vector2(500.0, -700.0), 120.0) # stale/unoccupied
	var occupancy := PackedInt32Array([1, 1, 0])
	var metadata := PackedInt32Array()
	metadata.resize(CAPACITY * 4)
	_write_key(metadata, 0, _source_key)
	_write_key(metadata, 1, _east_key)
	_write_key(metadata, 2, HydroTileKey.new(CubeSphere.FACE_NZ, LEVEL, 1, 1))

	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(CAPACITY, TILE_RES, dx, state, occupancy, metadata)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _on_atlas_initialized() -> void:
	var resources := WaterSystem.surface_resources()
	_recon = SparseHydroSurfaceReconstructionGPU.new()
	add_child(_recon)
	_recon.initialized.connect(_on_recon_initialized)
	_recon.initialization_failed.connect(func(error: Error):
		_fail("reconstruction initialization failed (%d)" % int(error)))
	_recon.reconstruction_recorded.connect(_on_reconstruction_recorded)
	_recon.reconstruction_failed.connect(func(_request_id: int, error: Error):
		_fail("reconstruction failed (%d)" % int(error)))
	var err := _recon.initialize(_atlas, resources.field_rid(),
		resources.field_resolution(), resources.field_half_extent_m(),
		LEVEL, PLANET_RADIUS)
	if err != OK:
		_fail("reconstruction initialize rejected (%d)" % int(err))


func _on_recon_initialized() -> void:
	var anchor := HydroTileTopology.edge_center_direction(
		_source_key, HydroTileTopology.DIR_EAST)
	# Move slightly back toward the source center so both source/east tiles are well
	# inside the local cache while avoiding a cube-tile boundary as the exact anchor.
	var source_center_uv := HydroTileTopology.tile_center_face_uv(_source_key)
	var source_center := CubeSphere.face_uv_to_dir(
		_source_key.face, source_center_uv.x, source_center_uv.y)
	anchor = (anchor + source_center * 2.0).normalized()
	var basis := CubeSphere.tangent_basis(anchor)
	WaterSystem.set_dynamic_surface_anchor_direction(anchor)
	WaterSystem.set_dynamic_surface_center_plane(Vector2.ZERO)
	if _recon.reconstruct(anchor, basis[0], basis[1], Vector2.ZERO, 0.5) < 0:
		_fail("reconstruction request rejected")


func _on_reconstruction_recorded(_request_id: int) -> void:
	var rid := WaterSystem.surface_resources().field_rid()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_request_texture_readback_render_thread").bind(rid))


func _request_texture_readback_render_thread(rid: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not rid.is_valid() or not rd.texture_is_valid(rid):
		call_deferred("_fail", "dynamic texture invalid during readback")
		return
	var err := rd.texture_get_data_async(rid, 0, Callable(self, &"_on_texture_bytes"))
	if err != OK:
		call_deferred("_fail", "texture readback request failed (%d)" % int(err))


func _on_texture_bytes(bytes: PackedByteArray) -> void:
	call_deferred("_verify_texture", bytes)


func _verify_texture(bytes: PackedByteArray) -> void:
	var res := WaterSystem.surface_resources().field_resolution()
	_require(bytes.size() == res * res * 16,
		"texture byte count mismatch %d" % bytes.size())
	if _finished:
		return

	var min_nonzero_height := INF
	var max_height := -INF
	var max_speed := 0.0
	var min_depth := INF
	var max_depth := 0.0
	var nonzero := 0
	for i in res * res:
		var o := i * 16
		var eta := bytes.decode_float(o)
		var vx := bytes.decode_float(o + 4)
		var vy := bytes.decode_float(o + 8)
		var depth := bytes.decode_float(o + 12)
		_require(is_finite(eta) and is_finite(vx) and is_finite(vy) and is_finite(depth),
			"non-finite texel %d" % i)
		if _finished:
			return
		if depth > 1.0e-4:
			nonzero += 1
			min_nonzero_height = minf(min_nonzero_height, eta)
			max_height = maxf(max_height, eta)
			max_speed = maxf(max_speed, Vector2(vx, vy).length())
			min_depth = minf(min_depth, depth)
			max_depth = maxf(max_depth, depth)

	_require(nonzero > 16 and nonzero < res * res / 3,
		"resident-footprint masking failed nonzero=%d" % nonzero)
	_require(min_nonzero_height > 1.8 and min_nonzero_height < 2.2,
		"source eta missing/incorrect min=%.6f" % min_nonzero_height)
	_require(max_height > 2.8 and max_height < 3.2,
		"east eta missing/incorrect max=%.6f" % max_height)
	_require(max_height < 10.0,
		"stale unoccupied slot leaked into render cache max=%.6f" % max_height)
	_require(max_speed > 0.9 and max_speed < 2.0,
		"velocity transform magnitude unexpected %.6f" % max_speed)
	_require(min_depth > 4.8 and min_depth < 5.2,
		"source physical depth missing/incorrect %.6f" % min_depth)
	_require(max_depth > 6.8 and max_depth < 7.2,
		"east physical depth missing/incorrect %.6f" % max_depth)
	if _finished:
		return

	_finished = true
	print("SPARSE_HYDRO_SURFACE_RECONSTRUCTION: PASS nonzero=", nonzero,
		" eta=[", min_nonzero_height, ",", max_height, "] depth=[", min_depth,
		",", max_depth, "] speed=", max_speed, " hash=", _recon.hash_size())
	_cleanup()
	get_tree().quit(0)


func _fill_tile(state: PackedFloat32Array, slot: int, depth: float,
		velocity: Vector2, bed: float) -> void:
	var cells := TILE_RES * TILE_RES
	for i in cells:
		var o := (slot * cells + i) * 4
		state[o] = depth
		state[o + 1] = depth * velocity.x
		state[o + 2] = depth * velocity.y
		state[o + 3] = bed


func _write_key(metadata: PackedInt32Array, slot: int, key: HydroTileKey) -> void:
	var o := slot * 4
	metadata[o] = key.face
	metadata[o + 1] = key.level
	metadata[o + 2] = key.x
	metadata[o + 3] = key.y


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("SPARSE_HYDRO_SURFACE_RECONSTRUCTION: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	WaterSystem.set_dynamic_surface_render_enabled(false)
	if _recon != null and is_instance_valid(_recon):
		_recon.release()
		_recon.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
	_recon = null
	_atlas = null
