class_name OceanGPUPhysics
extends Node
## Batched asynchronous GPU water-surface queries for buoyancy/hydrodynamics.
##
## Hull probes are evaluated in compute; Godot's rigid-body solver only receives
## the aggregated forces/torques produced by gameplay code. Three persistent
## buffer slots keep CPU and GPU running in parallel and avoid allocations or an
## rd.sync() stall on every physics tick.

signal batch_ready(request_id: int, results: Array[Dictionary])

const LOCAL_SIZE := 64
const QUERY_FLOATS := 8
const RESULT_FLOATS := 12
const MAX_QUERIES := 512
const SLOT_COUNT := 3

var _rd: RenderingDevice
var _shader: RID
var _pipeline: RID
var _available := false
var _slots: Array[Dictionary] = []
var _next_request_id := 1
var _next_slot := 0
var _latest_results: Array[Dictionary] = []
var _latest_request_id := -1


func _ready() -> void:
	_rd = RenderingServer.create_local_rendering_device()
	if _rd == null:
		return
	var shader_file: RDShaderFile = load("res://shaders/ocean_buoyancy.glsl")
	if shader_file == null:
		return
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	_shader = _rd.shader_create_from_spirv(spirv)
	if not _shader.is_valid():
		return
	_pipeline = _rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
		return
	_create_slots()
	_available = _slots.size() == SLOT_COUNT


func _create_slots() -> void:
	var query_bytes := PackedByteArray()
	query_bytes.resize(MAX_QUERIES * QUERY_FLOATS * 4)
	var result_bytes := PackedByteArray()
	result_bytes.resize(MAX_QUERIES * RESULT_FLOATS * 4)
	var params_bytes := PackedByteArray()
	params_bytes.resize(16)

	for slot_index in SLOT_COUNT:
		var query_buffer := _rd.storage_buffer_create(query_bytes.size(), query_bytes)
		var result_buffer := _rd.storage_buffer_create(result_bytes.size(), result_bytes)
		var params_buffer := _rd.storage_buffer_create(params_bytes.size(), params_bytes)
		if not query_buffer.is_valid() or not result_buffer.is_valid() or not params_buffer.is_valid():
			_free_rid(query_buffer)
			_free_rid(result_buffer)
			_free_rid(params_buffer)
			return

		var uniforms: Array[RDUniform] = []
		uniforms.append(_storage_uniform(0, query_buffer))
		uniforms.append(_storage_uniform(1, result_buffer))
		uniforms.append(_storage_uniform(2, params_buffer))
		var uniform_set := _rd.uniform_set_create(uniforms, _shader, 0)
		if not uniform_set.is_valid():
			_free_rid(query_buffer)
			_free_rid(result_buffer)
			_free_rid(params_buffer)
			return

		_slots.append({
			"query": query_buffer,
			"result": result_buffer,
			"params": params_buffer,
			"uniform_set": uniform_set,
			"busy": false,
			"request_id": -1,
			"count": 0,
			"slot_index": slot_index,
		})


func available() -> bool:
	return _available and Planet.ready_state and Planet.cfg != null


