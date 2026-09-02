extends Node
## 0.1.0 planetary water coordinator.
##
## OceanSystem remains the production visible coat. WaterSystem owns the shared
## render resources, stable gameplay-query facade and the production Phase-3 sparse
## hydrology runtime. Dynamic hydrology rendering remains opt-in until reconstruction
## validation is complete; physical sparse hydrology can run independently.

signal query_backend_ready
signal dynamic_surface_ready
signal dynamic_surface_render_changed(enabled: bool)
signal sparse_runtime_state_changed(state: String)
signal sparse_runtime_ready
signal sparse_runtime_failed(error: Error, stage: String)

# Conservative production bootstrap defaults. The actual cell size is snapped to
# HydroMetricGrid's exact cube-sphere-compatible value for the current planet.
var sparse_runtime_enabled := true
var sparse_capacity := 1024
var sparse_tile_resolution := 16
var sparse_target_cell_size_m := 4.0
var sparse_macro_dt_s := 0.05
var sparse_max_time_debt_s := 0.50
var sparse_max_gpu_substeps := 16

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

# Production sparse runtime ownership.
var _sparse_state := "offline"
var _sparse_generation := 0
var _sparse_metric_contract: Dictionary = {}
var _sparse_scheduler: SparseHydroScheduler
var _sparse_atlas: SparseHydroAtlasGPU
var _sparse_identity: SparseHydroIdentityBridge
var _sparse_connectivity: SparseHydroConnectivityGPU
var _sparse_reachability: HydroReachabilityService
var _sparse_runtime: SparseHydrologyRuntime
var _structure_crest_provider := Callable()

# Stable source definitions survive asynchronous bootstrap and world rebuilds.
# String id -> Dictionary(direction, rate_m3_s, velocity_world, tile_level, enabled)
var _point_sources: Dictionary = {}


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

	if not Planet.world_ready.is_connected(_on_planet_world_ready):
		Planet.world_ready.connect(_on_planet_world_ready)
	if Planet.ready_state:
		call_deferred("_bootstrap_sparse_runtime")
	else:
		_set_sparse_state("waiting_for_world")


func _process(_dt: float) -> void:
	if _render_consumer_bound:
		_sync_dynamic_input_frame()


func query_service() -> HydrologyQueryService:
	return _query_service


func surface_resources() -> WaterSurfaceResources:
	return _surface_resources


func sparse_runtime() -> SparseHydrologyRuntime:
	return _sparse_runtime


func sparse_runtime_available() -> bool:
	return _sparse_state == "ready" and _sparse_runtime != null \
		and _sparse_runtime.initialized_ok()


func sparse_runtime_state() -> String:
	return _sparse_state


func sparse_metric_contract() -> Dictionary:
	return _sparse_metric_contract.duplicate(true)


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


## Structure service hook. Return a finite crest elevation in metres for levees,
## dams, walls, gates, foundations, etc., or null/NAN when no structure contributes.
func set_hydrology_structure_crest_provider(provider: Callable) -> void:
	_structure_crest_provider = provider
	if _sparse_reachability != null:
		_sparse_reachability.set_structure_crest_provider(provider)


## Stable production source API. Definitions are retained even before sparse
## bootstrap and replay automatically when a world/runtime becomes ready.
func upsert_point_water_source(source_id: String, direction: Vector3,
		rate_m3_s: float, injection_velocity_world: Vector3 = Vector3.ZERO,
		tile_level: int = -1, source_enabled: bool = true) -> Error:
	if source_id.is_empty() or direction.length_squared() < 0.5 \
			or not is_finite(rate_m3_s) or not _finite_vec3(injection_velocity_world):
		return ERR_INVALID_PARAMETER
	_point_sources[source_id] = {
		"direction": direction.normalized(),
		"rate_m3_s": rate_m3_s,
		"velocity_world": injection_velocity_world,
		"tile_level": tile_level,
		"enabled": source_enabled,
	}
	if sparse_runtime_available() and _sparse_runtime.sources_available():
		return _sparse_runtime.upsert_point_source(source_id, direction.normalized(),
			rate_m3_s, injection_velocity_world, tile_level, source_enabled)
	return OK


