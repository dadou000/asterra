extends Node
## Renderer gate for cluster mouth flags: external exchange only at cluster ends.

const TILE_RES := 8
const DX := 2.0
const CENTER := Vector2(4.0, 4.0)
const DIRECTION := Vector2(1.0, 0.0)
const HALF_WIDTH_M := 2.0
const ADD_M3 := 4.0
const DT := 0.25
const TOL := 3.0e-3
const STATE_TOL := 2.0e-5
const TIMEOUT_FRAMES := 1200

var atlas: SparseHydroAtlasGPU
var exchange: HydroRiverReachExchangeGPU
var readback: HydroStateReadback
var before := PackedFloat32Array()
var result: Array[Dictionary] = []
var frames := 0
var finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_RIVER_CLUSTER_BOUNDARY_EXCHANGE: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	before = _initial_state()
	atlas = SparseHydroAtlasGPU.new()
	add_child(atlas)
	atlas.initialized.connect(_on_atlas_initialized)
	atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas init %d" % int(error)))
	if atlas.initialize(3, TILE_RES, DX, before) != OK:
		_fail("atlas initialize rejected")


func _process(_delta: float) -> void:
	if finished:
		return
	frames += 1
	if frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _on_atlas_initialized() -> void:
	# initialize() already populated A/B, but explicitly publish all three slots.
	if atlas.set_occupancy(PackedInt32Array([1, 1, 1])) != OK:
		_fail("occupancy publish failed")
		return
	exchange = HydroRiverReachExchangeGPU.new()
	add_child(exchange)
	exchange.initialized.connect(_on_exchange_initialized)
	exchange.initialization_failed.connect(func(error: Error):
		_fail("exchange init %d" % int(error)))
	exchange.exchange_ready.connect(_on_exchange_ready)
	exchange.exchange_failed.connect(func(_id: int, error: Error):
		_fail("exchange failed %d" % int(error)))
	if exchange.initialize(atlas, 4) != OK:
		_fail("exchange initialize rejected")


func _on_exchange_initialized() -> void:
	var records: Array[Dictionary] = [
		{
			"cell": 100, "slot": 0, "center_cell": CENTER,
			"direction_cell": DIRECTION, "half_width_m": HALF_WIDTH_M,
			"add_volume_m3": ADD_M3, "exchange_dt_s": DT, "mouth_cells": 1.25,
			"add_velocity": DIRECTION, "upstream_enabled": true,
			"downstream_enabled": false,
		},
		{
			"cell": 101, "slot": 1, "center_cell": CENTER,
			"direction_cell": DIRECTION, "half_width_m": HALF_WIDTH_M,
			"add_volume_m3": ADD_M3, "exchange_dt_s": DT, "mouth_cells": 1.25,
			"add_velocity": DIRECTION, "upstream_enabled": false,
			"downstream_enabled": false,
		},
		{
			"cell": 102, "slot": 2, "center_cell": CENTER,
			"direction_cell": DIRECTION, "half_width_m": HALF_WIDTH_M,
			"add_volume_m3": ADD_M3, "exchange_dt_s": DT, "mouth_cells": 1.25,
			"add_velocity": DIRECTION, "upstream_enabled": false,
			"downstream_enabled": true,
		},
	]
	if exchange.exchange(records) < 0:
		_fail("exchange request rejected")


func _on_exchange_ready(_request_id: int, results: Array[Dictionary]) -> void:
	if results.size() != 3:
		_fail("unexpected result count %d" % results.size())
		return
	result = results
	if absf(float(results[0].get("added_m3", 0.0)) - ADD_M3) > TOL:
		_fail("upstream boundary did not receive exact 1D parcel")
		return
	if float(results[0].get("removed_m3", 0.0)) > TOL \
			or float(results[0].get("measured_downstream_q_m3s", 0.0)) > TOL:
		_fail("upstream-only member incorrectly executed downstream exchange")
		return
	if absf(float(results[1].get("added_m3", 0.0))) > TOL \
			or absf(float(results[1].get("removed_m3", 0.0))) > TOL \
			or absf(float(results[1].get("measured_downstream_q_m3s", 0.0))) > TOL:
		_fail("internal member touched coarse exchange ledger")
		return
	if absf(float(results[2].get("added_m3", 0.0))) > TOL \
			or float(results[2].get("removed_m3", 0.0)) <= 0.0 \
			or float(results[2].get("measured_downstream_q_m3s", 0.0)) <= 0.0:
		_fail("downstream-only member did not perform measured outflow")
		return
	readback = HydroStateReadback.new()
	add_child(readback)
	readback.state_ready.connect(_on_state_ready)
	readback.readback_failed.connect(func(_id: int, error: Error):
		_fail("state readback %d" % int(error)))
	if readback.request_state(atlas.state_a_rid(), 3 * TILE_RES * TILE_RES) < 0:
		_fail("state readback rejected")


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	var before_v := [_slot_volume(before, 0), _slot_volume(before, 1), _slot_volume(before, 2)]
	var after_v := [_slot_volume(state, 0), _slot_volume(state, 1), _slot_volume(state, 2)]
	if absf(after_v[0] - (before_v[0] + float(result[0]["added_m3"]))) > TOL:
		_fail("upstream slot volume does not match addition")
		return
	if absf(after_v[1] - before_v[1]) > TOL:
		_fail("internal member state changed despite both mouths disabled")
		return
	if absf(after_v[2] - (before_v[2] - float(result[2]["removed_m3"]))) > TOL:
		_fail("downstream slot volume does not match removal")
		return
	# Middle slot must be byte-equivalent within FP tolerance, not just same volume.
	var cells := TILE_RES * TILE_RES
	for i in cells:
		for c in 4:
			var index := (cells + i) * 4 + c
			if absf(state[index] - before[index]) > STATE_TOL:
				_fail("internal member state changed cell=%d component=%d" % [i, c])
				return
	finished = true
	print("HYDRO_RIVER_CLUSTER_BOUNDARY_EXCHANGE: PASS add=", result[0]["added_m3"],
		" remove=", result[2]["removed_m3"])
	_cleanup()
	get_tree().quit(0)


func _initial_state() -> PackedFloat32Array:
	var cells := TILE_RES * TILE_RES
	var state := PackedFloat32Array()
	state.resize(3 * cells * 4)
	for slot in 3:
		for y in TILE_RES:
			for x in TILE_RES:
				var i := slot * cells + y * TILE_RES + x
				state[i * 4 + 3] = 100.0
				if _corridor_contains(x, y):
					state[i * 4] = 1.0
					state[i * 4 + 1] = 1.0
	return state


func _corridor_contains(x: int, y: int) -> bool:
	var p := Vector2(float(x) + 0.5, float(y) + 0.5)
	var rel := p - CENTER
	return absf(rel.y) * DX <= HALF_WIDTH_M + 1.0e-6


func _slot_volume(state: PackedFloat32Array, slot: int) -> float:
	var cells := TILE_RES * TILE_RES
	var total := 0.0
	for i in cells:
		total += maxf(float(state[(slot * cells + i) * 4]), 0.0) * DX * DX
	return total


func _fail(message: String) -> void:
	if finished:
		return
	finished = true
	push_error("HYDRO_RIVER_CLUSTER_BOUNDARY_EXCHANGE: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if exchange != null and is_instance_valid(exchange):
		exchange.release()
	if atlas != null and is_instance_valid(atlas):
		atlas.release()
