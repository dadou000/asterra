extends "res://scripts/terrain/spherical_geometry_clipmap_blankaware.gd"
## Phase 42A cache-factory bridge.
##
## No topology/process policy changes live here. It only replaces the authoritative
## active/staging cache node class so both roles synthesize from the shared geomorph
## control contract. The existing budget/handoff lifecycle remains inherited.

const Phase42TerrainClipmapCacheScript := preload(
	"res://scripts/terrain/gpu_terrain_clipmap_cache_phase42_active.gd")


func _replace_initial_active_cache() -> void:
	var previous_active: Node = _terrain_cache_active
	if previous_active != null and is_instance_valid(previous_active):
		previous_active.queue_free()
	_terrain_cache_active = Phase42TerrainClipmapCacheScript.new()
	_terrain_cache_active.name = "GPUTerrainClipmapCacheActivePhase42"
	_terrain_cache_active.call("set_staging_budget_mode", false)
	add_child(_terrain_cache_active)
	_force_cache_rebind()


func _ensure_staging_cache() -> void:
	if _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging):
		if not bool(_terrain_cache_staging.get("failed")):
			_set_cache_role(_terrain_cache_staging, true)
			_staging_reuses += 1
			return
		_release_staging_cache()
	_terrain_cache_staging = Phase42TerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheStagingPhase42"
	_terrain_cache_staging.call("set_staging_budget_mode", true)
	add_child(_terrain_cache_staging)
	_staging_allocations += 1
