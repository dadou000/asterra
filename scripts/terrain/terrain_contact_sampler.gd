class_name TerrainContactSampler
extends RefCounted
## Shared gameplay contact/altitude sampler for the GPU-first terrain runtime.
##
## Precise pristine terrain comes from TerrainHeightQuery, persistent edits come
## from Deltas, and the live high-resolution deformation tile is sampled from the
## asynchronous TerrainDeformationGPU CPU mirror. No synchronous GPU readback is
## ever performed by a physics contact.


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
	var d: Vector3 = direction.normalized()
	if not snap.is_empty():
		return Planet.terrain_height(d, null, snap)
	TerrainHeightQuery.request_height(d)
	var base_height: float = TerrainHeightQuery.height_for_direction(d, Planet.terrain_height(d))
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed:
		base_height += TerrainDeformationGPU.active_height_offset(d)
	return base_height


static func surface(direction: Vector3) -> Dictionary:
	if not Planet.ready_state or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return {"height": 0.0, "normal": direction.normalized(), "precise": false}
	var d: Vector3 = direction.normalized()
	var result: Dictionary = TerrainHeightQuery.surface_for_direction(d, Planet.terrain_height(d))
	if not TerrainDeformationGPU.ready_state or TerrainDeformationGPU.failed:
		return result
	var active_height: float = TerrainDeformationGPU.active_height_offset(d)
	var base_height: float = float(result.get("height", Planet.terrain_height(d)))
	var base_normal_value: Variant = result.get("normal", d)
	var base_normal: Vector3 = d
	if base_normal_value is Vector3:
		base_normal = (base_normal_value as Vector3).normalized()

	# Add the active tile's local gradient to the already reconstructed persistent
	# normal. A 0.5 m baseline is large enough to be stable at 0.25 m texel spacing.
	var normal_step_m := 0.5
	var basis: Array = CubeSphere.tangent_basis(d)
	var right: Vector3 = (basis[0] as Vector3).normalized()
	var up: Vector3 = (basis[1] as Vector3).normalized()
	var theta: float = normal_step_m / Planet.cfg.planet_radius
	var dx: Vector3 = (d + right * theta).normalized()
	var dy: Vector3 = (d + up * theta).normalized()
	var hx: float = TerrainDeformationGPU.active_height_offset(dx)
	var hy: float = TerrainDeformationGPU.active_height_offset(dy)
	var grad_x: float = (hx - active_height) / normal_step_m
	var grad_y: float = (hy - active_height) / normal_step_m
	var normal: Vector3 = (base_normal - right * grad_x - up * grad_y).normalized()
	if normal.dot(d) < 0.0:
		normal = -normal
	return {
		"height": base_height + active_height,
		"normal": normal,
		"precise": bool(result.get("precise", false)),
	}


static func altitude_msl(world_pos: Vec3D) -> float:
	if Planet.cfg == null or world_pos.length_sq() <= 1.0:
		return 0.0
	return world_pos.length() - Planet.cfg.planet_radius


static func altitude_agl(world_pos: Vec3D) -> float:
	if Planet.cfg == null or world_pos.length_sq() <= 1.0:
		return 0.0
	var d: Vector3 = world_pos.normalized().to_v3()
	return altitude_msl(world_pos) - height(d)


static func surface_world(direction: Vector3, clearance_m: float = 0.0) -> Vec3D:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return Vec3D.new(0.0, 0.0, 0.0)
	var d: Vector3 = direction.normalized()
	var r: float = Planet.cfg.planet_radius + height(d) + clearance_m
	return Vec3D.new(d.x * r, d.y * r, d.z * r)