func remove_point_water_source(source_id: String) -> bool:
	var existed := _point_sources.erase(source_id)
	if sparse_runtime_available() and _sparse_runtime.sources_available():
		var runtime_removed := _sparse_runtime.remove_point_source(source_id)
		return existed or runtime_removed
	return existed


func set_point_water_source_enabled(source_id: String, enabled: bool) -> bool:
	var value: Variant = _point_sources.get(source_id, null)
	if not (value is Dictionary):
		return false
	var source: Dictionary = value
	source["enabled"] = enabled
	_point_sources[source_id] = source
	if sparse_runtime_available() and _sparse_runtime.sources_available():
		return _sparse_runtime.set_point_source_enabled(source_id, enabled)
	return true


func point_water_source_count() -> int:
	return _point_sources.size()


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
		"phase": 3,
		"query_backend_bound": _query_service != null and _query_service.backend() != null,
		"query_available": _query_service != null and _query_service.available(),
		"query_in_flight": 0 if _query_service == null else _query_service.in_flight_count(),
		"render_consumer_bound": _render_consumer_bound,
		"dynamic_surface_render_enabled": _dynamic_surface_render_enabled,
		"dynamic_surface_anchor": dynamic_surface_anchor_frame(),
		"dynamic_surface": {} if _surface_resources == null else _surface_resources.stats(),
		"sparse_runtime_state": _sparse_state,
		"sparse_metric_contract": sparse_metric_contract(),
		"sparse_runtime": {} if _sparse_runtime == null else _sparse_runtime.stats(),
		"sparse_atlas": {} if _sparse_atlas == null else _sparse_atlas.stats(),
		"sparse_reachability": {} if _sparse_reachability == null \
			else _sparse_reachability.stats(),
		"registered_point_sources": _point_sources.size(),
	}


func _on_planet_world_ready(_fields: PlanetFields) -> void:
	# A new world can have a different radius, terrain texture and generation seed.
	# Rebuild every sparse GPU resource; stable external source definitions survive.
	call_deferred("_bootstrap_sparse_runtime")


func _bootstrap_sparse_runtime() -> void:
	if not sparse_runtime_enabled:
		_release_sparse_runtime()
		_set_sparse_state("disabled")
		return
	if not Planet.ready_state or Planet.cfg == null:
		_set_sparse_state("waiting_for_world")
		return
	if Planet.global_height_texture == null or Planet.global_height_face_res <= 0:
		_set_sparse_state("waiting_for_terrain")
		return
	if RenderingServer.get_rendering_device() == null:
		_release_sparse_runtime()
		_set_sparse_state("unavailable_no_rendering_device")
		return
	if sparse_capacity <= 0 or sparse_tile_resolution <= 0 or sparse_target_cell_size_m <= 0.0:
		_fail_sparse_bootstrap(ERR_INVALID_PARAMETER, "configuration")
		return

	_release_sparse_runtime()
	_sparse_generation += 1
	var generation := _sparse_generation
	var radius := maxf(Planet.cfg.planet_radius, 1.0)
	_sparse_metric_contract = HydroMetricGrid.contract_for_target(
		radius, sparse_tile_resolution, sparse_target_cell_size_m)
	var exact_dx := float(_sparse_metric_contract.get("compatible_cell_size_m", 0.0))
	if exact_dx <= 0.0 or not is_finite(exact_dx):
		_fail_sparse_bootstrap(ERR_INVALID_DATA, "metric_contract")
		return

	_sparse_scheduler = SparseHydroScheduler.new(sparse_capacity)
	_sparse_scheduler.freeze_wet_tiles = false

	var atlas := SparseHydroAtlasGPU.new()
	atlas.name = "SparseHydroAtlasGPU"
	_sparse_atlas = atlas
	add_child(atlas)
	atlas.initialized.connect(func(): _on_sparse_atlas_initialized(generation, atlas))
	atlas.initialization_failed.connect(func(error: Error):
		_on_sparse_component_failed(generation, atlas, error, "atlas"))
	_set_sparse_state("initializing_atlas")
	var err := atlas.initialize(sparse_capacity, sparse_tile_resolution, exact_dx)
	if err != OK:
		_fail_sparse_bootstrap(err, "atlas_submit")