## Queue one batched query without blocking for the result. Returns a monotonically
## increasing request id, or -1 when all three GPU slots are still in flight.
##
## `depths` are still-water depths. `coast_dirs` point landward along the local
## bathymetry gradient. `shore_distances` are signed distances to the zero-height
## coast (negative offshore). Passing these two optional arrays makes the physics
## wave phase match terrain-following visual surf; open-ocean probes may omit them.
func request_batch(points_planet: PackedVector3Array, depths: PackedFloat32Array,
		coast_dirs: PackedVector3Array = PackedVector3Array(),
		shore_distances: PackedFloat32Array = PackedFloat32Array(),
		wave_scale: float = 1.0) -> int:
	var count := mini(mini(points_planet.size(), depths.size()), MAX_QUERIES)
	if count <= 0 or not available():
		return -1

	var slot_index := _find_free_slot()
	if slot_index < 0:
		return -1
	var slot: Dictionary = _slots[slot_index]

	var input := PackedFloat32Array()
	input.resize(count * QUERY_FLOATS)
	for i in count:
		var p := points_planet[i]
		var d := Vector3(0.827, 0.201, 0.525)
		if i < coast_dirs.size() and coast_dirs[i].length_squared() > 1e-8:
			d = coast_dirs[i].normalized()
		var shore_distance := -maxf(depths[i], 0.0) * 8.0
		if i < shore_distances.size():
			shore_distance = shore_distances[i]
		var o := i * QUERY_FLOATS
		input[o] = p.x
		input[o + 1] = p.y
		input[o + 2] = p.z
		input[o + 3] = maxf(depths[i], 0.0)
		input[o + 4] = d.x
		input[o + 5] = d.y
		input[o + 6] = d.z
		input[o + 7] = shore_distance

	var params := PackedFloat32Array([
		Planet.cfg.planet_radius,
		maxf(wave_scale, 0.0),
		float(count),
		float(Time.get_ticks_usec()) / 1000000.0,
	])
	var input_bytes := input.to_byte_array()
	var params_bytes := params.to_byte_array()
	_rd.buffer_update(slot["query"], 0, input_bytes.size(), input_bytes)
	_rd.buffer_update(slot["params"], 0, params_bytes.size(), params_bytes)

	var compute_list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, slot["uniform_set"], 0)
	_rd.compute_list_dispatch(compute_list, int(ceil(float(count) / float(LOCAL_SIZE))), 1, 1)
	_rd.compute_list_end()
	_rd.submit()

	var request_id := _next_request_id
	_next_request_id += 1
	slot["busy"] = true
	slot["request_id"] = request_id
	slot["count"] = count
	_slots[slot_index] = slot
	_next_slot = (slot_index + 1) % SLOT_COUNT

	var byte_count := count * RESULT_FLOATS * 4
	var callback := func(bytes: PackedByteArray) -> void:
		_on_result_ready(slot_index, request_id, count, bytes)
	var err := _rd.buffer_get_data_async(slot["result"], callback, 0, byte_count)
	if err != OK:
		slot["busy"] = false
		_slots[slot_index] = slot
		return -1
	return request_id


func _find_free_slot() -> int:
	for offset in SLOT_COUNT:
		var index := (_next_slot + offset) % SLOT_COUNT
		if not bool(_slots[index]["busy"]):
			return index
	return -1


func _on_result_ready(slot_index: int, request_id: int, count: int,
		bytes: PackedByteArray) -> void:
	if slot_index < 0 or slot_index >= _slots.size():
		return
	var slot: Dictionary = _slots[slot_index]
	# A slot is never reused while busy, so this also rejects a callback left over
	# from device teardown/reinitialisation.
	if int(slot["request_id"]) != request_id:
		return

	var values := bytes.to_float32_array()
	var results: Array[Dictionary] = []
	if values.size() >= count * RESULT_FLOATS:
		results.resize(count)
		for i in count:
			var o := i * RESULT_FLOATS
			results[i] = {
				"height": values[o],
				"normal": Vector3(values[o + 4], values[o + 5], values[o + 6]),
				"velocity": Vector3(values[o + 8], values[o + 9], values[o + 10]),
				"breaking": values[o + 11],
			}

	slot["busy"] = false
	slot["request_id"] = -1
	slot["count"] = 0
	_slots[slot_index] = slot
	_latest_request_id = request_id
	_latest_results = results
	batch_ready.emit(request_id, results)


## Non-blocking access to the most recently completed batch. Physics systems can
## intentionally consume a 2-3-frame-old surface state, which is preferable to
## forcing a CPU/GPU synchronization point every fixed tick.
func latest_results() -> Array[Dictionary]:
	return _latest_results


func latest_request_id() -> int:
	return _latest_request_id


func in_flight_count() -> int:
	var count := 0
	for slot in _slots:
		if bool(slot["busy"]):
			count += 1
	return count


func max_queries() -> int:
	return MAX_QUERIES


func _storage_uniform(binding: int, buffer: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


func _free_rid(rid: RID) -> void:
	if _rd != null and rid.is_valid():
		_rd.free_rid(rid)


func _exit_tree() -> void:
	_available = false
	if _rd == null:
		return
	for slot in _slots:
		_free_rid(slot["uniform_set"])
		_free_rid(slot["query"])
		_free_rid(slot["result"])
		_free_rid(slot["params"])
	_slots.clear()
	_free_rid(_pipeline)
	_free_rid(_shader)
