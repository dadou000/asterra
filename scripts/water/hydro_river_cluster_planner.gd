class_name HydroRiverClusterPlanner
extends RefCounted
## Plans a short cardinally contiguous sparse-tile chain along one coarse 1D reach.
##
## The planner deliberately follows HydroTileTopology cardinal neighbors so the
## existing sparse connectivity builder opens fine<->fine interfaces automatically,
## including across cube-face seams. Each member stores its own face-local corridor
## direction/velocity; the physical flow direction remains planet-space coherent.


static func plan(store: PlanetHydrologyRiverPromotionStore,
		atlas: SparseHydroAtlasGPU, cell: int, max_tiles: int = 3) -> Dictionary:
	if store == null or not store.initialized or store.river_reaches == null \
			or atlas == null or not atlas.initialized_ok() or cell < 0 \
			or cell >= store.cell_count() or not store.river_reaches.is_reach_cell(cell) \
			or max_tiles <= 0:
		return {"error": ERR_INVALID_PARAMETER}
	var receiver := store.receiver[cell]
	if receiver < 0 or receiver >= store.cell_count() or receiver == cell:
		return {"error": ERR_INVALID_DATA, "reason": "invalid_receiver"}

	var contract := HydroMetricGrid.atlas_contract(atlas, store.grid.radius)
	var level := int(contract.get("level", -1))
	if level < 0:
		return {"error": ERR_INVALID_DATA, "reason": "metric_contract"}
	var source_dir := store.grid.cell_dir(cell).normalized()
	var receiver_dir := store.grid.cell_dir(receiver).normalized()
	var start := _key_for_direction(source_dir, level)
	if start == null:
		return {"error": ERR_INVALID_DATA, "reason": "source_key"}

	var reach := store.river_reaches.reach_state(cell, store.channel_storage_m3[cell])
	var depth := maxf(float(reach.get("depth_m", 0.0)), 0.0)
	var cross_area := store.river_reaches.cross_section_area(cell, depth)
	var q_capacity := store.river_reaches.discharge_for_depth(cell, depth)
	var speed := q_capacity / maxf(cross_area, 1.0e-9)
	var half_width := maxf(float(store.fields.river_width[cell]) * 0.5,
		atlas.cell_size_m * 0.75)
	var tile_span := float(atlas.tile_resolution) * atlas.cell_size_m
	var reach_length := maxf(float(store.river_reaches.reach_length_m[cell]), tile_span)
	var desired := clampi(int(ceil(reach_length / maxf(tile_span, 1.0))), 1, max_tiles)

	var members: Array[Dictionary] = []
	var seen: Dictionary = {}
	var key := start
	for index in desired:
		if key == null or seen.has(key.packed()):
			break
		seen[key.packed()] = true
		var geometry := _member_geometry(key, receiver_dir, speed, half_width, atlas)
		if geometry.is_empty():
			break
		geometry["index"] = index
		geometry["key"] = key
		geometry["tile_id"] = key.packed()
		members.append(geometry)
		if index + 1 >= desired:
			break
		var direction := _dominant_cardinal(geometry["direction_cell"] as Vector2)
		var link := HydroTileTopology.neighbor(key, direction)
		if link.is_empty():
			break
		var next := link.get("key") as HydroTileKey
		if next == null:
			break
		key = next

	if members.is_empty():
		return {"error": ERR_CANT_RESOLVE, "reason": "empty_cluster"}
	# Record exact pairwise topology so tests/runtime can verify every internal edge.
	for i in members.size() - 1:
		var a := members[i]
		var b := members[i + 1]
		var link := _link_between(a["key"] as HydroTileKey, b["key"] as HydroTileKey)
		if link.is_empty():
			return {"error": ERR_CANT_RESOLVE, "reason": "non_adjacent_members", "index": i}
		a["downstream_link"] = link
		b["upstream_link"] = {
			"source_direction": int(link.get("destination_direction", -1)),
			"destination_direction": int(link.get("source_direction", -1)),
			"edge_orientation": int(link.get("edge_orientation", 1)),
			"crossed_face": bool(link.get("crossed_face", false)),
		}
		members[i] = a
		members[i + 1] = b

	return {
		"error": OK,
		"cell": cell,
		"receiver": receiver,
		"level": level,
		"members": members,
		"member_count": members.size(),
		"tile_span_m": tile_span,
		"represented_span_m": float(members.size()) * tile_span,
		"hydraulic_speed_mps": speed,
		"half_width_m": half_width,
	}


