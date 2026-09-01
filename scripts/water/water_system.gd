extends Node
## 0.1.0 planetary water coordinator.
##
## Phase 1 establishes ownership boundaries without changing the working 0.0.5
## ocean. OceanSystem still owns the legacy renderer/query backend; WaterSystem
## owns the new shared GPU surface resources and exposes a stable query facade.
## The next migration step will make OceanSystem a consumer of these services.

signal query_backend_ready
signal dynamic_surface_ready

var _query_service: HydrologyQueryService
var _surface_resources: WaterSurfaceResources
var _legacy_backend_bind_attempts := 0


func _ready() -> void:
	process_priority = 9

	_query_service = HydrologyQueryService.new()
	_query_service.name = "HydrologyQueryService"
	add_child(_query_service)

	_surface_resources = WaterSurfaceResources.new()
	_surface_resources.name = "WaterSurfaceResources"
	add_child(_surface_resources)
	if _surface_resources.available():
		dynamic_surface_ready.emit()
	else:
		_surface_resources.resources_ready.connect(_on_surface_resources_ready)

	# WaterSystem is intentionally autoloaded immediately after OceanSystem during
	# the compatibility stage. Bind now if OceanSystem is ready, and retry deferred
	# for editor/startup order variations.
	_bind_legacy_query_backend()
	if _query_service.backend() == null:
		call_deferred("_bind_legacy_query_backend")


func query_service() -> HydrologyQueryService:
	return _query_service


func surface_resources() -> WaterSurfaceResources:
	return _surface_resources


func dynamic_surface_texture() -> Texture2D:
	return null if _surface_resources == null else _surface_resources.field_texture()


func dynamic_surface_available() -> bool:
	return _surface_resources != null and _surface_resources.available()


func debug_write_surface_gaussian(amplitude_m: float = 2.0, radius_m: float = 260.0,
		velocity_plane: Vector2 = Vector2.ZERO, activity: float = 1.0) -> Error:
	if _surface_resources == null:
		return ERR_UNAVAILABLE
	return _surface_resources.debug_write_gaussian(amplitude_m, radius_m,
		velocity_plane, activity)


func debug_clear_surface() -> Error:
	if _surface_resources == null:
		return ERR_UNAVAILABLE
	return _surface_resources.clear()


func gpu_stats() -> Dictionary:
	return {
		"phase": 1,
		"query_backend_bound": _query_service != null and _query_service.backend() != null,
		"query_available": _query_service != null and _query_service.available(),
		"query_in_flight": 0 if _query_service == null else _query_service.in_flight_count(),
		"dynamic_surface": {} if _surface_resources == null else _surface_resources.stats(),
	}


func _bind_legacy_query_backend() -> void:
	if _query_service == null or _query_service.backend() != null:
		return
	_legacy_backend_bind_attempts += 1

	var ocean := get_node_or_null("/root/OceanSystem")
	if ocean == null:
		_retry_legacy_bind_if_needed()
		return

	# GDScript script variables are Object properties. This is a temporary bridge;
	# once OceanSystem is migrated it will register its query backend explicitly.
	var backend_value: Variant = ocean.get("_physics")
	if backend_value is Node:
		var backend := backend_value as Node
		if backend.has_method(&"request_batch"):
			_query_service.bind_backend(backend)
			query_backend_ready.emit()
			return

	_retry_legacy_bind_if_needed()


func _retry_legacy_bind_if_needed() -> void:
	# Avoid an unbounded deferred loop if OceanSystem failed to initialize. A few
	# frames are sufficient for ordinary autoload/editor startup ordering.
	if _legacy_backend_bind_attempts < 8:
		get_tree().process_frame.connect(_bind_legacy_query_backend, CONNECT_ONE_SHOT)
	elif _legacy_backend_bind_attempts == 8:
		push_warning("WaterSystem: legacy OceanGPUPhysics backend was not available; "
			+ "HydrologyQueryService will remain offline until explicitly rebound.")


func _on_surface_resources_ready() -> void:
	dynamic_surface_ready.emit()
