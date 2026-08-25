class_name TerrainContactSampler
extends RefCounted
## Shared CPU contact sampler for gameplay against the GPU terrain.
##
## The visual terrain and streamed collision both derive from GroundHeightStore.
## Gameplay contact queries must also use the collision stream's band limit,
## otherwise a character can stand on detail that the triangle collision mesh
## intentionally cannot represent at its current grid spacing.

static func collision_level() -> int:
	if Planet.cfg == null:
		return GroundHeightStore.MAX_LEVEL
	var depth := clampi(Planet.cfg.collision_stream_depth, 1, 19)
	var divisions := 1 << depth
	var tile_size_uv := 2.0 / float(divisions)
	var grid := maxi(Planet.cfg.collision_grid, 4)
	var sample_spacing := tile_size_uv * PI * 0.25 * Planet.cfg.planet_radius / float(grid)
	return GroundHeightStore.level_for_spacing(sample_spacing)

static func height(direction: Vector3, snap: Dictionary = {}) -> float:
	if not Planet.ready_state:
		return 0.0
	return GroundHeightStore.sample_height(direction.normalized(), collision_level(), snap)

static func altitude_agl(world_pos: Vec3D) -> float:
	if Planet.cfg == null or world_pos.length_sq() <= 1.0:
		return 0.0
	var d := world_pos.normalized().to_v3()
	return world_pos.length() - Planet.cfg.planet_radius - height(d)

static func surface_world(direction: Vector3, clearance_m: float = 0.0) -> Vec3D:
	var d := direction.normalized()
	var r := Planet.cfg.planet_radius + height(d) + clearance_m
	return Vec3D.new(d.x * r, d.y * r, d.z * r)
