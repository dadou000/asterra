class_name HydroLODInterfaceFluxGPU
extends Node
## Conservative 2:1 physical-HydroLOD interface registry + GPU dispatcher.
##
## Same-level neighbors remain owned by SparseHydroConnectivityGPU. This component
## discovers only live coarse<->fine boundaries, uploads a compact descriptor table,
## and records the correction pass between the ordinary A->B SWE update and B->A
## canonicalization. Missing halves of a sparse fine boundary remain reflective.
##
## A descriptor stores:
##   coarse slot, destination-low fine slot, destination-high fine slot,
##   coarse edge, fine edge, seam reversal.
##
## The CPU registry also claims every represented mixed-resolution source edge so
## the frontier allocator does not try to allocate an overlapping same-level tile.

signal initialized
signal initialization_failed(error: Error)
signal topology_synced(interface_count: int, pool_revision: int)
signal released

const LOCAL_X := 64
const WORDS_PER_DESCRIPTOR := 8
const DESCRIPTOR_BYTES := WORDS_PER_DESCRIPTOR * 4
const PARAM_BYTES := 32
const INVALID_SLOT := -1

var maximum_physical_lod := 4
var gravity := 9.81
var dry_eps := 1.0e-5

var atlas: SparseHydroAtlasGPU

var _shader := RID()
var _pipeline := RID()
var _interfaces := RID()
var _params := RID()
var _uniform_set := RID()
var _control := RID()
var _max_descriptors := 0
var _interface_count := 0
var _synced_pool_revision := -1
var _edge_claims: Dictionary = {}
var _initialized := false
var _init_pending := false


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
		"res://shaders/water/hydro_lod_interface_flux.glsl")
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
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func interface_count() -> int:
	return _interface_count


func synced_pool_revision() -> int:
	return _synced_pool_revision


func needs_sync(pool: HydroTilePool) -> bool:
	return pool != null and pool.topology_revision != _synced_pool_revision


func owns_edge(source: HydroTileKey, direction: int,
		_topology_link: Dictionary = {}) -> bool:
	if source == null or direction < 0 or direction > 3:
		return false
	return _edge_claims.has(_edge_key(source.packed(), direction))


## Rebuild the complete mixed-level descriptor table from authoritative CPU
## ownership. Validation is synchronous; the tiny GPU upload is queued afterward.
## Runtime/transition code may therefore fail closed before scheduling another SWE
## step if a >2:1 boundary or incomplete transition state is detected.
func sync_pool(pool: HydroTilePool) -> Error:
	if not _initialized or atlas == null or pool == null \
			or pool.capacity != atlas.capacity:
		return ERR_INVALID_PARAMETER
	var built := _build_descriptors(pool)
	var error := int(built.get("error", FAILED))
	if error != OK:
		return error
	var words := built.get("words", PackedInt32Array()) as PackedInt32Array
	var count := int(built.get("count", 0))
	if count < 0 or count > _max_descriptors \
			or words.size() != count * WORDS_PER_DESCRIPTOR:
		return ERR_OUT_OF_MEMORY

	_interface_count = count
	_edge_claims = (built.get("edge_claims", {}) as Dictionary).duplicate(true)
	_synced_pool_revision = pool.topology_revision
	var params_bytes := _make_params(count)
	RenderingServer.call_on_render_thread(
		Callable(self, &"_sync_render_thread").bind(
			words.to_byte_array(), params_bytes))
	topology_synced.emit(count, _synced_pool_revision)
	return OK