static func _member_geometry(key: HydroTileKey, receiver_dir: Vector3,
		speed: float, half_width_m: float, atlas: SparseHydroAtlasGPU) -> Dictionary:
	var uv := HydroTileTopology.tile_center_face_uv(key)
	var center_dir := CubeSphere.face_uv_to_dir(key.face, uv.x, uv.y).normalized()
	var tangent := receiver_dir - center_dir * receiver_dir.dot(center_dir)
	if tangent.length_squared() <= 1.0e-12:
		return {}
	tangent = tangent.normalized()
	var local_direction := _world_vector_to_face_uv(center_dir, tangent)
	if local_direction.length_squared() <= 1.0e-12:
		return {}
	local_direction = local_direction.normalized()
	return {
		"center_cell": Vector2.ONE * (float(atlas.tile_resolution) * 0.5),
		"direction_cell": local_direction,
		"local_velocity": local_direction * maxf(speed, 0.0),
		"half_width_m": half_width_m,
		"center_direction": center_dir,
	}


static func _dominant_cardinal(direction: Vector2) -> int:
	if absf(direction.x) >= absf(direction.y):
		return HydroTileTopology.DIR_EAST if direction.x >= 0.0 \
			else HydroTileTopology.DIR_WEST
	return HydroTileTopology.DIR_NORTH if direction.y >= 0.0 \
		else HydroTileTopology.DIR_SOUTH


static func _link_between(a: HydroTileKey, b: HydroTileKey) -> Dictionary:
	for direction in 4:
		var link := HydroTileTopology.neighbor(a, direction)
		var key := link.get("key") as HydroTileKey if not link.is_empty() else null
		if key != null and key.equals(b):
			return link
	return {}


static func _key_for_direction(direction: Vector3, level: int) -> HydroTileKey:
	var mapped := CubeSphere.dir_to_face_uv(direction.normalized())
	if mapped.size() < 3:
		return null
	var side := 1 << clampi(level, 0, HydroTileKey.MAX_LEVEL)
	var gx := clampf((float(mapped[1]) + 1.0) * 0.5 * float(side),
		0.0, float(side) - 1.0e-9)
	var gy := clampf((float(mapped[2]) + 1.0) * 0.5 * float(side),
		0.0, float(side) - 1.0e-9)
	return HydroTileKey.new(int(mapped[0]), level,
		clampi(int(floor(gx)), 0, side - 1),
		clampi(int(floor(gy)), 0, side - 1))


static func _world_vector_to_face_uv(direction: Vector3, world_vector: Vector3) -> Vector2:
	if world_vector.length_squared() <= 1.0e-20:
		return Vector2.ZERO
	var d := direction.normalized()
	var mapped := CubeSphere.dir_to_face_uv(d)
	if mapped.size() < 3:
		return Vector2.ZERO
	var face := int(mapped[0])
	var u := float(mapped[1])
	var v := float(mapped[2])
	const EPS := 1.0e-4
	var du := (CubeSphere.face_uv_to_dir(face, clampf(u + EPS, -1.0, 1.0), v)
		- CubeSphere.face_uv_to_dir(face, clampf(u - EPS, -1.0, 1.0), v)).normalized()
	var dv := (CubeSphere.face_uv_to_dir(face, u, clampf(v + EPS, -1.0, 1.0))
		- CubeSphere.face_uv_to_dir(face, u, clampf(v - EPS, -1.0, 1.0))).normalized()
	var tangent := world_vector - d * world_vector.dot(d)
	return Vector2(tangent.dot(du), tangent.dot(dv))
