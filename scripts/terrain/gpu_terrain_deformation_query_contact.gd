extends "res://scripts/terrain/gpu_terrain_deformation_query.gd"
## Contact-grade freshness check for the active deformation field.
##
## Normal gameplay may use a one/two-update-old cached value to avoid stalls. A new
## rigid contact cannot: it must not lock its reference plane from stale deformation
## data or it can start excavating while the visible object is still above ground.


func has_fresh_sample(direction: Vector3) -> bool:
	if not ready_state or failed or not TerrainDeformationGPU.ready_state:
		return false
	if not bool(TerrainDeformationGPU.get("_has_active_content")):
		return true
	if direction.length_squared() <= 1e-12:
		return false
	var local_m: Vector2 = TerrainDeformationGPU.project_local(direction.normalized())
	if not is_finite(local_m.x) or not is_finite(local_m.y):
		return true
	var half_extent_m: float = float(TerrainDeformationGPU.HALF_EXTENT_M)
	if absf(local_m.x) >= half_extent_m or absf(local_m.y) >= half_extent_m:
		return true
	var key: int = _sample_key(local_m)
	var field_generation: int = TerrainDeformationGPU.field_generation()
	if _cache.has(key):
		var cached_generation: int = int(_cache_generation.get(key, -1))
		if cached_generation >= field_generation:
			return true
	_queue_sample(local_m, key)
	return false
