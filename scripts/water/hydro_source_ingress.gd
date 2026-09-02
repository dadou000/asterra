class_name HydroSourceIngress
extends Node
## Generic conservative ingress/egress layer for sparse hydrology.
##
## Point sources are stable world-space directions plus signed volumetric rates.
## Positive Q can create the first sparse tile. Negative Q is a sink and never
## allocates a missing dry tile. Source definitions are resolved to stable
## HydroTileKeys/cells only when the compact GPU source-term buffer is rebuilt.
##
## This is intentionally generic: glacier melt, springs, reservoir outlets, pipes,
## pumps and gameplay emitters can all use the same API. Rainfall/runoff will later
## use a distributed field path rather than creating millions of point entries.

signal initialized
signal initialization_failed(error: Error)
signal source_changed(source_id: String)
signal source_tile_activated(source_id: String, tile_id: int, slot: int)
signal flush_started(request_id: int, source_count: int)
signal flush_completed(request_id: int, source_count: int, entry_count: int)
signal flush_failed(request_id: int, error: Error, stage: String)
signal released

var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge
var terrain_bed: HydroTerrainBedGPU
var writer: HydroSourceTermsGPU

var default_tile_level := 12
var max_point_sources := 8192

var _sources: Dictionary = {} # String -> Dictionary
var _initialized := false
var _busy := false
var _dirty := false
var _next_flush_id := 1
var _flush_id := -1
var _activation_queue: Array[Dictionary] = []
var _activation_index := 0
var _pending_stage_request := -1
var _pending_stage_source_id := ""
var _pending_stage_key: HydroTileKey
var _pending_stage_slot := -1


func initialize(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		p_terrain_bed: HydroTerrainBedGPU) -> Error:
	if _initialized or _busy:
		return ERR_BUSY
	if p_scheduler == null or p_atlas == null or p_connectivity == null \
			or p_identity_bridge == null or p_terrain_bed == null:
		return ERR_INVALID_PARAMETER
	if not p_atlas.initialized_ok() or not p_connectivity.initialized_ok() \
			or not p_identity_bridge.is_bound() or not p_terrain_bed.initialized_ok():
		return ERR_UNCONFIGURED
	if p_scheduler.pool.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	identity_bridge = p_identity_bridge
	terrain_bed = p_terrain_bed
	terrain_bed.stage_recorded.connect(_on_terrain_stage_recorded)
	terrain_bed.stage_failed.connect(_on_terrain_stage_failed)

	writer = HydroSourceTermsGPU.new()
	writer.name = "HydroSourceTermsGPU"
	add_child(writer)
	writer.initialized.connect(_on_writer_initialized)
	writer.initialization_failed.connect(_on_writer_init_failed)
	writer.update_recorded.connect(_on_writer_update_recorded)
	writer.update_failed.connect(_on_writer_update_failed)
	return writer.initialize(atlas, max_point_sources)


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


func dirty() -> bool:
	return _dirty


func source_count() -> int:
	return _sources.size()


func source_ids() -> Array[String]:
	var out: Array[String] = []
	for id: Variant in _sources.keys():
		out.append(String(id))
	return out


## Signed volumetric flow Q [m^3/s]. Positive adds water; negative removes it.
## injection_velocity_world is used only for positive Q. Its radial component is
## discarded; the tangent component is transformed into the tile's local u/v frame.
func upsert_point_source(source_id: String, direction: Vector3,
		rate_m3_s: float, injection_velocity_world: Vector3 = Vector3.ZERO,
		tile_level: int = -1, enabled: bool = true) -> Error:
	if source_id.is_empty() or direction.length_squared() < 0.5 \
			or not is_finite(rate_m3_s) or not _finite_vec3(injection_velocity_world):
		return ERR_INVALID_PARAMETER
	var level := default_tile_level if tile_level < 0 else tile_level
	level = clampi(level, 0, HydroTileKey.MAX_LEVEL)
	_sources[source_id] = {
		"id": source_id,
		"direction": direction.normalized(),
		"rate_m3_s": rate_m3_s,
		"velocity_world": injection_velocity_world,
		"tile_level": level,
		"enabled": enabled,
	}
	_dirty = true
	source_changed.emit(source_id)
	return OK


