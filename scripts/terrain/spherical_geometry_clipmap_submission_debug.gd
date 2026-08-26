extends "res://scripts/terrain/spherical_geometry_clipmap_global_gpu.gd"
## Final terrain submission gate used by diagnostics.
##
## Horizon selection, screen-space LOD and view culling remain owned by the
## production clipmap parents. This layer only filters the already-selected logical
## levels immediately before submission. Arbitrary disabled LODs are compacted into
## the MultiMesh visible prefix, so unchecked levels execute no vertex shader work.

var _debug_level_enabled := PackedByteArray()
var _debug_source_ring_prefix: int = 0
var _debug_submitted_levels: Array[int] = []


func _ready() -> void:
	_init_debug_level_mask()
	super._ready()


func _init_debug_level_mask() -> void:
	if _debug_level_enabled.size() == MAX_LEVEL + 1:
		return
	_debug_level_enabled.resize(MAX_LEVEL + 1)
	for level: int in MAX_LEVEL + 1:
		_debug_level_enabled[level] = 1


func _apply_active_level_window() -> void:
	_init_debug_level_mask()
	super._apply_active_level_window()
	_debug_source_ring_prefix = _physical_ring_count
	_apply_debug_level_submission(_debug_source_ring_prefix)
	_apply_debug_center_mask()


func _update_sector_visibility() -> void:
	_init_debug_level_mask()
	super._update_sector_visibility()
	if _view_surface_culled:
		_debug_submitted_levels.clear()
		return
	# global_gpu leaves _view_ring_instances as the contiguous prefix selected by
	# horizon/nadir culling. Capture it before replacing the actual submitted prefix.
	_debug_source_ring_prefix = clampi(_view_ring_instances, 0, _physical_ring_count)
	_apply_debug_level_submission(_debug_source_ring_prefix)
	_apply_debug_center_mask()


func _show_all_active_sectors() -> void:
	_init_debug_level_mask()
	super._show_all_active_sectors()
	_debug_source_ring_prefix = _physical_ring_count
	_apply_debug_level_submission(_debug_source_ring_prefix)
	_apply_debug_center_mask()


# Compatibility hardening: if any older/debug code ever calls the historical
# restore hook, route it through this final filter rather than resurrecting every
# physical ring. No current production path needs this method.
func _restore_dynamic_ring_window() -> void:
	_init_debug_level_mask()
	var prefix: int = clampi(_debug_source_ring_prefix, 0, _physical_ring_count)
	if prefix == 0 and _physical_ring_count > 0 and not _view_surface_culled:
		prefix = _physical_ring_count
	_apply_debug_level_submission(prefix)


func _apply_debug_level_submission(source_prefix: int) -> void:
	_init_debug_level_mask()
	source_prefix = clampi(source_prefix, 0, _physical_ring_count)
	_debug_submitted_levels.clear()

	for source_index: int in source_prefix:
		var logical_level: int = _active_min_level + source_index + 1
		if debug_level_enabled(logical_level):
			_debug_submitted_levels.append(logical_level)

	var submitted_count: int = _debug_submitted_levels.size()
	for sector: int in _sector_batches.size():
		var batch: MultiMeshInstance3D = _sector_batches[sector]
		if batch.multimesh == null:
			continue
		var mm: MultiMesh = batch.multimesh
		var capacity: int = mm.instance_count
		var actual_count: int = mini(submitted_count, capacity)
		for slot: int in actual_count:
			mm.set_instance_custom_data(slot,
				Color(float(_debug_submitted_levels[slot]), float(sector), 1.0, 0.0))
		if mm.visible_instance_count != actual_count:
			mm.visible_instance_count = actual_count

	# From this point onward the statistic means actual submitted ring instances,
	# not the pre-mask contiguous view prefix.
	_view_ring_instances = submitted_count


func _apply_debug_center_mask() -> void:
	if debug_level_enabled(_active_min_level):
		return
	for batch: MultiMeshInstance3D in _center_sector_batches:
		batch.visible = false


func set_debug_level_enabled(level: int, enabled: bool) -> void:
	_init_debug_level_mask()
	if level < 0 or level > MAX_LEVEL:
		return
	var next_value: int = 1 if enabled else 0
	if int(_debug_level_enabled[level]) == next_value:
		return
	_debug_level_enabled[level] = next_value
	_refresh_debug_level_submission()


func set_all_debug_levels_enabled(enabled: bool) -> void:
	_init_debug_level_mask()
	var value: int = 1 if enabled else 0
	for level: int in MAX_LEVEL + 1:
		_debug_level_enabled[level] = value
	_refresh_debug_level_submission()


func debug_level_enabled(level: int) -> bool:
	_init_debug_level_mask()
	return level >= 0 and level <= MAX_LEVEL and _debug_level_enabled[level] != 0


func debug_level_count() -> int:
	return MAX_LEVEL + 1


func debug_submitted_levels() -> Array[int]:
	var out: Array[int] = []
	out.assign(_debug_submitted_levels)
	return out


func debug_enabled_levels() -> Array[int]:
	_init_debug_level_mask()
	var out: Array[int] = []
	for level: int in MAX_LEVEL + 1:
		if _debug_level_enabled[level] != 0:
			out.append(level)
	return out


func _refresh_debug_level_submission() -> void:
	if not _terrain_visible:
		return
	if _debug_side_cut:
		_show_all_active_sectors()
	else:
		_update_sector_visibility()


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	var center_submitted := not _view_surface_culled and debug_level_enabled(_active_min_level)
	var ring_submitted := not _view_surface_culled and not _debug_submitted_levels.is_empty()
	var batch_kinds := (1 if center_submitted else 0) + (1 if ring_submitted else 0)
	out["draw_batches"] = _visible_sector_count * batch_kinds
	out["debug_level_filter"] = true
	out["debug_enabled_levels"] = debug_enabled_levels()
	out["submitted_logical_levels"] = debug_submitted_levels()
	out["view_ring_prefix_before_level_mask"] = _debug_source_ring_prefix
	return out
