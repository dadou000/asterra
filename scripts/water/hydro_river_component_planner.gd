class_name HydroRiverComponentPlanner
extends RefCounted
## Plans a unique sparse-tile graph for a complete coarse river component.
##
## Each coarse reach owns a non-overlapping tile chain. An internal upstream chain
## terminates immediately beside the receiver reach's first tile. Confluence nodes
## are owned by the downstream reach and contain a branched junction description.

const MAX_PATH_VISITS := 8192
const EDGE_EPS := 1.0e-8


static func plan(store: PlanetHydrologyRiverClusterStore,
		atlas: SparseHydroAtlasGPU, requested_cells: PackedInt32Array) -> Dictionary:
	if store == null or not store.initialized or store.river_reaches == null \
			or atlas == null or not atlas.initialized_ok() or requested_cells.size() < 2:
		return {"error": ERR_INVALID_PARAMETER, "reason": "invalid_component_plan_request"}
	var cells: Array[int] = []
	var membership: Dictionary = {}
	for raw_cell in requested_cells:
		var cell := int(raw_cell)
		if cell < 0 or cell >= store.cell_count() or membership.has(cell) \
				or not store.river_reaches.is_reach_cell(cell) or store.is_refined_reach(cell):
			return {"error": ERR_INVALID_PARAMETER, "reason": "invalid_component_reach", "cell": cell}
		membership[cell] = true
		cells.append(cell)
	cells.sort()

	var topology := _topology(store, cells)
	if int(topology.get("error", FAILED)) != OK:
		return topology
	var ordered := topology.get("cells", PackedInt32Array()) as PackedInt32Array
	var indegree := topology.get("indegree", {}) as Dictionary
	var parents := topology.get("parents", {}) as Dictionary
	var contract := HydroMetricGrid.atlas_contract(atlas, store.grid.radius)
	var level := int(contract.get("level", -1))
	if level < 0:
		return {"error": ERR_INVALID_DATA, "reason": "metric_contract"}

	var node_keys: Dictionary = {}
	var node_ids: Dictionary = {}
	for raw_cell in ordered:
		var cell := int(raw_cell)
		var key := _key_for_direction(store.grid.cell_dir(cell), level)
		if key == null or node_ids.has(key.packed()):
			return {"error": ERR_CANT_RESOLVE, "reason": "component_node_tile_collision", "cell": cell}
		node_keys[cell] = key
		node_ids[key.packed()] = cell

	var hydraulic: Dictionary = {}
	for raw_cell in ordered:
		var cell := int(raw_cell)
		hydraulic[cell] = _hydraulic_geometry(store, atlas, cell)

	# Reserve downstream chains first. Upstream A* searches may terminate at the
	# downstream node but cannot accidentally cut through an already-owned stem.
	var routes: Dictionary = {}
	var reserved: Dictionary = {}
	var reverse_cells := Array(ordered)
	reverse_cells.reverse()
	for raw_value: Variant in reverse_cells:
		var cell := int(raw_value)
		var source := node_keys[cell] as HydroTileKey
		var receiver := int(store.receiver[cell])
		var target: HydroTileKey
		if membership.has(receiver):
			target = node_keys[receiver] as HydroTileKey
		else:
			target = _key_for_direction(store.grid.cell_dir(receiver), level)
		if target == null:
			return {"error": ERR_CANT_RESOLVE, "reason": "component_target_tile_missing", "cell": cell}
		var blocked := reserved.duplicate()
		for id_value: Variant in node_ids.keys():
			var id := int(id_value)
			if id != source.packed() and id != target.packed():
				blocked[id] = true
		var inclusive := _shortest_cardinal_path(source, target, blocked)
		if inclusive.is_empty():
			return {"error": ERR_CANT_RESOLVE, "reason": "component_sparse_path_unresolved", "cell": cell}
		var member_keys: Array[HydroTileKey] = []
		if inclusive.size() == 1:
			member_keys.append(source)
		else:
			for i in inclusive.size() - 1:
				member_keys.append(inclusive[i])
		if member_keys.is_empty():
			return {"error": ERR_CANT_RESOLVE, "reason": "component_empty_reach_path", "cell": cell}
		for key in member_keys:
			if reserved.has(key.packed()):
				return {"error": ERR_CANT_RESOLVE, "reason": "component_sparse_path_overlap", "cell": cell}
			reserved[key.packed()] = cell
		routes[cell] = {
			"keys": member_keys,
			"target_key": target,
			"receiver": receiver,
		}

	var reach_plans: Array[Dictionary] = []
	var reach_plan_by_cell: Dictionary = {}
	var junctions: Array[Dictionary] = []
	var total_members := 0
	for raw_cell in ordered:
		var cell := int(raw_cell)
		var route := routes[cell] as Dictionary
		var keys := route["keys"] as Array[HydroTileKey]
		var target := route["target_key"] as HydroTileKey
		var members: Array[Dictionary] = []
		for i in keys.size():
			var key := keys[i]
			var incoming_links: Array[Dictionary] = []
			if i > 0:
				var link := _link_between(keys[i - 1], key)
				if link.is_empty():
					return {"error": ERR_CANT_RESOLVE, "reason": "component_internal_route_gap", "cell": cell}
				incoming_links.append(_reverse_link(link))
			elif int(indegree.get(cell, 0)) > 0:
				var parent_cells := parents.get(cell, []) as Array
				for parent_value: Variant in parent_cells:
					var parent := int(parent_value)
					var parent_route := routes[parent] as Dictionary
					var parent_keys := parent_route["keys"] as Array[HydroTileKey]
					var parent_last := parent_keys[parent_keys.size() - 1]
					var parent_link := _link_between(parent_last, key)
					if parent_link.is_empty():
						return {"error": ERR_CANT_RESOLVE, "reason": "component_parent_route_gap", "cell": cell}
					var reversed := _reverse_link(parent_link)
					reversed["from_cell"] = parent
					incoming_links.append(reversed)

			var next_key := keys[i + 1] if i + 1 < keys.size() else target
			var outgoing_link := _link_between(key, next_key)
			if outgoing_link.is_empty():
				return {"error": ERR_CANT_RESOLVE, "reason": "component_outgoing_route_gap", "cell": cell}
			var geometry := _member_geometry(cell, key, incoming_links, outgoing_link,
				hydraulic, atlas)
			if geometry.is_empty():
				return {"error": ERR_CANT_RESOLVE, "reason": "component_member_geometry", "cell": cell}
			geometry["index"] = i
			geometry["key"] = key
			geometry["tile_id"] = key.packed()
			geometry["downstream_link"] = outgoing_link
			if not incoming_links.is_empty():
				geometry["upstream_links"] = incoming_links
			members.append(geometry)

			if bool(geometry.get("junction", false)):
				var parent_cells := PackedInt32Array()
				for incoming in incoming_links:
					parent_cells.append(int(incoming.get("from_cell", -1)))
				junctions.append({
					"cell": cell,
					"member_index": i,
					"tile_id": key.packed(),
					"incoming_cells": parent_cells,
					"segment_count": (geometry.get("junction_segments", []) as Array).size(),
					"verified": true,
				})

		var plan := {
			"cell": cell,
			"receiver": int(store.receiver[cell]),
			"members": members,
			"member_count": members.size(),
			"available_channel_volume_m3": store.available_channel_promotion_volume_m3(cell),
			"fully_promote": true,
			"contains_junction": int(indegree.get(cell, 0)) > 1,
		}
		reach_plans.append(plan)
		reach_plan_by_cell[cell] = plan
		total_members += members.size()

	return {
		"error": OK,
		"cells": ordered,
		"reach_plans": reach_plans,
		"reach_plan_by_cell": reach_plan_by_cell,
		"member_count": total_members,
		"junctions": junctions,
		"junction_count": junctions.size(),
		"upstream_mouth_cells": topology.get("upstream_mouth_cells", PackedInt32Array()),
		"upstream_mouth_count": int(topology.get("upstream_mouth_count", 0)),
		"downstream_outlet_cell": int(topology.get("downstream_outlet_cell", -1)),
		"fine_junction_verified": true,
		"physical_topology_verified": true,
		"planning": "cardinal_nonoverlapping_component_graph",
	}