func remove_source(source_id: String) -> bool:
	if not _sources.erase(source_id):
		return false
	_dirty = true
	source_changed.emit(source_id)
	return true


func set_source_enabled(source_id: String, enabled: bool) -> bool:
	var value: Variant = _sources.get(source_id, null)
	if not (value is Dictionary):
		return false
	var source: Dictionary = value
	source["enabled"] = enabled
	_sources[source_id] = source
	_dirty = true
	source_changed.emit(source_id)
	return true


## Apply all pending source-definition changes. Call only at a hydrology runtime
## phase boundary; this method may allocate/initialize new sparse tiles.
func flush() -> int:
	if not _initialized or _busy:
		return -1
	if not _dirty:
		return 0
	_flush_id = _next_flush_id
	_next_flush_id += 1
	_busy = true
	_activation_queue = _collect_missing_positive_source_tiles()
	_activation_index = 0
	flush_started.emit(_flush_id, _sources.size())
	if _activation_queue.is_empty():
		_rebuild_gpu_terms()
	else:
		_start_next_activation()
	return _flush_id


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"busy": _busy,
		"dirty": _dirty,
		"source_count": _sources.size(),
		"default_tile_level": default_tile_level,
		"max_point_sources": max_point_sources,
		"activation_queue": _activation_queue.size() - _activation_index,
		"writer_gpu_bytes": 0 if writer == null else writer.gpu_bytes_estimate(),
	}


func _collect_missing_positive_source_tiles() -> Array[Dictionary]:
	var by_tile: Dictionary = {}
	for id_variant: Variant in _sources.keys():
		var id := String(id_variant)
		var source: Dictionary = _sources[id]
		if not bool(source.get("enabled", true)) or float(source.get("rate_m3_s", 0.0)) <= 0.0:
			continue
		var resolved := _resolve_source(source)
		if resolved.is_empty():
			continue
		var key := resolved["key"] as HydroTileKey
		if scheduler.pool.contains(key):
			var record := scheduler.pool.record(key)
			if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
					!= HydroTilePool.TileState.ALLOCATING:
				continue
		var tile_id := key.packed()
		if not by_tile.has(tile_id):
			by_tile[tile_id] = {"key": key, "source_id": id}
	var out: Array[Dictionary] = []
	for value: Variant in by_tile.values():
		out.append((value as Dictionary).duplicate(true))
	return out


func _start_next_activation() -> void:
	if not _busy:
		return
	if _activation_index >= _activation_queue.size():
		_rebuild_gpu_terms()
		return
	var item := _activation_queue[_activation_index]
	var key := item["key"] as HydroTileKey
	var source_id := String(item.get("source_id", ""))
	var slot := scheduler.pool.slot_for(key)
	if slot < 0:
		slot = scheduler.reserve(key, 0, "point_source")
	if slot < 0:
		_fail_flush(ERR_CANT_ACQUIRE_RESOURCE, "source_tile_reserve")
		return
	var record := scheduler.pool.record(key)
	if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
			!= HydroTilePool.TileState.ALLOCATING:
		_activation_index += 1
		_start_next_activation()
		return

	var result := terrain_bed.stage_reserved_tile(key, slot)
	if not bool(result.get("queued", false)) or int(result.get("error", ERR_INVALID_DATA)) != OK:
		scheduler.cancel_reserved(key, "source_terrain_stage_rejected")
		_fail_flush(int(result.get("error", ERR_INVALID_DATA)), "source_terrain_stage")
		return
	_pending_stage_request = int(result.get("request_id", -1))
	_pending_stage_source_id = source_id
	_pending_stage_key = key
	_pending_stage_slot = slot


