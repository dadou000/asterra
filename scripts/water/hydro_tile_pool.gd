class_name HydroTilePool
extends RefCounted
## Bounded ownership layer between persistent tile IDs and transient GPU slots.
##
## The current implementation manages slot identity/state only; Phase 3 GPU atlas
## buffers attach to these slots next. Releasing a tile never changes its stable
## HydroTileKey ID.

enum TileState {
	SLEEPING_DRY,
	ALLOCATING,
	ACTIVE,
	SETTLING,
	FROZEN_WATER,
}

var capacity: int
var _free_slots: Array[int] = []
var _slot_to_id: PackedInt64Array
var _records: Dictionary = {} # packed tile id -> Dictionary


func _init(p_capacity: int = 1024) -> void:
	capacity = maxi(p_capacity, 1)
	_slot_to_id = PackedInt64Array()
	_slot_to_id.resize(capacity)
	for i in capacity:
		_slot_to_id[i] = -1
	# Pop from the back so the first allocation receives slot zero.
	for i in range(capacity - 1, -1, -1):
		_free_slots.append(i)


func allocate(key: HydroTileKey, physical_lod: int = 0) -> int:
	if key == null:
		return -1
	var id := key.packed()
	var existing: Variant = _records.get(id, null)
	if existing is Dictionary:
		return int((existing as Dictionary).get("slot", -1))
	if _free_slots.is_empty():
		return -1
	var slot := _free_slots.pop_back()
	_slot_to_id[slot] = id
	_records[id] = {
		"key": key,
		"slot": slot,
		"state": TileState.ALLOCATING,
		"physical_lod": maxi(physical_lod, 0),
		"max_depth_m": 0.0,
		"max_velocity_mps": 0.0,
		"max_outgoing_flux_m3s": 0.0,
		"disturbance_energy": 0.0,
		"quiet_time_s": 0.0,
		"last_reason": "allocate",
	}
	return slot


func release(key_or_id: Variant) -> bool:
	var id := _id_of(key_or_id)
	if id < 0 or not _records.has(id):
		return false
	var record: Dictionary = _records[id]
	var slot := int(record.get("slot", -1))
	_records.erase(id)
	if slot >= 0 and slot < capacity:
		_slot_to_id[slot] = -1
		_free_slots.append(slot)
	return true


func contains(key_or_id: Variant) -> bool:
	var id := _id_of(key_or_id)
	return id >= 0 and _records.has(id)


func slot_for(key_or_id: Variant) -> int:
	var id := _id_of(key_or_id)
	if id < 0:
		return -1
	var record: Variant = _records.get(id, null)
	return int((record as Dictionary).get("slot", -1)) if record is Dictionary else -1


func id_for_slot(slot: int) -> int:
	return _slot_to_id[slot] if slot >= 0 and slot < capacity else -1


func key_for_slot(slot: int) -> HydroTileKey:
	var id := id_for_slot(slot)
	return null if id < 0 else HydroTileKey.unpack(id)


func record(key_or_id: Variant) -> Dictionary:
	var id := _id_of(key_or_id)
	var value: Variant = _records.get(id, null)
	return (value as Dictionary).duplicate(true) if value is Dictionary else {}


func set_state(key_or_id: Variant, state: TileState, reason: String = "") -> bool:
	var id := _id_of(key_or_id)
	if not _records.has(id):
		return false
	var r: Dictionary = _records[id]
	r["state"] = int(state)
	if not reason.is_empty():
		r["last_reason"] = reason
	_records[id] = r
	return true


func update_activity(key_or_id: Variant, max_depth_m: float,
		max_velocity_mps: float, max_outgoing_flux_m3s: float,
		disturbance_energy: float, quiet_dt_s: float = 0.0) -> bool:
	var id := _id_of(key_or_id)
	if not _records.has(id):
		return false
	var r: Dictionary = _records[id]
	r["max_depth_m"] = maxf(max_depth_m, 0.0)
	r["max_velocity_mps"] = maxf(max_velocity_mps, 0.0)
	r["max_outgoing_flux_m3s"] = maxf(max_outgoing_flux_m3s, 0.0)
	r["disturbance_energy"] = maxf(disturbance_energy, 0.0)
	r["quiet_time_s"] = maxf(float(r.get("quiet_time_s", 0.0)) + quiet_dt_s, 0.0)
	_records[id] = r
	return true


func reset_quiet_time(key_or_id: Variant) -> bool:
	var id := _id_of(key_or_id)
	if not _records.has(id):
		return false
	var r: Dictionary = _records[id]
	r["quiet_time_s"] = 0.0
	_records[id] = r
	return true


func active_records() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for value: Variant in _records.values():
		if value is Dictionary:
			out.append((value as Dictionary).duplicate(true))
	return out


func allocated_count() -> int:
	return _records.size()


func free_count() -> int:
	return _free_slots.size()


func full() -> bool:
	return _free_slots.is_empty()


func clear() -> void:
	_records.clear()
	_free_slots.clear()
	for i in capacity:
		_slot_to_id[i] = -1
	for i in range(capacity - 1, -1, -1):
		_free_slots.append(i)


func stats() -> Dictionary:
	var counts := {
		"sleeping_dry": 0,
		"allocating": 0,
		"active": 0,
		"settling": 0,
		"frozen_water": 0,
	}
	for value: Variant in _records.values():
		if not (value is Dictionary):
			continue
		match int((value as Dictionary).get("state", TileState.ALLOCATING)):
			TileState.SLEEPING_DRY: counts["sleeping_dry"] += 1
			TileState.ALLOCATING: counts["allocating"] += 1
			TileState.ACTIVE: counts["active"] += 1
			TileState.SETTLING: counts["settling"] += 1
			TileState.FROZEN_WATER: counts["frozen_water"] += 1
	return {
		"capacity": capacity,
		"allocated": allocated_count(),
		"free": free_count(),
		"states": counts,
	}


func _id_of(value: Variant) -> int:
	if value is HydroTileKey:
		return (value as HydroTileKey).packed()
	if value is int:
		return int(value)
	return -1