static func _topology(store: PlanetHydrologyRiverClusterStore,
		cells: Array[int]) -> Dictionary:
	var membership: Dictionary = {}
	var indegree: Dictionary = {}
	var parents: Dictionary = {}
	for cell in cells:
		membership[cell] = true
		indegree[cell] = 0
		parents[cell] = []
	var outlets: Array[int] = []
	for cell in cells:
		var receiver := int(store.receiver[cell])
		if membership.has(receiver):
			indegree[receiver] = int(indegree[receiver]) + 1
			var p := parents[receiver] as Array
			p.append(cell)
			parents[receiver] = p
		else:
			outlets.append(cell)
	if outlets.size() != 1:
		return {"error": ERR_CANT_RESOLVE, "reason": "component_requires_single_downstream_outlet"}
	var roots: Array[int] = []
	for cell in cells:
		if int(indegree[cell]) == 0:
			roots.append(cell)
	roots.sort()
	if roots.is_empty():
		return {"error": ERR_CANT_RESOLVE, "reason": "component_has_no_upstream_root"}
	var work := indegree.duplicate()
	var queue: Array[int] = roots.duplicate()
	var ordered: Array[int] = []
	while not queue.is_empty():
		var cell := queue.pop_front()
		ordered.append(cell)
		var receiver := int(store.receiver[cell])
		if membership.has(receiver):
			work[receiver] = int(work[receiver]) - 1
			if int(work[receiver]) == 0:
				queue.append(receiver)
				queue.sort()
	if ordered.size() != cells.size():
		return {"error": ERR_CANT_RESOLVE, "reason": "component_receiver_cycle"}
	return {
		"error": OK,
		"cells": PackedInt32Array(ordered),
		"indegree": indegree,
		"parents": parents,
		"upstream_mouth_cells": PackedInt32Array(roots),
		"upstream_mouth_count": roots.size(),
		"downstream_outlet_cell": outlets[0],
	}