func _on_sparse_atlas_initialized(generation: int, atlas: SparseHydroAtlasGPU) -> void:
	if not _sparse_generation_matches(generation) or atlas != _sparse_atlas:
		return
	var identity := SparseHydroIdentityBridge.new()
	identity.name = "SparseHydroIdentityBridge"
	_sparse_identity = identity
	add_child(identity)
	var bind_error := identity.bind(_sparse_scheduler, atlas)
	if bind_error != OK:
		_fail_sparse_bootstrap(bind_error, "identity_bind")
		return

	var connectivity := SparseHydroConnectivityGPU.new()
	connectivity.name = "SparseHydroConnectivityGPU"
	_sparse_connectivity = connectivity
	add_child(connectivity)
	connectivity.initialized.connect(
		func(): _on_sparse_connectivity_initialized(generation, connectivity))
	connectivity.initialization_failed.connect(func(error: Error):
		_on_sparse_component_failed(generation, connectivity, error, "connectivity"))
	_set_sparse_state("initializing_connectivity")
	var err := connectivity.initialize(sparse_capacity)
	if err != OK:
		_fail_sparse_bootstrap(err, "connectivity_submit")


func _on_sparse_connectivity_initialized(generation: int,
		connectivity: SparseHydroConnectivityGPU) -> void:
	if not _sparse_generation_matches(generation) or connectivity != _sparse_connectivity:
		return
	var sync_error := connectivity.sync_pool(_sparse_scheduler.pool)
	if sync_error != OK:
		_fail_sparse_bootstrap(sync_error, "connectivity_bootstrap_sync")
		return

	_sparse_reachability = HydroReachabilityService.new()
	var reachability_error := _sparse_reachability.initialize(_sparse_atlas)
	if reachability_error != OK:
		_fail_sparse_bootstrap(reachability_error, "reachability")
		return
	if _structure_crest_provider.is_valid():
		_sparse_reachability.set_structure_crest_provider(_structure_crest_provider)

	var runtime := SparseHydrologyRuntime.new()
	runtime.name = "SparseHydrologyRuntime"
	runtime.process_priority = 12
	runtime.auto_run = true
	runtime.macro_dt_s = maxf(sparse_macro_dt_s, 1.0e-5)
	runtime.max_time_debt_s = maxf(sparse_max_time_debt_s, runtime.macro_dt_s)
	runtime.max_gpu_substeps = clampi(sparse_max_gpu_substeps, 1,
		SparseHydroStepGPU.MAX_GPU_SUBSTEPS)
	_sparse_runtime = runtime
	add_child(runtime)
	runtime.initialized.connect(func(): _on_sparse_runtime_initialized(generation, runtime))
	runtime.initialization_failed.connect(func(error: Error, component: String):
		_on_sparse_runtime_init_failed(generation, runtime, error, component))
	runtime.runtime_failed.connect(func(error: Error, stage: String):
		_on_sparse_runtime_failed(generation, runtime, error, stage))
	_set_sparse_state("initializing_runtime")
	var runtime_error := runtime.initialize(_sparse_scheduler, _sparse_atlas,
		_sparse_connectivity, _sparse_identity,
		Callable(_sparse_reachability, &"can_enter"))
	if runtime_error != OK:
		_fail_sparse_bootstrap(runtime_error, "runtime_submit")


