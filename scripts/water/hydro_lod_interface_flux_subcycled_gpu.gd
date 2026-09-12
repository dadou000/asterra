class_name HydroLODInterfaceFluxSubcycledGPU
extends HydroLODInterfaceFluxGPU
## Temporal-subcycling extension of the 2:1 interface operator.
##
## Fine updates write equal/opposite coarse increments into a GPU flux register.
## Registers are indexed by authoritative atlas cell rather than interface lane so
## two mixed edges meeting at one coarse corner share the same water reservation.

var _flux_register := RID()
var _flux_register_bytes := 0


func initialize(p_atlas: SparseHydroAtlasGPU, control_rid: RID,
		p_gravity: float = 9.81, p_dry_eps: float = 1.0e-5) -> Error:
	if _initialized or _init_pending:
		return ERR_BUSY
	if p_atlas == null or not p_atlas.initialized_ok() \
			or not p_atlas.hydrolod_enabled() or not control_rid.is_valid():
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_interface_flux_subcycled.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE
	atlas = p_atlas
	_control = control_rid
	gravity = maxf(p_gravity, 1.0e-4)
	dry_eps = maxf(p_dry_eps, 1.0e-8)
	_max_descriptors = maxi(atlas.capacity * 4, 1)
	_flux_register_bytes = atlas.total_cell_count() * 16
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


## Render-thread only. Start every solver advance from a clean register. Normal
## completion also drains all registers because the final fine tick forces a sync;
## this clear is a conservative recovery boundary for aborted/failed advances.
func clear_flux_register(rd: RenderingDevice) -> Error:
	if not _initialized or not _flux_register.is_valid():
		return ERR_UNAVAILABLE
	return rd.buffer_clear(_flux_register, 0, _flux_register_bytes)


func stats() -> Dictionary:
	var out := super.stats()
	out["temporal_flux_registers"] = true
	out["register_scope"] = "atlas_cell"
	out["shared_corner_reservations"] = true
	out["flux_register_bytes"] = _flux_register_bytes
	out["fine_flux_accumulates_until_coarse_sync"] = true
	out["gpu_bytes"] = int(out.get("gpu_bytes", 0)) + _flux_register_bytes
	return out


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var pipeline := rd.compute_pipeline_create(shader)
	var interface_bytes := PackedByteArray()
	interface_bytes.resize(_max_descriptors * DESCRIPTOR_BYTES)
	var param_bytes := PackedByteArray()
	param_bytes.resize(PARAM_BYTES)
	var register_bytes := PackedByteArray()
	register_bytes.resize(_flux_register_bytes)
	var interfaces := rd.storage_buffer_create(interface_bytes.size(), interface_bytes)
	var params := rd.storage_buffer_create(PARAM_BYTES, param_bytes)
	var flux_register := rd.storage_buffer_create(register_bytes.size(), register_bytes)
	if not pipeline.is_valid() or not interfaces.is_valid() or not params.is_valid() \
			or not flux_register.is_valid():
		_free_many(rd, [flux_register, params, interfaces, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, atlas.state_a_rid()),
		_storage_uniform(1, atlas.state_b_rid()),
		_storage_uniform(2, atlas.occupancy_rid()),
		_storage_uniform(3, interfaces),
		_storage_uniform(4, params),
		_storage_uniform(5, _control),
		_storage_uniform(6, atlas.tile_metadata_rid()),
		_storage_uniform(7, flux_register),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [flux_register, params, interfaces, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_subcycled_init", OK, {
		"shader": shader,
		"pipeline": pipeline,
		"interfaces": interfaces,
		"params": params,
		"flux_register": flux_register,
		"set": set_rid,
	})


func _finish_subcycled_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_interfaces = bundle["interfaces"]
	_params = bundle["params"]
	_flux_register = bundle["flux_register"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids := [_uniform_set, _flux_register, _params, _interfaces, _pipeline, _shader]
	_initialized = false
	_init_pending = false
	_interface_count = 0
	_synced_pool_revision = -1
	_edge_claims.clear()
	_uniform_set = RID(); _flux_register = RID(); _params = RID(); _interfaces = RID()
	_pipeline = RID(); _shader = RID(); _control = RID()
	_flux_register_bytes = 0
	atlas = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()
