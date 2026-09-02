extends Node
## Renderer-mode gate for terrain-aware coarse -> fine prolongation.
##
## A stepped/sloped dry bed receives one exact physical parcel. The resulting fine
## state must conserve that parcel, keep high cells dry, form a level free surface
## across all meaningfully wet cells, preserve bed, carry the requested local
## velocity, and write identical A+B ping-pong state.

const TILE_RES := 8
const DX := 1.0
const REQUESTED_M3 := 5.25
const VELOCITY := Vector2(0.35, -0.18)
const VOLUME_ABS_TOL := 4.0e-4
const ETA_TOL := 2.5e-4
const STATE_TOL := 2.0e-6
const VELOCITY_TOL := 2.0e-5
const WET_EPS := 2.0e-5
const TIMEOUT_FRAMES := 900

var _atlas: SparseHydroAtlasGPU
var _prolong: HydroCoarseProlongationGPU
var _read_a: HydroStateReadback
var _read_b: HydroStateReadback
var _planned_m3 := 0.0
var _state_a := PackedFloat32Array()
var _state_b := PackedFloat32Array()
var _bed := PackedFloat32Array()
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_COARSE_PROLONGATION: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_bed.resize(TILE_RES * TILE_RES)
	var state := PackedFloat32Array()
	state.resize(TILE_RES * TILE_RES * 4)
	for y in TILE_RES:
		for x in TILE_RES:
			var i := y * TILE_RES + x
			# Strong x steps plus a small y slope ensure the parcel cannot wet the
			# whole tile while still exercising non-identical elevations among wet cells.
			var bed := float(x / 2) * 0.60 + float(y) * 0.035 + 1200.0
			_bed[i] = bed
			state[i * 4 + 3] = bed

	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(1, TILE_RES, DX, state)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timeout")


func _on_atlas_initialized() -> void:
	_prolong = HydroCoarseProlongationGPU.new()
	add_child(_prolong)
	_prolong.initialized.connect(_on_prolong_initialized)
	_prolong.initialization_failed.connect(func(error: Error):
		_fail("prolongation initialization failed (%d)" % int(error)))
	_prolong.seed_recorded.connect(_on_seed_recorded)
	_prolong.seed_failed.connect(func(_id: int, error: Error):
		_fail("prolongation seed failed (%d)" % int(error)))
	var err := _prolong.initialize(_atlas)
	if err != OK:
		_fail("prolongation initialize rejected (%d)" % int(err))


func _on_prolong_initialized() -> void:
	var plan := _prolong.plan_volume(REQUESTED_M3)
	_require(int(plan.get("error", FAILED)) == OK, "volume plan failed")
	if _finished:
		return
	_planned_m3 = float(plan.get("represented_volume_m3", 0.0))
	_require(_planned_m3 > 0.0 and _planned_m3 <= REQUESTED_M3,
		"planned parcel is not positive/non-increasing")
	_require(String(plan.get("strategy", "")) == "level_free_surface",
		"unexpected prolongation strategy")
	if _prolong.seed_reserved(0, _planned_m3, VELOCITY) < 0:
		_fail("seed request rejected")


func _on_seed_recorded(_request_id: int, slot: int, represented_volume_m3: float) -> void:
	_require(slot == 0, "seed acknowledged wrong slot")
	_require_close(represented_volume_m3, _planned_m3, VOLUME_ABS_TOL,
		"seed acknowledgement volume")
	if _finished:
		return
	_read_a = HydroStateReadback.new()
	_read_b = HydroStateReadback.new()
	add_child(_read_a)
	add_child(_read_b)
	_read_a.state_ready.connect(func(_id: int, state: PackedFloat32Array):
		_state_a = state; _try_validate())
	_read_b.state_ready.connect(func(_id: int, state: PackedFloat32Array):
		_state_b = state; _try_validate())
	_read_a.readback_failed.connect(func(_id: int, error: Error):
		_fail("state A readback failed (%d)" % int(error)))
	_read_b.readback_failed.connect(func(_id: int, error: Error):
		_fail("state B readback failed (%d)" % int(error)))
	if _read_a.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0 \
			or _read_b.request_state(_atlas.state_b_rid(), _atlas.total_cell_count()) < 0:
		_fail("state readback request rejected")


func _try_validate() -> void:
	if _finished or _state_a.is_empty() or _state_b.is_empty():
		return
	_require(_state_a.size() == TILE_RES * TILE_RES * 4,
		"unexpected A state size")
	_require(_state_b.size() == _state_a.size(), "A/B state sizes differ")
	if _finished:
		return

	var volume := 0.0
	var eta_min := INF
	var eta_max := -INF
	var wet_count := 0
	var dry_count := 0
	for i in TILE_RES * TILE_RES:
		var o := i * 4
		var h := maxf(float(_state_a[o]), 0.0)
		var hu := float(_state_a[o + 1])
		var hv := float(_state_a[o + 2])
		var bed := float(_state_a[o + 3])
		volume += h * DX * DX
		_require_close(bed, float(_bed[i]), STATE_TOL, "bed preservation cell %d" % i)
		for c in 4:
			_require_close(float(_state_b[o + c]), float(_state_a[o + c]),
				STATE_TOL, "A/B mismatch cell %d component %d" % [i, c])
		if h > WET_EPS:
			wet_count += 1
			var eta := bed + h
			eta_min = minf(eta_min, eta)
			eta_max = maxf(eta_max, eta)
			_require_close(hu / h, VELOCITY.x, VELOCITY_TOL,
				"u velocity cell %d" % i)
			_require_close(hv / h, VELOCITY.y, VELOCITY_TOL,
				"v velocity cell %d" % i)
		else:
			dry_count += 1
			_require(absf(hu) <= STATE_TOL and absf(hv) <= STATE_TOL,
				"dry cell carries momentum %d" % i)

	_require_close(volume, _planned_m3, VOLUME_ABS_TOL,
		"terrain-aware represented volume")
	_require(wet_count > 1, "fixture produced too few wet cells")
	_require(dry_count > 0, "high terrain was incorrectly wetted")
	_require(eta_max - eta_min <= ETA_TOL,
		"wet free surface is not level: spread %.9g" % (eta_max - eta_min))
	if _finished:
		return
	_finished = true
	print("HYDRO_COARSE_PROLONGATION: PASS volume=", volume,
		" wet=", wet_count, " dry=", dry_count,
		" eta_spread=", eta_max - eta_min)
	_cleanup()
	get_tree().quit(0)


func _require_close(value: float, reference: float, tolerance: float,
		label: String) -> void:
	if absf(value - reference) > tolerance:
		_fail("%s got %.12g expected %.12g (tol %.6g)" % [
			label, value, reference, tolerance])


func _require(condition: bool, message: String) -> void:
	if not condition and not _finished:
		_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_COARSE_PROLONGATION: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _read_a != null and is_instance_valid(_read_a):
		_read_a.queue_free()
	if _read_b != null and is_instance_valid(_read_b):
		_read_b.queue_free()
	if _prolong != null and is_instance_valid(_prolong):
		_prolong.release()
		_prolong.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
