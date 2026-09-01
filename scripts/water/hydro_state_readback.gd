class_name HydroStateReadback
extends Node
## Debug/test-only asynchronous readback of a fixed-domain conservative state.
## Production gameplay must use compact hydrology queries instead of this path.

signal state_ready(request_id: int, state: PackedFloat32Array)
signal readback_failed(request_id: int, error: Error)

var _pending := false
var _next_request_id := 1


func pending() -> bool:
	return _pending


func request_state(state_rid: RID, cell_count: int) -> int:
	if _pending or not state_rid.is_valid() or cell_count <= 0:
		return -1
	if RenderingServer.get_rendering_device() == null:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_pending = true
	var byte_count := cell_count * 4 * 4
	RenderingServer.call_on_render_thread(
		Callable(self, &"_request_render_thread").bind(
			state_rid, byte_count, request_id))
	return request_id


## Render-thread only. Calling this after FixedHydroGPU.advance_recorded means the
## transfer is queued after the already-recorded canonicalizing compute work.
func _request_render_thread(state_rid: RID, byte_count: int, request_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not state_rid.is_valid():
		call_deferred("_fail", request_id, ERR_UNAVAILABLE)
		return
	var callback := Callable(self, &"_on_bytes").bind(request_id)
	var err := rd.buffer_get_data_async(state_rid, callback, 0, byte_count)
	if err != OK:
		call_deferred("_fail", request_id, err)


func _on_bytes(bytes: PackedByteArray, request_id: int) -> void:
	var state := bytes.to_float32_array()
	call_deferred("_publish", request_id, state)


func _publish(request_id: int, state: PackedFloat32Array) -> void:
	_pending = false
	state_ready.emit(request_id, state)


func _fail(request_id: int, error: Error) -> void:
	_pending = false
	readback_failed.emit(request_id, error)
