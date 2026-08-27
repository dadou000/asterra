extends "res://scripts/terrain/spherical_geometry_clipmap_occlusion.gd"
## Final production terrain entry point.
##
## Adds lazy lifetime management to the inherited double-buffered terrain cache.
## The active cache is always resident. The spare cache exists only while a
## reanchor is being prewarmed, reducing steady terrain-cache VRAM by ~60 MiB
## while preserving the parent's atomic warm-cache swap behavior.

func _ready() -> void:
	super._ready()
	_release_staging_cache()


func _start_handoff(target_offset: Vector2, retarget: bool) -> void:
	_ensure_staging_cache()
	super._start_handoff(target_offset, retarget)


func _cancel_handoff() -> void:
	super._cancel_handoff()
	_release_staging_cache()


func _ensure_staging_cache() -> void:
	if _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging):
		return
	_terrain_cache_staging = TerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheStaging"
	add_child(_terrain_cache_staging)


func _release_staging_cache() -> void:
	if _terrain_cache_staging == null:
		return
	if is_instance_valid(_terrain_cache_staging):
		_terrain_cache_staging.queue_free()
	_terrain_cache_staging = null


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_cache_architecture"] = "lazy_double_buffered_toroidal_gpu"
	out["terrain_cache_staging_allocated"] = _terrain_cache_staging != null \
		and is_instance_valid(_terrain_cache_staging)
	out["terrain_cache_steady_buffers"] = 1
	out["terrain_cache_peak_buffers"] = 2
	return out
