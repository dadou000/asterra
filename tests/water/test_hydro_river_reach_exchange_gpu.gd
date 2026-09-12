extends Node
## Renderer-mode gate for operator-split river mouth exchange.

const TILE_RES := 8
const DX := 2.0
const CENTER := Vector2(4.0, 4.0)
const DIRECTION := Vector2(1.0, 0.0)
const HALF_WIDTH_M := 2.0
const ADD_VOLUME_M3 := 4.0
const DT := 0.25
const VELOCITY := Vector2(1.0, 0.0)
const TIMEOUT_FRAMES := 1200
const VOLUME_TOL := 3.0e-3
const STATE_TOL := 2.0e-5

var atlas: SparseHydroAtlasGPU
var exchange: HydroRiverReachExchangeGPU
var read_a: HydroStateReadback
var read_b: HydroStateReadback
var initial_volume := 0.0
var result: Dictionary = {}
var state_a := PackedFloat32Array()
var state_b := PackedFloat32Array()
var frames := 0
var finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_RIVER_REACH_EXCHANGE: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var initial := _initial_state()
	initial_volume = _volume(initial)
	atlas = SparseHydroAtlasGPU.new(); add_child(atlas)
	atlas.initialized.connect(func(): _on_atlas_initialized(initial))
	atlas.initialization_failed.connect(func(error: Error): _fail("atlas init %d" % int(error)))
	var err := atlas.initialize(1, TILE_RES, DX, initial)
	if err != OK: _fail("atlas initialize rejected %d" % int(err))


func _process(_delta: float) -> void:
	if finished: return
	frames += 1
	if frames > TIMEOUT_FRAMES: _fail("timed out")


func _on_atlas_initialized(initial: PackedFloat32Array) -> void:
	if atlas.stage_slot_state(0, initial) != OK:
		_fail("failed to stage A/B initial state"); return
	if atlas.set_occupancy(PackedInt32Array([1])) != OK:
		_fail("failed to publish occupancy"); return
	exchange = HydroRiverReachExchangeGPU.new(); add_child(exchange)
	exchange.initialized.connect(_on_exchange_initialized)
	exchange.initialization_failed.connect(func(error: Error): _fail("exchange init %d" % int(error)))
	exchange.exchange_ready.connect(_on_exchange_ready)
	exchange.exchange_failed.connect(func(_id: int, error: Error): _fail("exchange failed %d" % int(error)))
	var err := exchange.initialize(atlas, 4)
	if err != OK: _fail("exchange initialize rejected %d" % int(err))


func _on_exchange_initialized() -> void:
	var request := exchange.exchange([{
		"cell": 7,
		"slot": 0,
		"center_cell": CENTER,
		"direction_cell": DIRECTION,
		"half_width_m": HALF_WIDTH_M,
		"add_volume_m3": ADD_VOLUME_M3,
		"exchange_dt_s": DT,
		"mouth_cells": 1.25,
		"add_velocity": VELOCITY,
	}])
	if request < 0: _fail("exchange request rejected")


func _on_exchange_ready(_request_id: int, results: Array[Dictionary]) -> void:
	if results.size() != 1:
		_fail("unexpected result count %d" % results.size()); return
	result = results[0]
	if int(result.get("cell", -1)) != 7:
		_fail("result cell identity mismatch"); return
	if float(result.get("mouth_or_status", -1.0)) < 0.0:
		_fail("GPU mouth classification failed"); return
	if absf(float(result.get("added_m3", 0.0)) - ADD_VOLUME_M3) > VOLUME_TOL:
		_fail("upstream addition is not exact"); return
	if float(result.get("removed_m3", 0.0)) <= 0.0 \
			or float(result.get("measured_downstream_q_m3s", 0.0)) <= 0.0:
		_fail("downstream advective exchange was not measured/removed"); return
	read_a = HydroStateReadback.new(); read_b = HydroStateReadback.new()
	add_child(read_a); add_child(read_b)
	read_a.state_ready.connect(func(_id: int, s: PackedFloat32Array): state_a = s; _validate())
	read_b.state_ready.connect(func(_id: int, s: PackedFloat32Array): state_b = s; _validate())
	read_a.readback_failed.connect(func(_id: int, error: Error): _fail("A readback %d" % int(error)))
	read_b.readback_failed.connect(func(_id: int, error: Error): _fail("B readback %d" % int(error)))
	if read_a.request_state(atlas.state_a_rid(), TILE_RES * TILE_RES) < 0: _fail("A request rejected")
	if read_b.request_state(atlas.state_b_rid(), TILE_RES * TILE_RES) < 0: _fail("B request rejected")


func _validate() -> void:
	if finished or state_a.is_empty() or state_b.is_empty(): return
	for i in TILE_RES * TILE_RES:
		for c in 4:
			if absf(state_a[i * 4 + c] - state_b[i * 4 + c]) > STATE_TOL:
				_fail("A/B mismatch cell %d component %d" % [i, c]); return
	var final_volume := _volume(state_a)
	var expected := initial_volume + float(result["added_m3"]) - float(result["removed_m3"])
	if absf(final_volume - expected) > VOLUME_TOL:
		_fail("fine volume balance mismatch got %.9g expected %.9g" % [final_volume, expected]); return
	# Off-corridor cells must remain dry.
	for y in TILE_RES:
		for x in TILE_RES:
			if _corridor_contains(x, y): continue
			if state_a[(y * TILE_RES + x) * 4] > STATE_TOL:
				_fail("off-corridor exchange created water at %d,%d" % [x, y]); return
	finished = true
	print("HYDRO_RIVER_REACH_EXCHANGE: PASS added=", result["added_m3"],
		" removed=", result["removed_m3"], " q=", result["measured_downstream_q_m3s"])
	_cleanup(); get_tree().quit(0)


func _initial_state() -> PackedFloat32Array:
	var state := PackedFloat32Array(); state.resize(TILE_RES * TILE_RES * 4)
	for y in TILE_RES:
		for x in TILE_RES:
			var i := y * TILE_RES + x
			state[i * 4 + 3] = 100.0 + float(x) * 0.02
			if _corridor_contains(x, y):
				state[i * 4] = 1.0
				state[i * 4 + 1] = 1.0
	return state


func _corridor_contains(x: int, y: int) -> bool:
	var p := Vector2(float(x) + 0.5, float(y) + 0.5)
	var rel := p - CENTER
	var dir := DIRECTION.normalized()
	return absf(rel.x * dir.y - rel.y * dir.x) * DX <= HALF_WIDTH_M + 1.0e-6


func _volume(state: PackedFloat32Array) -> float:
	var v := 0.0
	for i in TILE_RES * TILE_RES: v += maxf(float(state[i * 4]), 0.0) * DX * DX
	return v


func _fail(message: String) -> void:
	if finished: return
	finished = true; push_error("HYDRO_RIVER_REACH_EXCHANGE: " + message)
	_cleanup(); get_tree().quit(1)


func _cleanup() -> void:
	if exchange != null and is_instance_valid(exchange): exchange.release()
	if atlas != null and is_instance_valid(atlas): atlas.release()
