extends Node
## 0.1.0 planetary water coordinator.
##
## OceanSystem remains the production visible coat while WaterSystem owns the new
## shared GPU surface resources and the stable gameplay-query facade. Dynamic
## hydrology contribution defaults OFF until tests explicitly enable it.

signal query_backend_ready
signal dynamic_surface_ready
signal dynamic_surface_render_changed(enabled: bool)

var _query_service: HydrologyQueryService
var _surface_resources: WaterSurfaceResources
var _legacy_backend_bind_attempts := 0
var _render_consumer_bound := false
var _dynamic_surface_render_enabled := false


func _ready() -> void:
	process_priority = 9

	_query_service = HydrologyQueryService.new()
	_query_service.name = "HydrologyQueryService"
	add_child(_query_service)

	_surface_resources = WaterSurfaceResources.new()
	_surface_resources.name = "WaterSurfaceResources"
	add_child(_surface_resources)
	if _surface_resources.available():
		_bind_dynamic_surface_material()
		dynamic_surface_ready.emit()
	else:
		_surface_resources.resources_ready.connect(_on_surface_resources_ready)

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


func dynamic_surface_render_enabled() -> bool:
	return _dynamic_surface_render_enabled


## Production default is false. Tests/debug tools may enable the reconstructed
## hydrology coat without changing the underlying solver or shared texture.
func set_dynamic_surface_render_enabled(enabled: bool) -> void:
	_dynamic_surface_render_enabled = enabled
	var material := _ocean_material()
	if material != null:
		material.set_shader_parameter("u_dynamic_surface_enabled", 1.0 if enabled else 0.0)
	dynamic_surface_render_changed.emit(enabled)


## Sets the metric tangent-plane centre used by both the compute reconstruction
## and the ocean material sampling contract.
func set_dynamic_surface_center_plane(center_plane: Vector2) -> void:
	if _surface_resources != null:
		_surface_resources.set_field_center_plane(center_plane)
	var material := _ocean_material()
	if material != null:
		material.set_shader_parameter("u_dynamic_surface_center_plane", center_plane)


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
		"phase": 2,
		"query_backend_bound": _query_service != null and _query_service.backend() != null,
		"query_available": _query_service != null and _query_service.available(),
		"query_in_flight": 0 if _query_service == null else _query_service.in_flight_count(),
		"render_consumer_bound": _render_consumer_bound,
		"dynamic_surface_render_enabled": _dynamic_surface_render_enabled,
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

	var backend_value: Variant = ocean.get("_physics")
	if backend_value is Node:
		var backend := backend_value as Node
		if backend.has_method(&"request_batch"):
			_query_service.bind_backend(backend)
			query_backend_ready.emit()
			return

	_retry_legacy_bind_if_needed()


func _retry_legacy_bind_if_needed() -> void:
	if _legacy_backend_bind_attempts < 8:
		get_tree().process_frame.connect(_bind_legacy_query_backend, CONNECT_ONE_SHOT)
	elif _legacy_backend_bind_attempts == 8:
		push_warning("WaterSystem: legacy OceanGPUPhysics backend was not available; "
			+ "HydrologyQueryService will remain offline until explicitly rebound.")


func _ocean_material() -> ShaderMaterial:
	var ocean := get_node_or_null("/root/OceanSystem")
	if ocean == null or not ocean.has_method(&"material"):
		return null
	var material_value: Variant = ocean.call(&"material")
	return material_value as ShaderMaterial if material_value is ShaderMaterial else null


func _bind_dynamic_surface_material() -> void:
	_render_consumer_bound = false
	if _surface_resources == null or not _surface_resources.available():
		return
	var water_material := _ocean_material()
	if water_material == null:
		return
	var texture := _surface_resources.field_texture()
	if texture == null:
		return

	water_material.set_shader_parameter("u_dynamic_surface_field", texture)
	water_material.set_shader_parameter("u_dynamic_surface_center_plane",
		_surface_resources.field_center_plane())
	water_material.set_shader_parameter("u_dynamic_surface_half_extent_m",
		_surface_resources.field_half_extent_m())
	water_material.set_shader_parameter("u_dynamic_surface_enabled",
		1.0 if _dynamic_surface_render_enabled else 0.0)
	_render_consumer_bound = true


func _on_surface_resources_ready() -> void:
	_bind_dynamic_surface_material()
	dynamic_surface_ready.emit()