func _on_terrain_stage_recorded(request_id: int, tile_id: int, slot: int) -> void:
	if not _busy or request_id != _pending_stage_request or _pending_stage_key == null \
			or tile_id != _pending_stage_key.packed() or slot != _pending_stage_slot:
		return
	var activated_slot := scheduler.activate_reserved(_pending_stage_key,
		"point_source_terrain_ready")
	if activated_slot != slot:
		_fail_flush(ERR_CANT_ACQUIRE_RESOURCE, "source_tile_activate")
		return
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_fail_flush(conn_error, "source_connectivity")
		return
	source_tile_activated.emit(_pending_stage_source_id,
		_pending_stage_key.packed(), activated_slot)
	_pending_stage_request = -1
	_pending_stage_source_id = ""
	_pending_stage_key = null
	_pending_stage_slot = -1
	_activation_index += 1
	_start_next_activation()


func _on_terrain_stage_failed(request_id: int, error: Error) -> void:
	if not _busy or request_id != _pending_stage_request:
		return
	if _pending_stage_key != null:
		scheduler.cancel_reserved(_pending_stage_key, "source_terrain_stage_failed")
	_fail_flush(error, "source_terrain_stage")


func _rebuild_gpu_terms() -> void:
	var aggregated: Dictionary = {}
	var cell_area := maxf(atlas.cell_size_m * atlas.cell_size_m, 1.0e-8)
	for source_variant: Variant in _sources.values():
		if not (source_variant is Dictionary):
			continue
		var source: Dictionary = source_variant
		if not bool(source.get("enabled", true)):
			continue
		var rate := float(source.get("rate_m3_s", 0.0))
		if absf(rate) <= 1.0e-12:
			continue
		var resolved := _resolve_source(source)
		if resolved.is_empty():
			continue
		var key := resolved["key"] as HydroTileKey
		var slot := scheduler.pool.slot_for(key)
		if slot < 0:
			# Negative-only sinks on absent dry tiles are intentionally ignored.
			continue
		var record := scheduler.pool.record(key)
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			continue
		var cell := int(resolved["cell"])
		var aggregate_key := "%d:%d" % [slot, cell]
		var entry: Dictionary = aggregated.get(aggregate_key, {
			"slot": slot, "cell": cell,
			"add_depth_rate_mps": 0.0, "remove_depth_rate_mps": 0.0,
			"hu_rate": 0.0, "hv_rate": 0.0,
		})
		if rate > 0.0:
			var dh_rate := rate / cell_area
			var local_velocity := _world_velocity_to_face_uv(
				source["direction"], source["velocity_world"])
			entry["add_depth_rate_mps"] = float(entry["add_depth_rate_mps"]) + dh_rate
			entry["hu_rate"] = float(entry["hu_rate"]) + dh_rate * local_velocity.x
			entry["hv_rate"] = float(entry["hv_rate"]) + dh_rate * local_velocity.y
		else:
			entry["remove_depth_rate_mps"] = float(entry["remove_depth_rate_mps"]) \
				+ (-rate) / cell_area
		aggregated[aggregate_key] = entry

	var entries: Array[Dictionary] = []
	for value: Variant in aggregated.values():
		entries.append((value as Dictionary).duplicate(true))
	var request := writer.update_entries(entries)
	if request < 0:
		_fail_flush(ERR_BUSY, "source_gpu_update")


