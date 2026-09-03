class_name HydroSourceIngressLOD
extends HydroSourceIngress
## Production point-source ingress for a mixed physical-HydroLOD atlas.
##
## Signed source rates are volumetric [m3/s]. Converting them to solver depth rate
## must use the resolved tile's physical cell area, not the H0 atlas area.


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
