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

# Persistent planet-space frame owned by the hydrology cache, deliberately
# independent of OceanSystem's disposable toroidal render anchor.
var _surface_anchor_dir := Vector3(1.0, 0.0, 0.0)
var _surface_anchor_right := Vector3(0.0, 0.0, -1.0)
var _surface_anchor_up := Vector3(0.0, 1.0, 0.0)


func _ready() -> void:
	# OceanSystem runs at priority 10. Run immediately after it so its current
	# clipmap anchor has already been written to the material before we mirror that
	# temporary input frame into the dynamic-water sampling contract.
	process_priority = 11

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


func _process(_dt: float) -> void:
	if _render_consumer_bound:
		_sync_dynamic_input_frame()


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
		_sync_dynamic_input_frame()
		material.set_shader_parameter("u_dynamic_surface_enabled", 1.0 if enabled else 0.0)
	dynamic_surface_render_changed.emit(enabled)


## Define the persistent world anchor for the local hydrology cache. Reanchoring
## the toroidal visible ocean does not alter this frame.
func set_dynamic_surface_anchor_direction(direction: Vector3) -> void:
	if direction.length_squared() <= 1.0e-10:
		return
	_surface_anchor_dir = direction.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_surface_anchor_dir)
	_surface_anchor_right = tangent[0]
	_surface_anchor_up = tangent[1]
	_sync_dynamic_anchor_uniforms()


func dynamic_surface_anchor_frame() -> Dictionary:
	return {
		"dir": _surface_anchor_dir,
		"right": _surface_anchor_right,
		"up": _surface_anchor_up,
		"center_plane": Vector2.ZERO if _surface_resources == null \
			else _surface_resources.field_center_plane(),
	}


## Sets the metric centre inside the persistent hydrology tangent frame. Most
## local domains use Vector2.ZERO and move/rebuild the cache by changing its
## planet-space anchor rather than inheriting a render-clipmap recenter.
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
		"dynamic_surface_anchor": dynamic_surface_anchor_frame(),
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


func _sync_dynamic_anchor_uniforms() -> void:
	var material := _ocean_material()
	if material == null:
		return
	material.set_shader_parameter("u_dynamic_surface_anchor_dir", _surface_anchor_dir)
	material.set_shader_parameter("u_dynamic_surface_anchor_right", _surface_anchor_right)
	material.set_shader_parameter("u_dynamic_surface_anchor_up", _surface_anchor_up)


## Mirror the *current* disposable render-anchor frame only as the coordinate frame
## of the vertex call site. The dynamic field itself remains in _surface_anchor_*.
func _sync_dynamic_input_frame() -> void:
	var material := _ocean_material()
	if material == null:
		return
	var anchor_dir: Variant = material.get_shader_parameter("u_anchor_dir")
	var anchor_right: Variant = material.get_shader_parameter("u_anchor_right")
	var anchor_up: Variant = material.get_shader_parameter("u_anchor_up")
	if anchor_dir is Vector3 and anchor_right is Vector3 and anchor_up is Vector3:
		material.set_shader_parameter("u_dynamic_surface_input_anchor_dir", anchor_dir)
		material.set_shader_parameter("u_dynamic_surface_input_anchor_right", anchor_right)
		material.set_shader_parameter("u_dynamic_surface_input_anchor_up", anchor_up)
	var radius := 1.0
	if Planet.ready_state and Planet.cfg != null:
		radius = maxf(Planet.cfg.planet_radius, 1.0)
	material.set_shader_parameter("u_dynamic_surface_planet_radius", radius)


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
	_sync_dynamic_anchor_uniforms()
	_sync_dynamic_input_frame()
	water_material.set_shader_parameter("u_dynamic_surface_enabled",
		1.0 if _dynamic_surface_render_enabled else 0.0)
	_render_consumer_bound = true


func _on_surface_resources_ready() -> void:
	_bind_dynamic_surface_material()
	dynamic_surface_ready.emit()
