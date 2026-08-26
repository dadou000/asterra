class_name TerrainContactSampler
extends RefCounted
## Shared gameplay contact/altitude sampler for the GPU-first terrain runtime.
##
## Altitude is always measured geocentrically against the configured planet radius:
##
##     MSL = |world_position| - planet_radius
##     AGL = MSL - terrain_height(direction)
##
## Precise near-observer height comes from TerrainHeightQuery, which evaluates the
## same resident macro + GPU geomorph function as visible L0 terrain. CPU fallback
## is the resident coarse Planet height only; this class never calls TerrainDetail
## or GroundHeightStore procedural synthesis.


static func request_height(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12:
		return
	TerrainHeightQuery.request_height(direction.normalized())


static func coarse_height(direction: Vector3, snap: Dictionary = {}) -> float:
	if not Planet.ready_state or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return 0.0
	return Planet.terrain_height(direction.normalized(), null, snap)


static func height(direction: Vector3, snap: Dictionary = {}) -> float:
	if not Planet.ready_state or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return 0.0
	var d := direction.normalized()
	# Snapshot queries are editor/save consistency requests. The asynchronous GPU
	# query represents current live deltas, so snapshots intentionally use coarse
	# resident height rather than mixing two delta generations.
	if not snap.is_empty():
		return Planet.terrain_height(d, null, snap)
	TerrainHeightQuery.request_height(d)
	var fallback := Planet.terrain_height(d)
	return TerrainHeightQuery.height_for_direction(d, fallback)


static func altitude_msl(world_pos: Vec3D) -> float:
	if Planet.cfg == null or world_pos.length_sq() <= 1.0:
		return 0.0
	return world_pos.length() - Planet.cfg.planet_radius


static func altitude_agl(world_pos: Vec3D) -> float:
	if Planet.cfg == null or world_pos.length_sq() <= 1.0:
		return 0.0
	var d := world_pos.normalized().to_v3()
	return altitude_msl(world_pos) - height(d)


static func surface_world(direction: Vector3, clearance_m: float = 0.0) -> Vec3D:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return Vec3D.new(0.0, 0.0, 0.0)
	var d := direction.normalized()
	var r := Planet.cfg.planet_radius + height(d) + clearance_m
	return Vec3D.new(d.x * r, d.y * r, d.z * r)