static func _hydraulic_geometry(store: PlanetHydrologyRiverClusterStore,
		atlas: SparseHydroAtlasGPU, cell: int) -> Dictionary:
	var reach := store.river_reaches.reach_state(cell, store.channel_storage_m3[cell])
	var depth := maxf(float(reach.get("depth_m", 0.0)), 0.0)
	var area := store.river_reaches.cross_section_area(cell, depth)
	var q := store.river_reaches.discharge_for_depth(cell, depth)
	return {
		"speed_mps": q / maxf(area, 1.0e-9),
		"half_width_m": maxf(float(store.fields.river_width[cell]) * 0.5,
			atlas.cell_size_m * 0.75),
	}


static func _member_geometry(cell: int, key: HydroTileKey,
		incoming_links: Array[Dictionary], outgoing_link: Dictionary,
		hydraulic: Dictionary, atlas: SparseHydroAtlasGPU) -> Dictionary:
	var resolution := atlas.tile_resolution
	var center := Vector2.ONE * (0.5 * float(resolution))
	var out_edge := int(outgoing_link.get("source_direction", -1))
	if out_edge < 0:
		return {}
	var exit := _point_on_edge(out_edge, resolution)
	var local := hydraulic[cell] as Dictionary
	var speed := maxf(float(local.get("speed_mps", 0.0)), 0.0)
	var half_width := maxf(float(local.get("half_width_m", 0.0)), atlas.cell_size_m * 0.75)

	if incoming_links.size() > 1:
		var segments: Array[Dictionary] = []
		for incoming in incoming_links:
			var edge := int(incoming.get("source_direction", -1))
			var from_cell := int(incoming.get("from_cell", -1))
			if edge < 0 or from_cell < 0 or not hydraulic.has(from_cell):
				return {}
			var start := _point_on_edge(edge, resolution)
			var source_h := hydraulic[from_cell] as Dictionary
			var direction := (center - start).normalized()
			segments.append({
				"start_cell": start,
				"end_cell": center,
				"half_width_m": maxf(float(source_h.get("half_width_m", 0.0)),
					atlas.cell_size_m * 0.75),
				"local_velocity": direction * maxf(float(source_h.get("speed_mps", 0.0)), 0.0),
			})
		var out_direction := (exit - center).normalized()
		segments.append({
			"start_cell": center,
			"end_cell": exit,
			"half_width_m": half_width,
			"local_velocity": out_direction * speed,
		})
		return {
			"center_cell": (center + exit) * 0.5,
			"direction_cell": out_direction,
			"local_velocity": out_direction * speed,
			"half_width_m": half_width,
			"junction": true,
			"junction_segments": segments,
		}

	var start := center
	if incoming_links.size() == 1:
		var edge := int(incoming_links[0].get("source_direction", -1))
		if edge < 0:
			return {}
		start = _point_on_edge(edge, resolution)
	var direction := exit - start
	if direction.length_squared() <= EDGE_EPS:
		return {}
	direction = direction.normalized()
	return {
		"center_cell": (start + exit) * 0.5,
		"direction_cell": direction,
		"local_velocity": direction * speed,
		"half_width_m": half_width,
		"junction": false,
	}