func _on_sparse_runtime_initialized(generation: int,
		runtime: SparseHydrologyRuntime) -> void:
	if not _sparse_generation_matches(generation) or runtime != _sparse_runtime:
		return
	_set_sparse_state("ready")
	_replay_point_sources()
	sparse_runtime_ready.emit()


func _replay_point_sources() -> void:
	if not sparse_runtime_available() or not _sparse_runtime.sources_available():
		return
	for id_variant: Variant in _point_sources.keys():
		var source_id := String(id_variant)
		var value: Variant = _point_sources[source_id]
		if not (value is Dictionary):
			continue
		var source: Dictionary = value
		var err := _sparse_runtime.upsert_point_source(source_id,
			source.get("direction", Vector3.RIGHT),
			float(source.get("rate_m3_s", 0.0)),
			source.get("velocity_world", Vector3.ZERO),
			int(source.get("tile_level", -1)),
			bool(source.get("enabled", true)))
		if err != OK:
			push_warning("WaterSystem: failed to replay source '%s' (%d)." % [
				source_id, int(err)])


func _on_sparse_component_failed(generation: int, component_node: Node,
		error: Error, stage: String) -> void:
	if not _sparse_generation_matches(generation):
		return
	if component_node != _sparse_atlas and component_node != _sparse_connectivity:
		return
	_fail_sparse_bootstrap(error, stage)


func _on_sparse_runtime_init_failed(generation: int, runtime: SparseHydrologyRuntime,
		error: Error, component: String) -> void:
	if _sparse_generation_matches(generation) and runtime == _sparse_runtime:
		_fail_sparse_bootstrap(error, "runtime_init_" + component)


func _on_sparse_runtime_failed(generation: int, runtime: SparseHydrologyRuntime,
		error: Error, stage: String) -> void:
	if not _sparse_generation_matches(generation) or runtime != _sparse_runtime:
		return
	_set_sparse_state("failed")
	sparse_runtime_failed.emit(error, "runtime_" + stage)


func _fail_sparse_bootstrap(error: Error, stage: String) -> void:
	_set_sparse_state("failed")
	push_error("WaterSystem: sparse hydrology bootstrap failed at %s (%d)." % [
		stage, int(error)])
	sparse_runtime_failed.emit(error, stage)


func _sparse_generation_matches(generation: int) -> bool:
	return generation == _sparse_generation


func _set_sparse_state(state: String) -> void:
	if _sparse_state == state:
		return
	_sparse_state = state
	sparse_runtime_state_changed.emit(state)


func _release_sparse_runtime() -> void:
	# Invalidate every lambda/callback from the old generation before touching RIDs.
	_sparse_generation += 1
	if _sparse_runtime != null and is_instance_valid(_sparse_runtime):
		_sparse_runtime.release()
		_sparse_runtime.queue_free()
	if _sparse_identity != null and is_instance_valid(_sparse_identity):
		_sparse_identity.unbind()
		_sparse_identity.queue_free()
	if _sparse_connectivity != null and is_instance_valid(_sparse_connectivity):
		_sparse_connectivity.release()
		_sparse_connectivity.queue_free()
	if _sparse_atlas != null and is_instance_valid(_sparse_atlas):
		_sparse_atlas.release()
		_sparse_atlas.queue_free()
	_sparse_runtime = null
	_sparse_reachability = null
	_sparse_identity = null
	_sparse_connectivity = null
	_sparse_atlas = null
	_sparse_scheduler = null
	_sparse_metric_contract.clear()


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


func _finite_vec3(v: Vector3) -> bool:
	return is_finite(v.x) and is_finite(v.y) and is_finite(v.z)


func _exit_tree() -> void:
	if Planet.world_ready.is_connected(_on_planet_world_ready):
		Planet.world_ready.disconnect(_on_planet_world_ready)
	_release_sparse_runtime()
