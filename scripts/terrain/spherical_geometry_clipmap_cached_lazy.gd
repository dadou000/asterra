extends "res://scripts/terrain/spherical_geometry_clipmap_cached.gd"
## Lazy staging wrapper for the double-buffered terrain cache.
##
## The production cached renderer is unchanged while a handoff is active, but the
## spare 512x512x15 RGBA32F cache is released between handoffs. This cuts steady
## terrain-cache residency from two caches to one; peak residency is still two
## caches only during prewarm, when an atomic reanchor actually needs both.

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