static func _point_on_edge(edge: int, resolution: int) -> Vector2:
	var r := float(resolution)
	var p := 0.5 * r
	match edge:
		HydroTileTopology.DIR_WEST: return Vector2(0.0, p)
		HydroTileTopology.DIR_EAST: return Vector2(r, p)
		HydroTileTopology.DIR_SOUTH: return Vector2(p, 0.0)
		HydroTileTopology.DIR_NORTH: return Vector2(p, r)
	return Vector2.ONE * p


static func _reverse_link(link: Dictionary) -> Dictionary:
	return {
		"key": null,
		"source_direction": int(link.get("destination_direction", -1)),
		"destination_direction": int(link.get("source_direction", -1)),
		"edge_orientation": int(link.get("edge_orientation", 1)),
		"crossed_face": bool(link.get("crossed_face", false)),
	}


static func _link_between(source: HydroTileKey, destination: HydroTileKey) -> Dictionary:
	if source == null or destination == null:
		return {}
	for direction in 4:
		var link := HydroTileTopology.neighbor(source, direction)
		if link.is_empty():
			continue
		var key := link.get("key") as HydroTileKey
		if key != null and key.equals(destination):
			return link
	return {}


static func _shortest_cardinal_path(start: HydroTileKey, goal: HydroTileKey,
		blocked: Dictionary) -> Array[HydroTileKey]:
	if start == null or goal == null or start.level != goal.level:
		return []
	if start.equals(goal):
		return [start]
	var open: Array[Dictionary] = [{
		"key": start, "g": 0.0, "f": _heuristic(start, goal),
	}]
	var best_g: Dictionary = {start.packed(): 0.0}
	var came: Dictionary = {}
	var key_by_id: Dictionary = {start.packed(): start}
	var visited := 0
	while not open.is_empty() and visited < MAX_PATH_VISITS:
		open.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
			return float(a["f"]) < float(b["f"]))
		var current_rec := open.pop_front()
		var current := current_rec["key"] as HydroTileKey
		var current_id := current.packed()
		var current_g := float(current_rec["g"])
		if current_g > float(best_g.get(current_id, INF)) + 1.0e-9:
			continue
		visited += 1
		if current.equals(goal):
			return _reconstruct_path(current_id, came, key_by_id)
		for direction in 4:
			var link := HydroTileTopology.neighbor(current, direction)
			if link.is_empty():
				continue
			var next := link.get("key") as HydroTileKey
			if next == null:
				continue
			var next_id := next.packed()
			if blocked.has(next_id) and not next.equals(goal):
				continue
			var tentative := current_g + 1.0
			if tentative + 1.0e-9 >= float(best_g.get(next_id, INF)):
				continue
			best_g[next_id] = tentative
			came[next_id] = current_id
			key_by_id[next_id] = next
			open.append({"key": next, "g": tentative,
				"f": tentative + _heuristic(next, goal)})
	return []


static func _reconstruct_path(goal_id: int, came: Dictionary,
		key_by_id: Dictionary) -> Array[HydroTileKey]:
	var reverse: Array[HydroTileKey] = []
	var current := goal_id
	while true:
		var key := key_by_id.get(current) as HydroTileKey
		if key == null:
			return []
		reverse.append(key)
		if not came.has(current):
			break
		current = int(came[current])
	reverse.reverse()
	return reverse


static func _heuristic(a: HydroTileKey, b: HydroTileKey) -> float:
	var auv := HydroTileTopology.tile_center_face_uv(a)
	var buv := HydroTileTopology.tile_center_face_uv(b)
	var ad := CubeSphere.face_uv_to_dir(a.face, auv.x, auv.y).normalized()
	var bd := CubeSphere.face_uv_to_dir(b.face, buv.x, buv.y).normalized()
	var angle := acos(clampf(ad.dot(bd), -1.0, 1.0))
	return angle * float(1 << a.level) * 0.5


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
