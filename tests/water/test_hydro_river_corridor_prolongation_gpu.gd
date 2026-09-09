extends Node
## Renderer-mode numerical gate for HydroRiverCorridorProlongationGPU.

const TILE_RES := 8
const DX := 2.0
const TARGET_M3 := 32.0
const ABS_VOLUME_TOL := 2.0e-3
const STATE_TOL := 2.0e-5
const STAGE_TOL := 2.0e-3
const TIMEOUT_FRAMES := 1200
const CENTER := Vector2(4.0, 4.0)
const DIRECTION := Vector2(1.0, 0.0)
const HALF_WIDTH_M := 2.0
const VELOCITY := Vector2(1.20, 0.35)

var _atlas: SparseHydroAtlasGPU
var _seeder: HydroRiverCorridorProlongationGPU
var _read_a: HydroStateReadback
var _read_b: HydroStateReadback
var _target_m3 := 0.0
var _state_a := PackedFloat32Array()
var _state_b := PackedFloat32Array()
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_RIVER_CORRIDOR_PROLONGATION: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var initial := _initial_state()
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(1, TILE_RES, DX, initial)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _on_atlas_initialized() -> void:
	_seeder = HydroRiverCorridorProlongationGPU.new()
	add_child(_seeder)
	_seeder.initialized.connect(_on_seeder_initialized)
	_seeder.initialization_failed.connect(func(error: Error):
		_fail("seeder initialization failed (%d)" % int(error)))
	_seeder.seed_recorded.connect(_on_seed_recorded)
	_seeder.seed_failed.connect(func(_request_id: int, error: Error):
		_fail("seed failed (%d)" % int(error)))
	var err := _seeder.initialize(_atlas)
	if err != OK:
		_fail("seeder initialize rejected (%d)" % int(err))


func _on_seeder_initialized() -> void:
	var plan := _seeder.plan_volume(TARGET_M3)
	if int(plan.get("error", FAILED)) != OK:
		_fail("volume plan rejected")
		return
	_target_m3 = float(plan["represented_volume_m3"])
	var request := _seeder.seed_reserved(0, _target_m3,
		CENTER, DIRECTION, HALF_WIDTH_M, VELOCITY)
	if request < 0:
		_fail("seed request rejected")


func _on_seed_recorded(_request_id: int, _slot: int, represented_volume_m3: float) -> void:
	if absf(represented_volume_m3 - _target_m3) > 1.0e-6:
		_fail("seed acknowledgement volume mismatch")
		return
	_read_a = HydroStateReadback.new()
	_read_b = HydroStateReadback.new()
	add_child(_read_a)
	add_child(_read_b)
	_read_a.state_ready.connect(func(_id: int, state: PackedFloat32Array):
		_state_a = state
		_try_validate())
	_read_b.state_ready.connect(func(_id: int, state: PackedFloat32Array):
		_state_b = state
		_try_validate())
	_read_a.readback_failed.connect(func(_id: int, error: Error):
		_fail("A readback failed (%d)" % int(error)))
	_read_b.readback_failed.connect(func(_id: int, error: Error):
		_fail("B readback failed (%d)" % int(error)))
	if _read_a.request_state(_atlas.state_a_rid(), TILE_RES * TILE_RES) < 0:
		_fail("A readback request rejected")
	if _read_b.request_state(_atlas.state_b_rid(), TILE_RES * TILE_RES) < 0:
		_fail("B readback request rejected")


func _try_validate() -> void:
	if _finished or _state_a.is_empty() or _state_b.is_empty():
		return
	var volume := 0.0
	var wet_count := 0
	var dry_outside_count := 0
	var reference_stage := NAN
	var high_ridge_wet := false
	for y in TILE_RES:
		for x in TILE_RES:
			var i := y * TILE_RES + x
			var o := i * 4
			for c in 4:
				if absf(_state_a[o + c] - _state_b[o + c]) > STATE_TOL:
					_fail("A/B mismatch at cell %d component %d" % [i, c])
					return
			var h := float(_state_a[o])
			var hu := float(_state_a[o + 1])
			var hv := float(_state_a[o + 2])
			var bed := float(_state_a[o + 3])
			volume += h * DX * DX
			var eligible := _corridor_contains(x, y)
			if not eligible:
				if h > STATE_TOL:
					_fail("off-corridor cell became wet at (%d,%d)" % [x, y])
					return
				dry_outside_count += 1
				continue
			if x == 4 and y == 3 and h > STATE_TOL:
				high_ridge_wet = true
			if h <= STATE_TOL:
				continue
			wet_count += 1
			var stage := bed + h
			if is_nan(reference_stage):
				reference_stage = stage
			elif absf(stage - reference_stage) > STAGE_TOL:
				_fail("wet corridor stage is not level")
				return
			if absf(hu / h - VELOCITY.x) > 2.0e-4 \
					or absf(hv / h - VELOCITY.y) > 2.0e-4:
				_fail("wet corridor momentum does not match downstream velocity")
				return

	if absf(volume - _target_m3) > ABS_VOLUME_TOL:
		_fail("corridor volume mismatch got %.9g expected %.9g" % [volume, _target_m3])
		return
	if wet_count < 4:
		_fail("too few wet corridor cells (%d)" % wet_count)
		return
	if dry_outside_count <= 0:
		_fail("fixture did not exercise off-corridor dry masking")
		return
	if high_ridge_wet:
		_fail("high in-corridor terrain ridge should remain dry")
		return
	_finished = true
	print("HYDRO_RIVER_CORRIDOR_PROLONGATION: PASS volume=", volume,
		" wet_cells=", wet_count)
	_cleanup()
	get_tree().quit(0)


func _corridor_contains(x: int, y: int) -> bool:
	var p := Vector2(float(x) + 0.5, float(y) + 0.5)
	var rel := p - CENTER
	var dir := DIRECTION.normalized()
	var perpendicular := absf(rel.x * dir.y - rel.y * dir.x)
	return perpendicular <= HALF_WIDTH_M / DX + 1.0e-6


func _initial_state() -> PackedFloat32Array:
	var state := PackedFloat32Array()
	state.resize(TILE_RES * TILE_RES * 4)
	for y in TILE_RES:
		for x in TILE_RES:
			var i := y * TILE_RES + x
			var bed := 100.0 + float(x) * 0.08 + float(y) * 0.015
			# One high terrain obstruction inside the corridor; the level solve should
			# naturally leave it dry rather than forcing a uniform river depth.
			if x == 4 and y == 3:
				bed += 5.0
			state[i * 4 + 3] = bed
	return state


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_RIVER_CORRIDOR_PROLONGATION: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _seeder != null and is_instance_valid(_seeder):
		_seeder.release()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
