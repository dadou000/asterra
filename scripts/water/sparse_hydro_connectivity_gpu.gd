class_name SparseHydroConnectivityGPU
extends Node
## Same-level sparse tile connectivity mirrored to GPU.
##
## Four entries per source slot, ordered W/E/S/N:
##   neighbor_slots[edge] = destination transient slot, or -1 when not resident
##   neighbor_links[edge] = destination_edge[0..3] | reversed_bit<<2
##
## Stable geometry comes from HydroTileTopology, so cube-face seam orientation is
## identical to terrain. The buffers contain no persistent identity; if slots are
## recycled, sync_pool() rebuilds connectivity from the authoritative pool.

signal initialized
signal initialization_failed(error: Error)
signal released

const EDGES := 4
const REVERSED_BIT := 1 << 2

var capacity := 0
var _neighbor_slots := RID()
var _neighbor_links := RID()
var _initialized := false
var _init_pending := false


func initialize(p_capacity: int) -> Error:
	if _initialized or _init_pending:
		return ERR_BUSY
	if p_capacity <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	capacity = p_capacity
	var arrays := build_arrays(null, capacity)
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		(arrays["neighbor_slots"] as PackedInt32Array).to_byte_array(),
		(arrays["neighbor_links"] as PackedInt32Array).to_byte_array()))
	return OK


func initialized_ok() -> bool:
	return _initialized


func neighbor_slots_rid() -> RID:
	return _neighbor_slots if _initialized else RID()


func neighbor_links_rid() -> RID:
	return _neighbor_links if _initialized else RID()


func gpu_bytes_estimate() -> int:
	return capacity * EDGES * 4 * 2


func sync_pool(pool: HydroTilePool) -> Error:
	if not _initialized or pool == null or pool.capacity != capacity:
		return ERR_INVALID_PARAMETER
	var arrays := build_arrays(pool, capacity)
	RenderingServer.call_on_render_thread(Callable(self, &"_sync_render_thread").bind(
		(arrays["neighbor_slots"] as PackedInt32Array).to_byte_array(),
		(arrays["neighbor_links"] as PackedInt32Array).to_byte_array()))
	return OK


static func build_arrays(pool: HydroTilePool, p_capacity: int = -1) -> Dictionary:
	var cap := p_capacity
	if pool != null:
		cap = pool.capacity
	cap = maxi(cap, 0)
	var slots := PackedInt32Array()
	var links := PackedInt32Array()
	slots.resize(cap * EDGES)
	links.resize(cap * EDGES)
	for i in slots.size():
		slots[i] = -1
		links[i] = -1
	if pool == null:
		return {"neighbor_slots": slots, "neighbor_links": links}

	for source_slot in cap:
		var source_id := pool.id_for_slot(source_slot)
		if source_id < 0:
			continue
		var source := HydroTileKey.unpack(source_id)
		for direction in EDGES:
			var link := HydroTileTopology.neighbor(source, direction)
			if link.is_empty():
				continue
			var destination := link["key"] as HydroTileKey
			var destination_slot := pool.slot_for(destination)
			if destination_slot < 0:
				continue
			var index := source_slot * EDGES + direction
			slots[index] = destination_slot
			var destination_direction := int(link["destination_direction"]) & 3
			var reversed := int(link["edge_orientation"]) < 0
			links[index] = destination_direction | (REVERSED_BIT if reversed else 0)
	return {"neighbor_slots": slots, "neighbor_links": links}


static func unpack_destination_direction(link_value: int) -> int:
	return link_value & 3


static func unpack_reversed(link_value: int) -> bool:
	return (link_value & REVERSED_BIT) != 0


func _init_render_thread(slot_bytes: PackedByteArray, link_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var slots := rd.storage_buffer_create(slot_bytes.size(), slot_bytes)
	var links := rd.storage_buffer_create(link_bytes.size(), link_bytes)
	if not slots.is_valid() or not links.is_valid():
		_free_many(rd, [slots, links])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"neighbor_slots": slots, "neighbor_links": links,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_neighbor_slots = bundle["neighbor_slots"]
	_neighbor_links = bundle["neighbor_links"]
	_initialized = true
	initialized.emit()


func _sync_render_thread(slot_bytes: PackedByteArray, link_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	var err := rd.buffer_update(_neighbor_slots, 0, slot_bytes.size(), slot_bytes)
	if err != OK:
		push_error("SparseHydroConnectivityGPU: neighbor slot sync failed (%d)" % int(err))
		return
	err = rd.buffer_update(_neighbor_links, 0, link_bytes.size(), link_bytes)
	if err != OK:
		push_error("SparseHydroConnectivityGPU: neighbor link sync failed (%d)" % int(err))


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _neighbor_slots.is_valid():
		return
	var rids := [_neighbor_slots, _neighbor_links]
	_initialized = false
	_neighbor_slots = RID(); _neighbor_links = RID()
	RenderingServer.call_on_render_thread(Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
