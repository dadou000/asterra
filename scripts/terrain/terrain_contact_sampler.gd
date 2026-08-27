class_name TerrainContactSampler
extends RefCounted
## Shared gameplay contact/altitude sampler for the GPU-first terrain runtime.
##
## Precise contact comes from the pooled TerrainHeightQuery service. Deltas are
## applied by that service on read, so edited terrain, player contact and vehicle
## support all share the same mutable height source.


static func request_height(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12:
		return
	TerrainHeightQuery.request_height(direction.normalized())


static func request_batch(directions: Array[Vector3]) -> void:
	TerrainHeightQuery.request_batch(directions)


static func request_surface(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12:
		return
	TerrainHeightQuery.request_surface(direction.normalized())


static func request_surfaces(directions: Array[Vector3]) -> void:
	TerrainHeightQuery.request_surfaces(directions)


static func coarse_height(direction: Vector3, snap: Dictionary = {}) -> float:
	if not Planet.ready_state or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return 0.0
	return Planet.terrain_height(direction.normalized(), null, snap)


static func height(direction: Vector3, snap: Dictionary = {}) -> float:
	if not Planet.ready_state or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return 0.0
	var d := direction.normalized()
	if not snap.is_empty():
		return Planet.terrain_height(d, null, snap)
	TerrainHeightQuery.request_height(d)
	return TerrainHeightQuery.height_for_direction(d, Planet.terrain_height(d))


static func surface(direction: Vector3) -> Dictionary:
	if not Planet.ready_state or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return {"height": 0.0, "normal": direction.normalized(), "precise": false}
	var d := direction.normalized()
	return TerrainHeightQuery.surface_for_direction(d, Planet.terrain_height(d))


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
