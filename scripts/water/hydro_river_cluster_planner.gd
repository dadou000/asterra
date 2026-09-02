class_name HydroRiverClusterPlanner
extends RefCounted
## Plans a short cardinally contiguous sparse-tile chain along one coarse 1D reach.
##
## Besides tile adjacency, the planner propagates the exact corridor crossing point
## through every shared edge. `edge_orientation` handles reversed cube-face seams,
## so the water strip itself stays continuous across members instead of each tile
## independently recentering the river.

const EDGE_EPS := 1.0e-8


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
	var anchor := _cell_position_for_direction(key, source_dir, atlas.tile_resolution)
	var upstream_edge := -1
	for index in desired:
		if key == null or seen.has(key.packed()):
			break
		seen[key.packed()] = true
		var geometry := _member_geometry(key, receiver_dir, speed, half_width,
			atlas, anchor)
		if geometry.is_empty():
			break
		if upstream_edge >= 0:
			_orient_geometry_inward(geometry, upstream_edge)
		geometry["index"] = index
		geometry["key"] = key
		geometry["tile_id"] = key.packed()
		if upstream_edge >= 0:
			geometry["upstream_direction"] = upstream_edge
		members.append(geometry)
		if index + 1 >= desired:
			break

		var local_direction := geometry["direction_cell"] as Vector2
		var direction := _dominant_cardinal(local_direction)
		# Never immediately leave through the same edge we entered. If curvature or
		# numerical projection makes that component dominant, choose the strongest
		# remaining downstream component instead.
		if direction == upstream_edge:
			direction = _alternate_downstream_cardinal(local_direction, upstream_edge)
		var parameter := _edge_crossing_parameter(
			geometry["center_cell"] as Vector2, local_direction, direction,
			atlas.tile_resolution)
		if not is_finite(parameter):
			break
		var link := HydroTileTopology.neighbor(key, direction)
		if link.is_empty():
			break
		var next := link.get("key") as HydroTileKey
		if next == null:
			break
		var orientation := int(link.get("edge_orientation", 1))
		var destination_parameter := parameter if orientation >= 0 else 1.0 - parameter
		var destination_edge := int(link.get("destination_direction", -1))
		if destination_edge < 0:
			break
		var next_anchor := _point_on_edge(destination_edge, destination_parameter,
			atlas.tile_resolution)

		var current := members[index]
		current["downstream_link"] = link
		current["downstream_edge_parameter"] = parameter
		members[index] = current
		key = next
		anchor = next_anchor
		upstream_edge = destination_edge

	if members.is_empty():
		return {"error": ERR_CANT_RESOLVE, "reason": "empty_cluster"}
	# Backfill reciprocal edge metadata now that the member sequence is complete.
	for i in range(1, members.size()):
		var previous := members[i - 1]
		var current := members[i]
		var link := previous.get("downstream_link", {}) as Dictionary
		if link.is_empty():
			return {"error": ERR_CANT_RESOLVE, "reason": "missing_internal_link", "index": i}
		var source_parameter := float(previous.get("downstream_edge_parameter", 0.5))
		var orientation := int(link.get("edge_orientation", 1))
		current["upstream_link"] = {
			"source_direction": int(link.get("destination_direction", -1)),
			"destination_direction": int(link.get("source_direction", -1)),
			"edge_orientation": orientation,
			"crossed_face": bool(link.get("crossed_face", false)),
		}
		current["upstream_edge_parameter"] = source_parameter \
			if orientation >= 0 else 1.0 - source_parameter
		members[i] = current

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
		"corridor_edge_continuity": true,
	}


static func _member_geometry(key: HydroTileKey, receiver_dir: Vector3,
		speed: float, half_width_m: float, atlas: SparseHydroAtlasGPU,
		center_cell: Vector2) -> Dictionary:
	var center_dir := _direction_for_cell_position(key, center_cell,
		atlas.tile_resolution)
	var tangent := receiver_dir - center_dir * receiver_dir.dot(center_dir)
	if tangent.length_squared() <= 1.0e-12:
		return {}
	tangent = tangent.normalized()
	var local_direction := _world_vector_to_face_uv(center_dir, tangent)
	if local_direction.length_squared() <= 1.0e-12:
		return {}
	local_direction = local_direction.normalized()
	return {
		"center_cell": center_cell,
		"direction_cell": local_direction,
		"local_velocity": local_direction * maxf(speed, 0.0),
		"half_width_m": half_width_m,
		"center_direction": center_dir,
	}


