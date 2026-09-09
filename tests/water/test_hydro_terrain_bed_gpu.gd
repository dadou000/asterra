extends Node
## Renderer-mode gate for direct GPU terrain -> sparse hydrology initialization.
##
## A synthetic six-face RF macro texture gives every cube face a distinct constant
## height. The target +Z tile must reconstruct that face entirely on GPU, add the
## supplied sparse delta patch, clear h/hu/hv, mirror the result into A and B, and
## leave source terms zero without publishing the slot.

const CAPACITY := 1
const TILE_RES := 8
const DX := 1.0
const FACE_RES := 16
const EXPECTED_MACRO := 14.0 # FACE_PZ == 4, synthetic value = 10 + face
const TOL := 2.0e-5
const TIMEOUT_FRAMES := 1200

var _atlas: SparseHydroAtlasGPU
var _terrain: HydroTerrainBedGPU
var _readback: HydroStateReadback
var _macro: Texture2DArray
var _deltas := PackedFloat32Array()
var _stage := 0
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_TERRAIN_BED_GPU: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_macro = _make_macro_texture()
	_require(_macro != null, "synthetic macro texture creation failed")
	if _finished:
		return

	var stale := PackedFloat32Array()
	stale.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	for i in TILE_RES * TILE_RES:
		var o := i * 4
		stale[o] = 73.0
		stale[o + 1] = 900.0
		stale[o + 2] = -700.0
		stale[o + 3] = -123.0

	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(CAPACITY, TILE_RES, DX, stale)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _make_macro_texture() -> Texture2DArray:
	var images: Array[Image] = []
	for face in 6:
		var image := Image.create(FACE_RES, FACE_RES, false, Image.FORMAT_RF)
		image.fill(Color(10.0 + float(face), 0.0, 0.0, 1.0))
		if image.generate_mipmaps() != OK:
			return null
		images.append(image)
	var texture := Texture2DArray.new()
	return texture if texture.create_from_images(images) == OK else null


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out at stage %d" % _stage)


func _on_atlas_initialized() -> void:
	_terrain = HydroTerrainBedGPU.new()
	add_child(_terrain)
	_terrain.initialized.connect(_on_terrain_initialized)
	_terrain.initialization_failed.connect(func(error: Error):
		_fail("terrain bed initialization failed (%d)" % int(error)))
	_terrain.stage_failed.connect(func(_request_id: int, error: Error):
		_fail("terrain bed stage failed (%d)" % int(error)))
	_terrain.stage_recorded.connect(_on_stage_recorded)
	var err := _terrain.initialize(_atlas, _macro, FACE_RES, {
		"planet_radius": 1000.0,
		"base_spacing": 1.0,
		"terrain_level": 0,
		"detail_seed": 12345,
		"detail_strength": 0.0,
	})
	if err != OK:
		_fail("terrain bed initialize rejected (%d)" % int(err))


func _on_terrain_initialized() -> void:
	_deltas.resize(TILE_RES * TILE_RES)
	_deltas.fill(0.0)
	_deltas[0] = 0.25
	_deltas[(TILE_RES >> 1) * TILE_RES + (TILE_RES >> 1)] = -0.50
	var key := HydroTileKey.new(CubeSphere.FACE_PZ, 5, 12, 17)
	var result := _terrain.stage_reserved_tile(key, 0, _deltas)
	_require(bool(result.get("queued", false)) and int(result.get("error", FAILED)) == OK,
		"terrain stage was not queued: %s" % str(result))
	_stage = 1


func _on_stage_recorded(_request_id: int, _tile_id: int, slot: int) -> void:
	_require(slot == 0, "terrain stage reported wrong slot")
	if _finished:
		return
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(_on_readback)
	_readback.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("readback failed (%d)" % int(error)))
	_stage = 2
	if _readback.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0:
		_fail("state A readback rejected")


func _on_readback(_request_id: int, values: PackedFloat32Array) -> void:
	if _stage == 2:
		_validate_state(values, "A")
		if _finished:
			return
		_stage = 3
		if _readback.request_state(_atlas.state_b_rid(), _atlas.total_cell_count()) < 0:
			_fail("state B readback rejected")
		return
	if _stage == 3:
		_validate_state(values, "B")
		if _finished:
			return
		_stage = 4
		if _readback.request_state(_atlas.source_rid(), _atlas.total_cell_count()) < 0:
			_fail("source readback rejected")
		return
	if _stage == 4:
		_validate_sources(values)
		if _finished:
			return
		_finished = true
		print("HYDRO_TERRAIN_BED_GPU: PASS config=", _terrain.configuration())
		_cleanup()
		get_tree().quit(0)


func _validate_state(values: PackedFloat32Array, label: String) -> void:
	_require(values.size() == TILE_RES * TILE_RES * 4,
		"%s state size mismatch" % label)
	if _finished:
		return
	for i in TILE_RES * TILE_RES:
		var o := i * 4
		_require(absf(values[o]) <= TOL and absf(values[o + 1]) <= TOL
			and absf(values[o + 2]) <= TOL,
			"%s cell %d is not dry/zero-momentum: %s" % [label, i,
				str(Vector3(values[o], values[o + 1], values[o + 2]))])
		var expected := EXPECTED_MACRO + _deltas[i]
		_require(absf(values[o + 3] - expected) <= TOL,
			"%s bed mismatch cell=%d got=%.9g expected=%.9g" % [
				label, i, values[o + 3], expected])
		if _finished:
			return


func _validate_sources(values: PackedFloat32Array) -> void:
	_require(values.size() == TILE_RES * TILE_RES * 4, "source state size mismatch")
	if _finished:
		return
	for i in values.size():
		_require(absf(values[i]) <= TOL,
			"source term was not cleared index=%d value=%.9g" % [i, values[i]])
		if _finished:
			return


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_TERRAIN_BED_GPU: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	if _terrain != null and is_instance_valid(_terrain):
		_terrain.release()
		_terrain.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
	_macro = null
