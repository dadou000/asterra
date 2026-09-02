extends Node
## Renderer-mode gate for published-weather -> atmospheric source forcing and for
## additive composition with the existing gameplay source layer.

const CAPACITY := 3
const TILE_RES := 8
const LEVEL := 8
const DX := 4.0
const WEATHER_W := 32
const WEATHER_H := 16
const TIMEOUT_FRAMES := 1200
const GAMEPLAY_RATE_MPS := 1.0e-5
const LAND_EXPECTED_MPS := 26.0 * 1.0e-3 / 3600.0
const OCEAN_EXPECTED_MPS := 30.0 * 1.0e-3 / 3600.0
const RATE_TOL := 2.0e-8
const DEPTH_TOL := 5.0e-8

var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU
var _solver: SparseHydroStepGPU
var _forcing: HydroWeatherForcingGPU
var _gameplay_writer: HydroSourceTermsGPU
var _weather_texture: ImageTexture
var _atlas_ready := false
var _connectivity_ready := false
var _frames := 0
var _finished := false
var _solver_step_id := -1


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_WEATHER_FORCING: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return
	_begin()


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _begin() -> void:
	var weather_image := Image.create(WEATHER_W, WEATHER_H, false, Image.FORMAT_RGBAF)
	weather_image.fill(Color(0.2, 0.4, 1.0, 0.5))
	_weather_texture = ImageTexture.create_from_image(weather_image)

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_bed(state, 0, 10.0)
	_fill_bed(state, 1, -10.0)
	_fill_bed(state, 2, 50.0)
	var occupancy := PackedInt32Array([1, 1, 0])
	var metadata := PackedInt32Array()
	metadata.resize(CAPACITY * 4)
	_write_key(metadata, 0, HydroTileKey.new(CubeSphere.FACE_PX, LEVEL, 120, 120))
	_write_key(metadata, 1, HydroTileKey.new(CubeSphere.FACE_PX, LEVEL, 121, 120))
	_write_key(metadata, 2, HydroTileKey.new(CubeSphere.FACE_NZ, LEVEL, 12, 14))

	_atlas = SparseHydroAtlasGPU.new()
	_atlas.name = "WeatherForcingAtlas"
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var atlas_err := _atlas.initialize(CAPACITY, TILE_RES, DX, state, occupancy, metadata)
	if atlas_err != OK:
		_fail("atlas initialize rejected (%d)" % int(atlas_err))
		return

	_connectivity = SparseHydroConnectivityGPU.new()
	_connectivity.name = "WeatherForcingConnectivity"
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity initialization failed (%d)" % int(error)))
	var connectivity_err := _connectivity.initialize(CAPACITY)
	if connectivity_err != OK:
		_fail("connectivity initialize rejected (%d)" % int(connectivity_err))


func _on_atlas_initialized() -> void:
	_atlas_ready = true
	_try_initialize_solver()


func _on_connectivity_initialized() -> void:
	_connectivity_ready = true
	_try_initialize_solver()


func _try_initialize_solver() -> void:
	if not _atlas_ready or not _connectivity_ready or _solver != null or _finished:
		return
	_solver = SparseHydroStepGPU.new()
	_solver.name = "WeatherForcingSolver"
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(func(error: Error):
		_fail("solver initialization failed (%d)" % int(error)))
	_solver.diagnostics_ready.connect(_on_solver_diagnostics)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("solver initialize rejected (%d)" % int(err))


func _on_solver_initialized() -> void:
	_require(_solver.atmospheric_source_rid().is_valid(),
		"solver did not expose atmospheric source RID")
	if _finished:
		return
	_forcing = HydroWeatherForcingGPU.new()
	_forcing.name = "WeatherForcingWriter"
	add_child(_forcing)
	_forcing.initialized.connect(_on_forcing_initialized)
	_forcing.initialization_failed.connect(func(error: Error):
		_fail("forcing initialization failed (%d)" % int(error)))
	_forcing.update_recorded.connect(_on_forcing_recorded)
	_forcing.update_failed.connect(func(_request_id: int, error: Error):
		_fail("forcing update failed (%d)" % int(error)))
	_forcing.maximum_precipitation_mm_h = 30.0
	_forcing.land_infiltration_capacity_mm_h = 4.0
	_forcing.weather_gain = 1.0
	_forcing.land_bed_threshold_m = 0.0
	var err := _forcing.initialize(_atlas, _solver.atmospheric_source_rid(), _weather_texture)
	if err != OK:
		_fail("forcing initialize rejected (%d)" % int(err))


func _on_forcing_initialized() -> void:
	if _forcing.request_update() < 0:
		_fail("forcing update request rejected")


func _on_forcing_recorded(_request_id: int) -> void:
	_request_buffer_readback(_solver.atmospheric_source_rid(),
		Callable(self, &"_on_forcing_bytes"))


func _on_forcing_bytes(bytes: PackedByteArray) -> void:
	call_deferred("_verify_forcing", bytes)


func _verify_forcing(bytes: PackedByteArray) -> void:
	var expected_bytes := CAPACITY * TILE_RES * TILE_RES * 16
	_require(bytes.size() == expected_bytes,
		"forcing byte count %d != %d" % [bytes.size(), expected_bytes])
	if _finished:
		return
	var cells := TILE_RES * TILE_RES
	for slot in CAPACITY:
		for cell in cells:
			var o := (slot * cells + cell) * 16
			var add_rate := bytes.decode_float(o)
			var remove_rate := bytes.decode_float(o + 4)
			var hu_rate := bytes.decode_float(o + 8)
			var hv_rate := bytes.decode_float(o + 12)
			var expected := 0.0
			if slot == 0:
				expected = LAND_EXPECTED_MPS
			elif slot == 1:
				expected = OCEAN_EXPECTED_MPS
			_require(absf(add_rate - expected) <= RATE_TOL,
				"slot %d cell %d add %.10g != %.10g" % [slot, cell, add_rate, expected])
			_require(absf(remove_rate) <= RATE_TOL and absf(hu_rate) <= RATE_TOL \
					and absf(hv_rate) <= RATE_TOL,
				"unexpected non-add forcing at slot %d cell %d" % [slot, cell])
			if _finished:
				return
	_begin_composition_gate()


