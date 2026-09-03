class_name HydroLODHierarchy
extends RefCounted
## CPU authority for the physical 2:1 HydroLOD quadtree.
##
## Quadtree level increases toward finer spatial tiles. WaterSystem's configured
## base_tile_level is H0; Hn therefore maps to key.level = H0 - n. Parent and any
## descendant must never be simultaneously solver-visible outside a paused atomic
## transition.


static func physical_lod(base_tile_level: int, key: HydroTileKey) -> int:
	if key == null or base_tile_level < 0:
		return -1
	return base_tile_level - key.level


static func valid_physical_key(base_tile_level: int, maximum_lod: int,
		key: HydroTileKey) -> bool:
	if key == null or base_tile_level < 0:
		return false
	var lod := physical_lod(base_tile_level, key)
	return lod >= 0 and lod <= maxi(maximum_lod, 0)


static func is_ancestor(ancestor: HydroTileKey, descendant: HydroTileKey) -> bool:
	if ancestor == null or descendant == null or ancestor.face != descendant.face \
			or ancestor.level >= descendant.level:
		return false
	var shift := descendant.level - ancestor.level
	return (descendant.x >> shift) == ancestor.x \
		and (descendant.y >> shift) == ancestor.y


static func overlaps(a: HydroTileKey, b: HydroTileKey) -> bool:
	if a == null or b == null or a.face != b.face:
		return false
	return a.equals(b) or is_ancestor(a, b) or is_ancestor(b, a)


static func children(parent: HydroTileKey) -> Array[HydroTileKey]:
	var out: Array[HydroTileKey] = []
	if parent == null or parent.level >= HydroTileKey.MAX_LEVEL:
		return out
	for q in 4:
		out.append(parent.child(q))
	return out


static func child_ids(parent: HydroTileKey) -> PackedInt64Array:
	var out := PackedInt64Array()
	for child in children(parent):
		out.append(child.packed())
	return out


static func sibling_parent(key: HydroTileKey) -> HydroTileKey:
	return null if key == null or key.level <= 0 else key.parent()


static func immediate_siblings(key: HydroTileKey) -> Array[HydroTileKey]:
	var parent := sibling_parent(key)
	return [] if parent == null else children(parent)


static func covering_record(pool: HydroTilePool, key: HydroTileKey) -> Dictionary:
	if pool == null or key == null:
		return {}
	var current := key
	while current != null:
		if pool.contains(current):
			return pool.record(current)
		if current.level <= 0:
			break
		current = current.parent()
	return {}


static func descendant_records(pool: HydroTilePool, key: HydroTileKey) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if pool == null or key == null:
		return out
	for record in pool.active_records():
		var candidate := record.get("key") as HydroTileKey
		if candidate != null and is_ancestor(key, candidate):
			out.append(record)
	return out


static func representation_conflict(pool: HydroTilePool, key: HydroTileKey,
		ignored_ids: PackedInt64Array = PackedInt64Array()) -> Dictionary:
	if pool == null or key == null:
		return {"conflict": true, "reason": "invalid_hierarchy_request"}
	var ignored: Dictionary = {}
	for id in ignored_ids:
		ignored[int(id)] = true
	for record in pool.active_records():
		var candidate := record.get("key") as HydroTileKey
		if candidate == null or ignored.has(candidate.packed()):
			continue
		if overlaps(key, candidate):
			return {
				"conflict": true,
				"reason": "overlapping_hydrolod_owner",
				"requested_tile_id": key.packed(),
				"resident_tile_id": candidate.packed(),
			}
	return {"conflict": false}


static func immediate_children_resident(pool: HydroTilePool,
		parent: HydroTileKey, require_active: bool = true) -> Dictionary:
	if pool == null or parent == null:
		return {"ready": false, "reason": "invalid_parent"}
	var child_keys := children(parent)
	if child_keys.size() != 4:
		return {"ready": false, "reason": "parent_has_no_children"}
	var slots := PackedInt32Array()
	for child in child_keys:
		if not pool.contains(child):
			return {"ready": false, "reason": "child_not_resident",
				"tile_id": child.packed()}
		var record := pool.record(child)
		if require_active and int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			return {"ready": false, "reason": "child_not_published",
				"tile_id": child.packed()}
		slots.append(int(record.get("slot", -1)))
	return {
		"ready": true,
		"parent": parent,
		"children": child_keys,
		"child_slots": slots,
	}
