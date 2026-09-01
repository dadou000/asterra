class_name HydrologyQueryService
extends Node
## Stable water-query facade for gameplay systems.
##
## Phase 1 deliberately wraps the existing OceanGPUPhysics implementation rather
## than replacing it. This gives hydrology one public query API immediately while
## preserving the proven asynchronous triple-buffered backend. Later phases can
## swap in the active-tile hydrology query backend without changing callers.

signal batch_ready(request_id: int, results: Array[Dictionary])
signal backend_changed(backend: Node)

var _backend: Node


func bind_backend(backend: Node) -> void:
	if backend == _backend:
		return
	_unbind_backend_signal()
	_backend = backend
	_bind_backend_signal()
	backend_changed.emit(_backend)


func clear_backend() -> void:
	bind_backend(null)


func backend() -> Node:
	return _backend


func available() -> bool:
	return _backend != null \
		and is_instance_valid(_backend) \
		and _backend.has_method(&"available") \
		and bool(_backend.call(&"available"))


func request_batch(points_planet: PackedVector3Array, depths: PackedFloat32Array,
		coast_dirs: PackedVector3Array = PackedVector3Array(),
		shore_distances: PackedFloat32Array = PackedFloat32Array(),
		wave_scale: float = 1.0) -> int:
	if _backend == null or not is_instance_valid(_backend):
		return -1
	if not _backend.has_method(&"request_batch"):
		return -1
	return int(_backend.call(&"request_batch", points_planet, depths, coast_dirs,
		shore_distances, wave_scale))


func latest_results() -> Array[Dictionary]:
	if _backend == null or not is_instance_valid(_backend):
		return []
	if not _backend.has_method(&"latest_results"):
		return []
	var value: Variant = _backend.call(&"latest_results")
	if value is Array:
		return value as Array[Dictionary]
	return []


func latest_request_id() -> int:
	if _backend == null or not is_instance_valid(_backend):
		return -1
	if not _backend.has_method(&"latest_request_id"):
		return -1
	return int(_backend.call(&"latest_request_id"))


func in_flight_count() -> int:
	if _backend == null or not is_instance_valid(_backend):
		return 0
	if not _backend.has_method(&"in_flight_count"):
		return 0
	return int(_backend.call(&"in_flight_count"))


func max_queries() -> int:
	if _backend == null or not is_instance_valid(_backend):
		return 0
	if not _backend.has_method(&"max_queries"):
		return 0
	return int(_backend.call(&"max_queries"))


func _bind_backend_signal() -> void:
	if _backend == null or not is_instance_valid(_backend):
		return
	if not _backend.has_signal(&"batch_ready"):
		return
	var callback := Callable(self, &"_on_backend_batch_ready")
	if not _backend.is_connected(&"batch_ready", callback):
		_backend.connect(&"batch_ready", callback)


func _unbind_backend_signal() -> void:
	if _backend == null or not is_instance_valid(_backend):
		return
	if not _backend.has_signal(&"batch_ready"):
		return
	var callback := Callable(self, &"_on_backend_batch_ready")
	if _backend.is_connected(&"batch_ready", callback):
		_backend.disconnect(&"batch_ready", callback)


func _on_backend_batch_ready(request_id: int, results: Array[Dictionary]) -> void:
	batch_ready.emit(request_id, results)


func _exit_tree() -> void:
	_unbind_backend_signal()
	_backend = null