func _resolve_source(source: Dictionary) -> Dictionary:
	var d: Vector3 = source.get("direction", Vector3.ZERO)
	if d.length_squared() < 0.5:
		return {}
	var mapped := CubeSphere.dir_to_face_uv(d.normalized())
	if mapped.size() < 3:
		return {}
	var face := int(mapped[0])
	var u := clampf(float(mapped[1]), -1.0, 1.0)
	var v := clampf(float(mapped[2]), -1.0, 1.0)
	var level := clampi(int(source.get("tile_level", default_tile_level)),
		0, HydroTileKey.MAX_LEVEL)
	var side := 1 << level
	var gx := clampf((u + 1.0) * 0.5 * float(side), 0.0,
		float(side) - 1.0e-9)
	var gy := clampf((v + 1.0) * 0.5 * float(side), 0.0,
		float(side) - 1.0e-9)
	var tx := clampi(int(floor(gx)), 0, side - 1)
	var ty := clampi(int(floor(gy)), 0, side - 1)
	var local_x := clampi(int(floor((gx - float(tx)) * float(atlas.tile_resolution))),
		0, atlas.tile_resolution - 1)
	var local_y := clampi(int(floor((gy - float(ty)) * float(atlas.tile_resolution))),
		0, atlas.tile_resolution - 1)
	return {
		"key": HydroTileKey.new(face, level, tx, ty),
		"cell": local_y * atlas.tile_resolution + local_x,
		"cell_xy": Vector2i(local_x, local_y),
		"face_uv": Vector2(u, v),
	}


func _world_velocity_to_face_uv(direction: Vector3, velocity_world: Vector3) -> Vector2:
	if velocity_world.length_squared() <= 1.0e-20:
		return Vector2.ZERO
	var d := direction.normalized()
	var mapped := CubeSphere.dir_to_face_uv(d)
	var face := int(mapped[0])
	var u := float(mapped[1])
	var v := float(mapped[2])
	const EPS := 1.0e-4
	var du := (CubeSphere.face_uv_to_dir(face, clampf(u + EPS, -1.0, 1.0), v)
		- CubeSphere.face_uv_to_dir(face, clampf(u - EPS, -1.0, 1.0), v)).normalized()
	var dv := (CubeSphere.face_uv_to_dir(face, u, clampf(v + EPS, -1.0, 1.0))
		- CubeSphere.face_uv_to_dir(face, u, clampf(v - EPS, -1.0, 1.0))).normalized()
	var tangent_velocity := velocity_world - d * velocity_world.dot(d)
	return Vector2(tangent_velocity.dot(du), tangent_velocity.dot(dv))


func _on_writer_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_writer_init_failed(error: Error) -> void:
	initialization_failed.emit(error)


func _on_writer_update_recorded(_request_id: int, entry_count: int) -> void:
	if not _busy:
		return
	var completed_id := _flush_id
	var source_total := _sources.size()
	_busy = false
	_dirty = false
	_reset_flush_state()
	flush_completed.emit(completed_id, source_total, entry_count)


func _on_writer_update_failed(_request_id: int, error: Error) -> void:
	if _busy:
		_fail_flush(error, "source_gpu_update")


func _fail_flush(error: Error, stage: String) -> void:
	var failed_id := _flush_id
	_busy = false
	_dirty = true
	_reset_flush_state()
	flush_failed.emit(failed_id, error, stage)


func _reset_flush_state() -> void:
	_flush_id = -1
	_activation_queue = []
	_activation_index = 0
	_pending_stage_request = -1
	_pending_stage_source_id = ""
	_pending_stage_key = null
	_pending_stage_slot = -1


func _finite_vec3(v: Vector3) -> bool:
	return is_finite(v.x) and is_finite(v.y) and is_finite(v.z)


func release() -> void:
	if terrain_bed != null:
		if terrain_bed.stage_recorded.is_connected(_on_terrain_stage_recorded):
			terrain_bed.stage_recorded.disconnect(_on_terrain_stage_recorded)
		if terrain_bed.stage_failed.is_connected(_on_terrain_stage_failed):
			terrain_bed.stage_failed.disconnect(_on_terrain_stage_failed)
	if writer != null and is_instance_valid(writer):
		writer.release()
		writer.queue_free()
	writer = null
	_initialized = false
	_busy = false
	_dirty = false
	_reset_flush_state()
	_sources.clear()
	scheduler = null
	atlas = null
	connectivity = null
	identity_bridge = null
	terrain_bed = null
	released.emit()


func _exit_tree() -> void:
	release()