func _begin_composition_gate() -> void:
	_gameplay_writer = HydroSourceTermsGPU.new()
	_gameplay_writer.name = "WeatherGameplaySourceWriter"
	add_child(_gameplay_writer)
	_gameplay_writer.initialized.connect(_on_gameplay_writer_initialized)
	_gameplay_writer.initialization_failed.connect(func(error: Error):
		_fail("gameplay writer initialization failed (%d)" % int(error)))
	_gameplay_writer.update_recorded.connect(_on_gameplay_source_recorded)
	_gameplay_writer.update_failed.connect(func(_request_id: int, error: Error):
		_fail("gameplay writer update failed (%d)" % int(error)))
	var err := _gameplay_writer.initialize(_atlas, 8)
	if err != OK:
		_fail("gameplay writer initialize rejected (%d)" % int(err))


func _on_gameplay_writer_initialized() -> void:
	var entries: Array[Dictionary] = [{
		"slot": 0,
		"cell": 0,
		"add_depth_rate_mps": GAMEPLAY_RATE_MPS,
		"remove_depth_rate_mps": 0.0,
		"hu_rate": 0.0,
		"hv_rate": 0.0,
	}]
	if _gameplay_writer.update_entries(entries) < 0:
		_fail("gameplay source update rejected")


func _on_gameplay_source_recorded(_request_id: int, _entry_count: int) -> void:
	_solver_step_id = _solver.advance(1.0, 1, true)
	if _solver_step_id < 0:
		_fail("solver composition advance rejected")


func _on_solver_diagnostics(step_id: int, diagnostics: Dictionary) -> void:
	if step_id != _solver_step_id or _finished:
		return
	_require(int(diagnostics.get("post_invalid_cells", 0)) == 0,
		"solver reported invalid cells")
	_require(int(diagnostics.get("steps_taken", 0)) == 1,
		"composition fixture did not take exactly one step")
	var advanced := float(diagnostics.get("advanced_dt_s", 0.0))
	_require(absf(advanced - 1.0) <= 1.0e-6,
		"composition fixture advanced %.9f s instead of 1 s" % advanced)
	if _finished:
		return
	_request_buffer_readback(_atlas.state_a_rid(), Callable(self, &"_on_state_bytes"))


func _on_state_bytes(bytes: PackedByteArray) -> void:
	call_deferred("_verify_composed_state", bytes)


func _verify_composed_state(bytes: PackedByteArray) -> void:
	var expected_bytes := CAPACITY * TILE_RES * TILE_RES * 16
	_require(bytes.size() == expected_bytes,
		"state byte count %d != %d" % [bytes.size(), expected_bytes])
	if _finished:
		return

	var land_cell0 := bytes.decode_float(0)
	var land_cell1 := bytes.decode_float(16)
	var ocean_base := TILE_RES * TILE_RES * 16
	var ocean_cell0 := bytes.decode_float(ocean_base)
	var expected_composed := LAND_EXPECTED_MPS + GAMEPLAY_RATE_MPS
	_require(absf(land_cell0 - expected_composed) <= DEPTH_TOL,
		"source composition %.10g != %.10g" % [land_cell0, expected_composed])
	_require(absf(land_cell1 - LAND_EXPECTED_MPS) <= DEPTH_TOL,
		"weather-only land depth %.10g != %.10g" % [land_cell1, LAND_EXPECTED_MPS])
	_require(absf(ocean_cell0 - OCEAN_EXPECTED_MPS) <= DEPTH_TOL,
		"weather-only ocean depth %.10g != %.10g" % [ocean_cell0, OCEAN_EXPECTED_MPS])
	if _finished:
		return

	_finished = true
	print("HYDRO_WEATHER_FORCING: PASS land_mps=", LAND_EXPECTED_MPS,
		" ocean_mps=", OCEAN_EXPECTED_MPS,
		" composed_depth=", land_cell0)
	_cleanup()
	get_tree().quit(0)


func _request_buffer_readback(rid: RID, callback: Callable) -> void:
	RenderingServer.call_on_render_thread(
		Callable(self, &"_request_buffer_readback_render_thread").bind(rid, callback))


func _request_buffer_readback_render_thread(rid: RID, callback: Callable) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not rid.is_valid():
		call_deferred("_fail", "invalid RID during buffer readback")
		return
	var err := rd.buffer_get_data_async(rid, callback)
	if err != OK:
		call_deferred("_fail", "buffer readback request failed (%d)" % int(err))


func _fill_bed(state: PackedFloat32Array, slot: int, bed: float) -> void:
	var cells := TILE_RES * TILE_RES
	for cell in cells:
		var o := (slot * cells + cell) * 4
		state[o] = 0.0
		state[o + 1] = 0.0
		state[o + 2] = 0.0
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
	push_error("HYDRO_WEATHER_FORCING: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _forcing != null and is_instance_valid(_forcing):
		_forcing.release()
		_forcing.queue_free()
	if _gameplay_writer != null and is_instance_valid(_gameplay_writer):
		_gameplay_writer.release()
		_gameplay_writer.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
		_connectivity.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
	_forcing = null
	_gameplay_writer = null
	_solver = null
	_connectivity = null
	_atlas = null
	_weather_texture = null