## Render-thread only. Called by SparseHydroStepGPU after the ordinary A->B update
## and before canonicalization. Descriptors are deliberately serialized with
## barriers so a tile corner shared by two LOD interfaces cannot race in-place B.
func record_corrections(rd: RenderingDevice, compute: int) -> void:
	if not _initialized or _interface_count <= 0 \
			or not _pipeline.is_valid() or not _uniform_set.is_valid():
		return
	var groups := int(ceil(float(atlas.tile_resolution) / float(LOCAL_X)))
	for interface_index in _interface_count:
		rd.compute_list_bind_compute_pipeline(compute, _pipeline)
		rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
		var push := PackedInt32Array([interface_index, 0, 0, 0]).to_byte_array()
		rd.compute_list_set_push_constant(compute, push, push.size())
		rd.compute_list_dispatch(compute, groups, 1, 1)
		rd.compute_list_add_barrier(compute)


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"interface_count": _interface_count,
		"maximum_descriptors": _max_descriptors,
		"synced_pool_revision": _synced_pool_revision,
		"claimed_source_edges": _edge_claims.size(),
		"two_to_one_only": true,
		"partial_sparse_interfaces": true,
		"same_step_reflux_correction": true,
		"gpu_bytes": _max_descriptors * DESCRIPTOR_BYTES + PARAM_BYTES,
	}


func _build_descriptors(pool: HydroTilePool) -> Dictionary:
	var words := PackedInt32Array()
	var claims: Dictionary = {}
	var seen: Dictionary = {}
	var records := pool.active_records()

	# A descriptor snapshot must represent a committed solver-visible topology.
	for record in records:
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			return {"error": ERR_BUSY}

	for record in records:
		var coarse := record.get("key") as HydroTileKey
		if coarse == null or not HydroLODHierarchy.valid_physical_key(
				atlas.base_tile_level, maximum_physical_lod, coarse):
			continue
		for direction in 4:
			var link := HydroTileTopology.neighbor(coarse, direction)
			if link.is_empty():
				continue
			var same_region := link.get("key") as HydroTileKey
			if same_region == null:
				continue

			# Exact same-level ownership is handled by the ordinary connectivity graph.
			if _published_slot(pool, same_region) >= 0:
				continue

			# If an ancestor covers the destination, this tile is the fine side. The
			# corresponding descriptor is emitted when that coarser owner is scanned.
			var covering := HydroLODHierarchy.covering_record(pool, same_region)
			if not covering.is_empty():
				var covering_key := covering.get("key") as HydroTileKey
				if covering_key != null and covering_key.level < coarse.level:
					if coarse.level - covering_key.level > 1:
						return {"error": ERR_INVALID_DATA,
							"reason": "hydrolod_balance_violation"}
					continue

			var destination_direction := int(link.get("destination_direction", -1))
			if destination_direction < 0 or destination_direction > 3:
				return {"error": ERR_INVALID_DATA, "reason": "invalid_lod_link"}

			# Any touching descendant deeper than the immediate children is a >2:1
			# boundary and must never enter the solver.
			for descendant in HydroLODHierarchy.descendant_records(pool, same_region):
				var descendant_key := descendant.get("key") as HydroTileKey
				if descendant_key == null \
						or not _touches_region_edge(descendant_key,
							same_region, destination_direction):
					continue
				if descendant_key.level > coarse.level + 1:
					return {"error": ERR_INVALID_DATA,
						"reason": "hydrolod_balance_violation"}

			var edge_children := _edge_children(same_region, destination_direction)
			if edge_children.size() != 2:
				return {"error": ERR_INVALID_DATA, "reason": "edge_children_unresolved"}
			var fine_low := edge_children[0]
			var fine_high := edge_children[1]
			var fine_low_slot := _published_slot(pool, fine_low)
			var fine_high_slot := _published_slot(pool, fine_high)
			if fine_low_slot < 0 and fine_high_slot < 0:
				continue

			var descriptor_key := _edge_key(coarse.packed(), direction)
			if seen.has(descriptor_key):
				continue
			seen[descriptor_key] = true
			var coarse_slot := int(record.get("slot", -1))
			if coarse_slot < 0:
				return {"error": ERR_INVALID_DATA, "reason": "invalid_coarse_slot"}
			words.append(coarse_slot)
			words.append(fine_low_slot)
			words.append(fine_high_slot)
			words.append(direction)
			words.append(destination_direction)
			words.append(1 if int(link.get("edge_orientation", 1)) < 0 else 0)
			words.append(0)
			words.append(0)

			claims[descriptor_key] = true
			if fine_low_slot >= 0:
				claims[_edge_key(fine_low.packed(), destination_direction)] = true
			if fine_high_slot >= 0:
				claims[_edge_key(fine_high.packed(), destination_direction)] = true

	return {
		"error": OK,
		"words": words,
		"count": words.size() / WORDS_PER_DESCRIPTOR,
		"edge_claims": claims,
	}


