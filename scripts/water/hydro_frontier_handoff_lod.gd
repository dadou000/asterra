class_name HydroFrontierHandoffLOD
extends HydroFrontierHandoffGPU
## Same-level frontier handoff with a per-source-level physical metric.
## Cross-LOD interfaces are not routed through this operator; their flux ownership
## belongs to the Phase-4 interface/reflux layer.

var scheduler: SparseHydroScheduler


func initialize_lod(p_scheduler: SparseHydroScheduler,
		atlas: SparseHydroAtlasGPU) -> Error:
	if p_scheduler == null or p_scheduler.pool == null:
		return ERR_INVALID_PARAMETER
	scheduler = p_scheduler
	return super.initialize(atlas)


func _make_params(source_slot: int, destination_slot: int, source_direction: int,
		destination_direction: int, reversed: bool, seed_dt_s: float,
		max_source_fraction: float, gravity: float) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, source_slot)
	bytes.encode_u32(4, destination_slot)
	bytes.encode_u32(8, source_direction)
	bytes.encode_u32(12, destination_direction)
	bytes.encode_u32(16, 1 if reversed else 0)
	bytes.encode_u32(20, _atlas.tile_resolution)
	bytes.encode_u32(24, _atlas.capacity)
	bytes.encode_u32(28, 0)
	var dx := _atlas.cell_size_m
	if scheduler != null and scheduler.pool != null:
		var source_key := scheduler.pool.key_for_slot(source_slot)
		var destination_key := scheduler.pool.key_for_slot(destination_slot)
		# This handoff is intentionally same-level. Rejecting a mixed pair in seed()
		# would require changing the stable base ABI, so encode an impossible zero dt
		# here if policy ever violates that contract; the shader then moves no parcel.
		if source_key != null and destination_key != null \
				and source_key.level == destination_key.level:
			dx = _atlas.cell_size_for_level(source_key.level)
		elif source_key != null:
			dx = _atlas.cell_size_for_level(source_key.level)
			seed_dt_s = 0.0
	bytes.encode_float(32, maxf(dx, 1.0e-4))
	bytes.encode_float(36, maxf(seed_dt_s, 0.0))
	bytes.encode_float(40, clampf(max_source_fraction, 0.0, 0.5))
	bytes.encode_float(44, maxf(gravity, 1.0e-4))
	return bytes


func release() -> void:
	scheduler = null
	super.release()