static func _orient_geometry_inward(geometry: Dictionary, entry_edge: int) -> void:
	var direction := geometry.get("direction_cell", Vector2.RIGHT) as Vector2
	var inward := _inward_normal(entry_edge)
	if direction.dot(inward) < 0.0:
		direction = -direction
		geometry["direction_cell"] = direction
		var velocity := geometry.get("local_velocity", Vector2.ZERO) as Vector2
		geometry["local_velocity"] = -velocity


static func _inward_normal(edge: int) -> Vector2:
	match edge:
		HydroTileTopology.DIR_WEST: return Vector2.RIGHT
		HydroTileTopology.DIR_EAST: return Vector2.LEFT
		HydroTileTopology.DIR_SOUTH: return Vector2.UP
		HydroTileTopology.DIR_NORTH: return Vector2.DOWN
	return Vector2.ZERO


static func _dominant_cardinal(direction: Vector2) -> int:
	if absf(direction.x) >= absf(direction.y):
		return HydroTileTopology.DIR_EAST if direction.x >= 0.0 \
			else HydroTileTopology.DIR_WEST
	return HydroTileTopology.DIR_NORTH if direction.y >= 0.0 \
		else HydroTileTopology.DIR_SOUTH


static func _alternate_downstream_cardinal(direction: Vector2, forbidden: int) -> int:
	var choices := [
		{"dir": HydroTileTopology.DIR_EAST, "score": direction.x},
		{"dir": HydroTileTopology.DIR_WEST, "score": -direction.x},
		{"dir": HydroTileTopology.DIR_NORTH, "score": direction.y},
		{"dir": HydroTileTopology.DIR_SOUTH, "score": -direction.y},
	]
	choices.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["score"]) > float(b["score"]))
	for choice in choices:
		if int(choice["dir"]) != forbidden and float(choice["score"]) > EDGE_EPS:
			return int(choice["dir"])
	return _dominant_cardinal(direction)


static func _edge_crossing_parameter(center: Vector2, direction: Vector2,
		edge: int, resolution: int) -> float:
	var r := float(resolution)
	var t := NAN
	match edge:
		HydroTileTopology.DIR_WEST:
			if absf(direction.x) > EDGE_EPS:
				t = (0.0 - center.x) / direction.x
		HydroTileTopology.DIR_EAST:
			if absf(direction.x) > EDGE_EPS:
				t = (r - center.x) / direction.x
		HydroTileTopology.DIR_SOUTH:
			if absf(direction.y) > EDGE_EPS:
				t = (0.0 - center.y) / direction.y
		HydroTileTopology.DIR_NORTH:
			if absf(direction.y) > EDGE_EPS:
				t = (r - center.y) / direction.y
	if not is_finite(t) or t < -EDGE_EPS:
		return NAN
	var point := center + direction * t
	if edge in [HydroTileTopology.DIR_WEST, HydroTileTopology.DIR_EAST]:
		return clampf(point.y / r, 0.0, 1.0)
	return clampf(point.x / r, 0.0, 1.0)


static func _point_on_edge(edge: int, parameter: float, resolution: int) -> Vector2:
	var r := float(resolution)
	var p := clampf(parameter, 0.0, 1.0) * r
	match edge:
		HydroTileTopology.DIR_WEST: return Vector2(0.0, p)
		HydroTileTopology.DIR_EAST: return Vector2(r, p)
		HydroTileTopology.DIR_SOUTH: return Vector2(p, 0.0)
		HydroTileTopology.DIR_NORTH: return Vector2(p, r)
	return Vector2.ONE * (0.5 * r)


static func _cell_position_for_direction(key: HydroTileKey, direction: Vector3,
		resolution: int) -> Vector2:
	var mapped := CubeSphere.dir_to_face_uv(direction.normalized())
	if mapped.size() < 3 or int(mapped[0]) != key.face:
		return Vector2.ONE * (0.5 * float(resolution))
	var side := float(1 << key.level)
	var gx := (float(mapped[1]) + 1.0) * 0.5 * side
	var gy := (float(mapped[2]) + 1.0) * 0.5 * side
	return Vector2((gx - float(key.x)) * float(resolution),
		(gy - float(key.y)) * float(resolution))


static func _direction_for_cell_position(key: HydroTileKey, center_cell: Vector2,
		resolution: int) -> Vector3:
	var r := maxf(float(resolution), 1.0)
	var side := float(1 << key.level)
	var fx := (float(key.x) + clampf(center_cell.x / r, 0.0, 1.0)) / side
	var fy := (float(key.y) + clampf(center_cell.y / r, 0.0, 1.0)) / side
	return CubeSphere.face_uv_to_dir(key.face, fx * 2.0 - 1.0, fy * 2.0 - 1.0).normalized()


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