func _published_slot(pool: HydroTilePool, key: HydroTileKey) -> int:
	if key == null or not pool.contains(key):
		return -1
	var record := pool.record(key)
	if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
			== HydroTilePool.TileState.ALLOCATING:
		return -1
	return int(record.get("slot", -1))


## Immediate destination children that touch destination_direction, ordered by
## increasing destination edge parameter (V for W/E, U for S/N).
func _edge_children(region: HydroTileKey, direction: int) -> Array[HydroTileKey]:
	var out: Array[HydroTileKey] = []
	if region == null:
		return out
	match direction:
		HydroTileTopology.DIR_WEST:
			out = [region.child(0), region.child(2)]
		HydroTileTopology.DIR_EAST:
			out = [region.child(1), region.child(3)]
		HydroTileTopology.DIR_SOUTH:
			out = [region.child(0), region.child(1)]
		HydroTileTopology.DIR_NORTH:
			out = [region.child(2), region.child(3)]
	return out


func _touches_region_edge(candidate: HydroTileKey, region: HydroTileKey,
		direction: int) -> bool:
	if candidate == null or region == null \
			or not HydroLODHierarchy.is_ancestor(region, candidate):
		return false
	var shift := candidate.level - region.level
	var side := 1 << shift
	var local_x := candidate.x - (region.x << shift)
	var local_y := candidate.y - (region.y << shift)
	match direction:
		HydroTileTopology.DIR_WEST: return local_x == 0
		HydroTileTopology.DIR_EAST: return local_x == side - 1
		HydroTileTopology.DIR_SOUTH: return local_y == 0
		HydroTileTopology.DIR_NORTH: return local_y == side - 1
	return false


func _edge_key(tile_id: int, direction: int) -> String:
	return "%d:%d" % [tile_id, direction]


func _make_params(count: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, atlas.tile_resolution)
	bytes.encode_u32(4, atlas.capacity)
	bytes.encode_u32(8, count)
	bytes.encode_u32(12, maxi(atlas.base_tile_level, 0))
	bytes.encode_float(16, atlas.cell_size_m)
	bytes.encode_float(20, gravity)
	bytes.encode_float(24, dry_eps)
	bytes.encode_float(28, 1.0)
	return bytes


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
	var interfaces := rd.storage_buffer_create(interface_bytes.size(), interface_bytes)
	var params := rd.storage_buffer_create(PARAM_BYTES, param_bytes)
	if not pipeline.is_valid() or not interfaces.is_valid() or not params.is_valid():
		_free_many(rd, [params, interfaces, pipeline, shader])
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
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [params, interfaces, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_init", OK, {
		"shader": shader,
		"pipeline": pipeline,
		"interfaces": interfaces,
		"params": params,
		"set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_interfaces = bundle["interfaces"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _sync_render_thread(descriptor_bytes: PackedByteArray,
		param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _interfaces.is_valid() or not _params.is_valid():
		return
	var err := OK
	if not descriptor_bytes.is_empty():
		err = rd.buffer_update(_interfaces, 0, descriptor_bytes.size(), descriptor_bytes)
	if err == OK:
		err = rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if err != OK:
		push_error("HydroLODInterfaceFluxGPU: topology upload failed (%d)." % int(err))


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(rid)
	return uniform


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids := [_uniform_set, _params, _interfaces, _pipeline, _shader]
	_initialized = false
	_init_pending = false
	_interface_count = 0
	_synced_pool_revision = -1
	_edge_claims.clear()
	_uniform_set = RID(); _params = RID(); _interfaces = RID()
	_pipeline = RID(); _shader = RID(); _control = RID()
	atlas = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
