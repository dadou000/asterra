extends Node
## Production view/cache bridge from sparse physical hydrology to the shared local
## RGBA32F dynamic-water texture.
##
## This node is deliberately separate from SparseHydrologyRuntime. Camera/ocean
## movement can recenter this cache, but can never allocate/release hydrology tiles,
## change CFL scheduling or otherwise affect authoritative physical state.

signal cache_ready
signal cache_failed(error: Error)
signal cache_updated(request_id: int)

var activity_gain := 0.30
var reanchor_fraction := 0.35

var _runtime: SparseHydrologyRuntime
var _reconstruction: SparseHydroSurfaceReconstructionGPU
var _ready_cache := false


func _ready() -> void:
	process_priority = 13
	if not WaterSystem.sparse_runtime_ready.is_connected(_on_sparse_runtime_ready):
		WaterSystem.sparse_runtime_ready.connect(_on_sparse_runtime_ready)
	if not WaterSystem.sparse_runtime_state_changed.is_connected(_on_sparse_state_changed):
		WaterSystem.sparse_runtime_state_changed.connect(_on_sparse_state_changed)
	if not WaterSystem.dynamic_surface_ready.is_connected(_on_dynamic_surface_ready):
		WaterSystem.dynamic_surface_ready.connect(_on_dynamic_surface_ready)
	call_deferred("_try_bind")


func available() -> bool:
	return _ready_cache and _reconstruction != null \
		and _reconstruction.initialized_ok()


func stats() -> Dictionary:
	return {
		"available": available(),
		"activity_gain": activity_gain,
		"reanchor_fraction": reanchor_fraction,
		"runtime_bound": _runtime != null,
		"reconstruction": {} if _reconstruction == null \
			else _reconstruction.stats(),
	}


func request_update() -> int:
	if not available() or _reconstruction.pending():
		return -1
	var frame := WaterSystem.dynamic_surface_anchor_frame()
	var anchor_dir: Variant = frame.get("dir", Vector3.RIGHT)
	var anchor_right: Variant = frame.get("right", Vector3(0.0, 0.0, -1.0))
	var anchor_up: Variant = frame.get("up", Vector3.UP)
	var center: Variant = frame.get("center_plane", Vector2.ZERO)
	if not (anchor_dir is Vector3 and anchor_right is Vector3 \
			and anchor_up is Vector3 and center is Vector2):
		return -1
	return _reconstruction.reconstruct(anchor_dir, anchor_right, anchor_up,
		center, activity_gain)


func _on_sparse_runtime_ready() -> void:
	_try_bind()


func _on_dynamic_surface_ready() -> void:
	_try_bind()


func _on_sparse_state_changed(state: String) -> void:
	if state != "ready":
		_teardown()
	else:
		call_deferred("_try_bind")


func _try_bind() -> void:
	if WaterSystem.sparse_runtime_state() != "ready" \
			or not WaterSystem.dynamic_surface_available():
		return
	var runtime := WaterSystem.sparse_runtime()
	if runtime == null or not runtime.initialized_ok() or runtime.atlas == null \
			or not runtime.atlas.initialized_ok():
		return
	if _runtime == runtime and _reconstruction != null:
		return

	_teardown()
	_runtime = runtime
	if not _runtime.cycle_completed.is_connected(_on_runtime_cycle_completed):
		_runtime.cycle_completed.connect(_on_runtime_cycle_completed)

	var metric := WaterSystem.sparse_metric_contract()
	var hydro_level := int(metric.get("level", -1))
	if hydro_level < 0 or not Planet.ready_state or Planet.cfg == null:
		_fail(ERR_UNCONFIGURED)
		return
	var resources := WaterSystem.surface_resources()
	if resources == null or not resources.available():
		_fail(ERR_UNAVAILABLE)
		return

	_reconstruction = SparseHydroSurfaceReconstructionGPU.new()
	_reconstruction.name = "SparseHydroSurfaceReconstructionGPU"
	add_child(_reconstruction)
	_reconstruction.initialized.connect(_on_reconstruction_initialized)
	_reconstruction.initialization_failed.connect(_on_reconstruction_init_failed)
	_reconstruction.reconstruction_recorded.connect(_on_reconstruction_recorded)
	_reconstruction.reconstruction_failed.connect(_on_reconstruction_failed)
	var err := _reconstruction.initialize(runtime.atlas, resources.field_rid(),
		resources.field_resolution(), resources.field_half_extent_m(),
		hydro_level, maxf(Planet.cfg.planet_radius, 1.0), runtime.solver.dry_eps)
	if err != OK:
		_fail(err)


