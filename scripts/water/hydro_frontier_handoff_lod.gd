class_name HydroFrontierHandoffLOD
extends HydroFrontierHandoffGPU
## Same-level frontier handoff with a per-source-level physical metric.
## Cross-LOD interfaces are never routed through this operator; their flux ownership
## belongs to the Phase-4 interface/reflux layer.

var scheduler: SparseHydroScheduler


func initialize_lod(p_scheduler: SparseHydroScheduler,
		atlas: SparseHydroAtlasGPU) -> Error:
	if p_scheduler == null or p_scheduler.pool == null:
		return ERR_INVALID_PARAMETER
	scheduler = p_scheduler
	return super.initialize(atlas)


func seed(source_slot: int, destination_slot: int, source_direction: int,
		destination_direction: int, reversed: bool, seed_dt_s: float,
		max_source_fraction: float = 0.12, gravity: float = 9.81) -> int:
	if scheduler == null or scheduler.pool == null:
		return -1
	var source_key := scheduler.pool.key_for_slot(source_slot)
	var destination_key := scheduler.pool.key_for_slot(destination_slot)
	if source_key == null or destination_key == null \
			or source_key.level != destination_key.level:
		return -1
	return super.seed(source_slot, destination_slot, source_direction,
		destination_direction, reversed, seed_dt_s, max_source_fraction, gravity)


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
	var source_key := scheduler.pool.key_for_slot(source_slot)
	var dx := _atlas.cell_size_for_level(source_key.level) \
		if source_key != null else _atlas.cell_size_m
	bytes.encode_float(32, maxf(dx, 1.0e-4))
	bytes.encode_float(36, seed_dt_s)
	bytes.encode_float(40, clampf(max_source_fraction, 0.0, 0.5))
	bytes.encode_float(44, maxf(gravity, 1.0e-4))
	return bytes


func release() -> void:
	scheduler = null
	super.release()
