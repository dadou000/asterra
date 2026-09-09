class_name HydroSourceIngressLOD
extends HydroSourceIngress
## Production point-source ingress for a mixed physical-HydroLOD atlas.
##
## Stable source positions resolve through the currently authoritative quadtree
## owner. An H0 source therefore continues feeding the same physical point if its
## region is represented by an H1/H2 parent. Signed rates remain volumetric [m3/s]
## and are converted with the resolved owner's physical cell area.

var minimum_tile_level := 0


func request_rebuild() -> void:
	_revision += 1
	_dirty = true


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
	var min_level := clampi(minimum_tile_level, 0, atlas.base_tile_level)
	var requested_level := clampi(int(source.get("tile_level", default_tile_level)),
		min_level, atlas.base_tile_level)
	var requested_key := _key_at(face, u, v, requested_level)
	var resolved_key := requested_key

	var covering := HydroLODHierarchy.covering_record(scheduler.pool, requested_key)
	if not covering.is_empty():
		var candidate := covering.get("key") as HydroTileKey
		if candidate != null and int(covering.get("state", HydroTilePool.TileState.ALLOCATING)) \
				!= HydroTilePool.TileState.ALLOCATING:
			resolved_key = candidate
	else:
		# Explicit coarse source addresses may already be represented by a finer owner.
		# Follow the unique point path downward rather than allocating an overlap.
		for level in range(requested_level + 1, atlas.base_tile_level + 1):
			var candidate := _key_at(face, u, v, level)
			if not scheduler.pool.contains(candidate):
				continue
			var record := scheduler.pool.record(candidate)
			if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
					== HydroTilePool.TileState.ALLOCATING:
				continue
			resolved_key = candidate
			break

	var side := float(1 << resolved_key.level)
	var gx := clampf((u + 1.0) * 0.5 * side, 0.0, side - 1.0e-9)
	var gy := clampf((v + 1.0) * 0.5 * side, 0.0, side - 1.0e-9)
	var local_x := clampi(int(floor((gx - float(resolved_key.x))
		* float(atlas.tile_resolution))), 0, atlas.tile_resolution - 1)
	var local_y := clampi(int(floor((gy - float(resolved_key.y))
		* float(atlas.tile_resolution))), 0, atlas.tile_resolution - 1)
	return {
		"key": resolved_key,
		"cell": local_y * atlas.tile_resolution + local_x,
		"cell_xy": Vector2i(local_x, local_y),
		"face_uv": Vector2(u, v),
		"requested_level": requested_level,
		"resolved_level": resolved_key.level,
		"physical_lod": atlas.physical_lod_for_level(resolved_key.level),
	}


func _key_at(face: int, u: float, v: float, level: int) -> HydroTileKey:
	var side := 1 << level
	var gx := clampf((u + 1.0) * 0.5 * float(side), 0.0,
		float(side) - 1.0e-9)
	var gy := clampf((v + 1.0) * 0.5 * float(side), 0.0,
		float(side) - 1.0e-9)
	return HydroTileKey.new(face, level,
		clampi(int(floor(gx)), 0, side - 1),
		clampi(int(floor(gy)), 0, side - 1))


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
		var physical_lod := maxi(atlas.physical_lod_for_level(key.level), 0)
		slot = scheduler.reserve(key, physical_lod, "point_source")
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
	if not bool(result.get("queued", false)) \
			or int(result.get("error", ERR_INVALID_DATA)) != OK:
		scheduler.cancel_reserved(key, "source_terrain_stage_rejected")
		_fail_flush(int(result.get("error", ERR_INVALID_DATA)), "source_terrain_stage")
		return
	_pending_stage_request = int(result.get("request_id", -1))
	_pending_stage_source_id = source_id
	_pending_stage_key = key
	_pending_stage_slot = slot


func _rebuild_gpu_terms() -> void:
	var aggregated: Dictionary = {}
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
			continue
		var record := scheduler.pool.record(key)
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			continue
		var cell_area := maxf(atlas.cell_area_for_level(key.level), 1.0e-8)
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