func _on_reconstruction_initialized() -> void:
	_ready_cache = true
	_align_cache_to_ocean(true)
	request_update()
	cache_ready.emit()


func _on_runtime_cycle_completed(_cycle_id: int, _report: Dictionary) -> void:
	if not available():
		return
	_align_cache_to_ocean(false)
	request_update()


func _align_cache_to_ocean(force: bool) -> void:
	var ocean := get_node_or_null("/root/OceanSystem")
	if ocean == null or not ocean.has_method(&"material"):
		return
	var material_value: Variant = ocean.call(&"material")
	if not (material_value is ShaderMaterial):
		return
	var center_value: Variant = (material_value as ShaderMaterial).get_shader_parameter(
		"u_center_dir")
	if not (center_value is Vector3):
		return
	var center_dir := (center_value as Vector3).normalized()
	if center_dir.length_squared() < 0.5:
		return

	var current := WaterSystem.dynamic_surface_anchor_frame()
	var current_value: Variant = current.get("dir", Vector3.RIGHT)
	var current_dir := Vector3.RIGHT
	if current_value is Vector3:
		current_dir = current_value as Vector3
	var radius := 1.0
	if Planet.ready_state and Planet.cfg != null:
		radius = maxf(Planet.cfg.planet_radius, 1.0)
	var arc_m := acos(clampf(current_dir.normalized().dot(center_dir), -1.0, 1.0)) * radius
	var threshold := WaterSystem.surface_resources().field_half_extent_m() \
		* clampf(reanchor_fraction, 0.05, 0.90)
	if force or arc_m >= threshold:
		WaterSystem.set_dynamic_surface_anchor_direction(center_dir)
		WaterSystem.set_dynamic_surface_center_plane(Vector2.ZERO)


func _on_reconstruction_recorded(request_id: int) -> void:
	cache_updated.emit(request_id)


func _on_reconstruction_init_failed(error: Error) -> void:
	_fail(error)


func _on_reconstruction_failed(_request_id: int, error: Error) -> void:
	cache_failed.emit(error)


func _fail(error: Error) -> void:
	cache_failed.emit(error)
	push_warning("SparseHydroSurfaceCacheBridge: cache initialization failed (%d)." % int(error))
	_teardown()


func _teardown() -> void:
	_ready_cache = false
	if _runtime != null and is_instance_valid(_runtime) \
			and _runtime.cycle_completed.is_connected(_on_runtime_cycle_completed):
		_runtime.cycle_completed.disconnect(_on_runtime_cycle_completed)
	_runtime = null
	if _reconstruction != null and is_instance_valid(_reconstruction):
		_reconstruction.release()
		_reconstruction.queue_free()
	_reconstruction = null


func _exit_tree() -> void:
	if WaterSystem.sparse_runtime_ready.is_connected(_on_sparse_runtime_ready):
		WaterSystem.sparse_runtime_ready.disconnect(_on_sparse_runtime_ready)
	if WaterSystem.sparse_runtime_state_changed.is_connected(_on_sparse_state_changed):
		WaterSystem.sparse_runtime_state_changed.disconnect(_on_sparse_state_changed)
	if WaterSystem.dynamic_surface_ready.is_connected(_on_dynamic_surface_ready):
		WaterSystem.dynamic_surface_ready.disconnect(_on_dynamic_surface_ready)
	_teardown()
