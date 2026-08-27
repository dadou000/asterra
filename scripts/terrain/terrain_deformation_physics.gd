extends Node
## Finite-strength elasto-plastic terrain response.
##
## Contact mechanics stay on the CPU because they are tiny and latency-sensitive.
## The expensive per-cell deformation raster is submitted to TerrainDeformationGPU
## when available. The original sparse Deltas implementation remains as a fallback
## for unsupported GPUs, initialization failures, and contacts outside the active
## 64 m high-resolution tile.

const MATERIAL_TOPSOIL := 0
const MATERIAL_WET_CLAY := 1
const MATERIAL_GRAVEL := 2
const MATERIAL_ROCK := 3

const FOOTPRINT_GENERIC := 0
const FOOTPRINT_SPHERE := 1

const STATE_TILE := 64
const STATE_CHANNELS := 2 # compaction, damage
const MAX_VISUAL_NOTIFY_HZ := 12.0
const MIN_CONTACT_RADIUS_M := 0.18
const MAX_PLASTIC_RATE_MPS := 5.0

var _state_tiles: Dictionary = {}
var _notify_pending := false
var _notify_center := Vector3(1.0, 0.0, 0.0)
var _notify_radius_m := 0.0
var _notify_timer_s := 0.0
var _deformation_events := 0
var _displaced_volume_m3 := 0.0
var _gpu_contact_events := 0
var _cpu_fallback_events := 0


func _ready() -> void:
	process_priority = -3


func _process(dt: float) -> void:
	if not _notify_pending:
		return
	_notify_timer_s += dt
	if _notify_timer_s < 1.0 / MAX_VISUAL_NOTIFY_HZ:
		return
	Deltas.notify_changed(_notify_center, _notify_radius_m)
	_notify_pending = false
	_notify_radius_m = 0.0
	_notify_timer_s = 0.0


func material_name(material_id: int) -> String:
	var material: Dictionary = _material(material_id)
	return String(material["name"])


func material_parameters(material_id: int) -> Dictionary:
	return _material(material_id).duplicate(true)


func apply_contact(center_dir: Vector3, contact_radius_m: float, normal_load_n: float,
		penetration_m: float, normal_speed_mps: float, tangential_speed_mps: float,
		dt: float, cutting: float = 0.0, material_id: int = MATERIAL_TOPSOIL,
		footprint_type: int = FOOTPRINT_GENERIC, shape_radius_m: float = 0.0) -> Dictionary:
	if Planet.cfg == null or center_dir.length_squared() <= 1e-12 or dt <= 0.0:
		return _empty_result()
	var d: Vector3 = center_dir.normalized()
	var persistent_spacing_m: float = Deltas.sample_spacing(Planet.cfg.planet_radius)
	var minimum_radius_m: float = maxf(MIN_CONTACT_RADIUS_M, persistent_spacing_m * 0.72)
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed:
		minimum_radius_m = maxf(MIN_CONTACT_RADIUS_M,
			float(TerrainDeformationGPU.SAMPLE_SPACING_M) * 0.72)
	var radius_m: float = maxf(contact_radius_m, minimum_radius_m)
	var area_m2: float = PI * radius_m * radius_m
	var material: Dictionary = _material(material_id)
	var state: Vector2 = state_at(d)
	var compaction: float = state.x
	var damage: float = state.y
	var base_yield_pa: float = float(material["yield_pa"])
	var hardening: float = float(material["hardening"])
	var yield_pa: float = base_yield_pa * (1.0 + hardening * compaction)
	yield_pa *= clampf(1.0 - damage * 0.42, 0.45, 1.0)
	var stiffness_pa_per_m: float = float(material["stiffness_pa_per_m"])
	var damping_pa_s_per_m: float = float(material["damping_pa_s_per_m"])
	var support_pressure_pa: float = yield_pa
	support_pressure_pa += stiffness_pa_per_m * maxf(penetration_m, 0.0)
	support_pressure_pa += damping_pa_s_per_m * maxf(-normal_speed_mps, 0.0)
	var support_force_n: float = maxf(support_pressure_pa * area_m2, 0.0)
	var demand_pressure_pa: float = maxf(normal_load_n, 0.0) / maxf(area_m2, 1e-4)
	var bearing_ratio: float = demand_pressure_pa / maxf(yield_pa, 1.0)
	var shear_strength_pa: float = float(material["shear_pa"]) * (1.0 + 0.55 * compaction)
	var shear_drive_pa: float = demand_pressure_pa * clampf(cutting, 0.0, 1.0)
	shear_drive_pa *= clampf(absf(tangential_speed_mps) / 0.75, 0.0, 3.0)
	var shear_ratio: float = shear_drive_pa / maxf(shear_strength_pa, 1.0)
	var bearing_over: float = maxf(bearing_ratio - 1.0, 0.0)
	var shear_over: float = maxf(shear_ratio - 1.0, 0.0)
	var plastic_rate_mps: float = float(material["plastic_rate_mps"])
	var cut_rate: float = float(material["cut_rate"])
	var sink_rate_mps: float = plastic_rate_mps * pow(bearing_over, 0.72)
	sink_rate_mps += cut_rate * shear_over * maxf(absf(tangential_speed_mps), 0.25)
	sink_rate_mps = clampf(sink_rate_mps, 0.0, MAX_PLASTIC_RATE_MPS)
	var sink_m: float = sink_rate_mps * dt
	var moved_volume_m3 := 0.0
	var gpu_active := false
	if sink_m > 1e-6:
		var compaction_gain: float = float(material["compaction_gain"])
		var damage_gain: float = float(material["damage_gain"]) * (0.35 + shear_over)
		var rim_fraction: float = float(material["rim_fraction"])
		var max_depth_m: float = float(material["max_depth_m"])
		if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed:
			gpu_active = TerrainDeformationGPU.enqueue_contact(
				d, radius_m, sink_m, compaction_gain, damage_gain,
				rim_fraction, max_depth_m, footprint_type, shape_radius_m,
				penetration_m)
		if gpu_active:
			var mean_weight: float = 0.50 if footprint_type == FOOTPRINT_SPHERE else 1.0 / 3.0
			moved_volume_m3 = sink_m * area_m2 * mean_weight
			_gpu_contact_events += 1
			var lattice: Array = Deltas.dir_to_lattice(d)
			_add_state(int(lattice[0]), int(round(float(lattice[1]))),
				int(round(float(lattice[2]))), sink_m * compaction_gain,
				sink_m * damage_gain)
		else:
			moved_volume_m3 = _apply_patch(d, radius_m, sink_m, compaction_gain,
				damage_gain, rim_fraction, max_depth_m, footprint_type,
				shape_radius_m, penetration_m)
			if moved_volume_m3 > 1e-8:
				_cpu_fallback_events += 1
				_queue_notify(d, radius_m * 2.1)
		if moved_volume_m3 > 1e-8:
			_deformation_events += 1
			_displaced_volume_m3 += moved_volume_m3
	var restitution: float = float(material["restitution"])
	return {
		"support_force_n": support_force_n,
		"support_pressure_pa": support_pressure_pa,
		"demand_pressure_pa": demand_pressure_pa,
		"yield_pressure_pa": yield_pa,
		"bearing_ratio": bearing_ratio,
		"shear_ratio": shear_ratio,
		"sink_rate_mps": sink_rate_mps,
		"deformed_m": sink_m,
		"moved_volume_m3": moved_volume_m3,
		"compaction": compaction,
		"damage": damage,
		"restitution": restitution,
		"material": String(material["name"]),
		"gpu_active": gpu_active,
		"footprint_type": footprint_type,
	}


func state_at(direction: Vector3) -> Vector2:
	if direction.length_squared() <= 1e-12:
		return Vector2.ZERO
	var d: Vector3 = direction.normalized()
	var lattice: Array = Deltas.dir_to_lattice(d)
	var face: int = int(lattice[0])
	var i: int = int(round(float(lattice[1])))
	var j: int = int(round(float(lattice[2])))
	var persistent: Vector2 = _read_state(face, i, j)
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed:
		var active: Vector2 = TerrainDeformationGPU.active_state(d)
		return Vector2(maxf(persistent.x, active.x), maxf(persistent.y, active.y))
	return persistent


func clear_state() -> void:
	_state_tiles.clear()
	_deformation_events = 0
	_displaced_volume_m3 = 0.0
	_gpu_contact_events = 0
	_cpu_fallback_events = 0
	if TerrainDeformationGPU.ready_state:
		TerrainDeformationGPU.clear_active()


func stats() -> Dictionary:
	return {
		"state_tiles": _state_tiles.size(),
		"events": _deformation_events,
		"displaced_volume_m3": _displaced_volume_m3,
		"visual_notify_hz": MAX_VISUAL_NOTIFY_HZ,
		"gpu_contact_events": _gpu_contact_events,
		"cpu_fallback_events": _cpu_fallback_events,
		"gpu": TerrainDeformationGPU.stats(),
		"gpu_queries": TerrainDeformationQueryGPU.stats() if TerrainDeformationQueryGPU.ready_state else {},
	}


func serialize() -> Dictionary:
	var keys := PackedInt64Array()
	var blob := PackedByteArray()
	for key: Variant in _state_tiles.keys():
		keys.append(int(key))
		var tile: PackedFloat32Array = _state_tiles[key]
		blob.append_array(tile.to_byte_array())
	return {"version": 1, "keys": keys, "state": blob}


func deserialize(data: Dictionary) -> void:
	_state_tiles.clear()
	if TerrainDeformationGPU.ready_state:
		TerrainDeformationGPU.clear_active()
	if not data.has("keys") or not data.has("state"):
		return
	var keys: PackedInt64Array = data["keys"]
	var blob: PackedByteArray = data["state"]
	var stride: int = STATE_TILE * STATE_TILE * STATE_CHANNELS * 4
	for index in keys.size():
		var slice: PackedByteArray = blob.slice(index * stride, (index + 1) * stride)
		_state_tiles[keys[index]] = slice.to_float32_array()


func _apply_patch(center_dir: Vector3, radius_m: float, sink_m: float,
		compaction_gain: float, damage_gain: float, rim_fraction: float,
		max_depth_m: float, footprint_type: int = FOOTPRINT_GENERIC,
		shape_radius_m: float = 0.0, penetration_m: float = 0.0) -> float:
	var planet_radius: float = Planet.cfg.planet_radius
	var spacing_m: float = Deltas.sample_spacing(planet_radius)
	var cell_area_m2: float = spacing_m * spacing_m
	var lattice: Array = Deltas.dir_to_lattice(center_dir)
	var face: int = int(lattice[0])
	var center_i: int = int(round(float(lattice[1])))
	var center_j: int = int(round(float(lattice[2])))
	var cells: int = maxi(1, int(ceil(radius_m / spacing_m)) + 1)
	var removed_volume_m3 := 0.0
	for y in range(-cells, cells + 1):
		for x in range(-cells, cells + 1):
			var distance_m: float = Vector2(float(x), float(y)).length() * spacing_m
			if distance_m > radius_m:
				continue
			var weight: float = _footprint_weight(distance_m, radius_m,
				footprint_type, shape_radius_m, penetration_m)
			if weight <= 0.0:
				continue
			var requested_delta: float = -sink_m * weight
			var actual_delta: float = Deltas.add_offset(face, center_i + x, center_j + y,
				requested_delta, -max_depth_m, max_depth_m * 0.25)
			if actual_delta >= -1e-8:
				continue
			removed_volume_m3 += -actual_delta * cell_area_m2
			var comp_delta: float = -actual_delta * compaction_gain
			var dmg_delta: float = -actual_delta * damage_gain
			_add_state(face, center_i + x, center_j + y, comp_delta, dmg_delta)
	if removed_volume_m3 <= 1e-9 or rim_fraction <= 1e-6:
		return removed_volume_m3
	var ring_inner: float = radius_m * 1.08
	var ring_outer: float = radius_m * 1.9
	var ring_cells: int = maxi(cells + 1, int(ceil(ring_outer / spacing_m)) + 1)
	var ring_points: Array[Vector2i] = []
	var ring_weights := PackedFloat32Array()
	var total_weight := 0.0
	for y in range(-ring_cells, ring_cells + 1):
		for x in range(-ring_cells, ring_cells + 1):
			var distance_m: float = Vector2(float(x), float(y)).length() * spacing_m
			if distance_m < ring_inner or distance_m > ring_outer:
				continue
			var mid: float = (ring_inner + ring_outer) * 0.5
			var half_width: float = maxf((ring_outer - ring_inner) * 0.5, 1e-4)
			var w: float = maxf(1.0 - absf(distance_m - mid) / half_width, 0.0)
			if w <= 0.0:
				continue
			ring_points.append(Vector2i(center_i + x, center_j + y))
			ring_weights.append(w)
			total_weight += w
	if total_weight <= 1e-8:
		return removed_volume_m3
	var target_rim_volume_m3: float = removed_volume_m3 * clampf(rim_fraction, 0.0, 0.95)
	for index in ring_points.size():
		var point: Vector2i = ring_points[index]
		var share: float = ring_weights[index] / total_weight
		var rise_m: float = target_rim_volume_m3 * share / maxf(cell_area_m2, 1e-6)
		Deltas.add_offset(face, point.x, point.y, rise_m, -max_depth_m, max_depth_m * 0.25)
	return removed_volume_m3


func _footprint_weight(distance_m: float, radius_m: float, footprint_type: int,
		shape_radius_m: float, penetration_m: float) -> float:
	if footprint_type == FOOTPRINT_SPHERE and shape_radius_m > 1e-4 and penetration_m > 1e-5:
		if distance_m >= shape_radius_m:
			return 0.0
		var root_term: float = maxf(shape_radius_m * shape_radius_m - distance_m * distance_m, 0.0)
		var sag_m: float = shape_radius_m - sqrt(root_term)
		var overlap_m: float = maxf(penetration_m - sag_m, 0.0)
		return clampf(overlap_m / penetration_m, 0.0, 1.0)
	var q: float = distance_m / maxf(radius_m, 1e-4)
	var weight: float = 1.0 - q * q
	return weight * weight


func _read_state(face: int, i: int, j: int) -> Vector2:
	var address: Vector3i = Deltas.canonical_address(face, i, j)
	if address.x < 0:
		return Vector2.ZERO
	var key: int = Deltas.tile_key(address.x, address.y >> 6, address.z >> 6)
	if not _state_tiles.has(key):
		return Vector2.ZERO
	var tile: PackedFloat32Array = _state_tiles[key]
	var local_index: int = ((address.z & (STATE_TILE - 1)) * STATE_TILE +
		(address.y & (STATE_TILE - 1))) * STATE_CHANNELS
	return Vector2(tile[local_index], tile[local_index + 1])


func _add_state(face: int, i: int, j: int, compaction_delta: float, damage_delta: float) -> void:
	var address: Vector3i = Deltas.canonical_address(face, i, j)
	if address.x < 0:
		return
	var key: int = Deltas.tile_key(address.x, address.y >> 6, address.z >> 6)
	if not _state_tiles.has(key):
		var fresh := PackedFloat32Array()
		fresh.resize(STATE_TILE * STATE_TILE * STATE_CHANNELS)
		_state_tiles[key] = fresh
	var tile: PackedFloat32Array = _state_tiles[key]
	var local_index: int = ((address.z & (STATE_TILE - 1)) * STATE_TILE +
		(address.y & (STATE_TILE - 1))) * STATE_CHANNELS
	tile[local_index] = clampf(tile[local_index] + compaction_delta, 0.0, 1.0)
	tile[local_index + 1] = clampf(tile[local_index + 1] + damage_delta, 0.0, 1.0)
	_state_tiles[key] = tile


func _queue_notify(center_dir: Vector3, radius_m: float) -> void:
	if not _notify_pending:
		_notify_center = center_dir.normalized()
		_notify_radius_m = radius_m
		_notify_pending = true
		return
	var surface_distance_m: float = acos(clampf(_notify_center.dot(center_dir.normalized()), -1.0, 1.0))
	surface_distance_m *= Planet.cfg.planet_radius
	_notify_radius_m = maxf(_notify_radius_m, surface_distance_m + radius_m)


func _empty_result() -> Dictionary:
	return {
		"support_force_n": 0.0,
		"support_pressure_pa": 0.0,
		"demand_pressure_pa": 0.0,
		"yield_pressure_pa": 0.0,
		"bearing_ratio": 0.0,
		"shear_ratio": 0.0,
		"sink_rate_mps": 0.0,
		"deformed_m": 0.0,
		"moved_volume_m3": 0.0,
		"compaction": 0.0,
		"damage": 0.0,
		"restitution": 0.0,
		"material": "none",
		"gpu_active": false,
		"footprint_type": FOOTPRINT_GENERIC,
	}


func _material(material_id: int) -> Dictionary:
	match material_id:
		MATERIAL_WET_CLAY:
			return {
				"name": "Wet clay", "yield_pa": 70000.0, "hardening": 2.7,
				"stiffness_pa_per_m": 260000.0, "damping_pa_s_per_m": 180000.0,
				"plastic_rate_mps": 0.44, "shear_pa": 52000.0, "cut_rate": 0.75,
				"compaction_gain": 0.72, "damage_gain": 0.30, "rim_fraction": 0.34,
				"max_depth_m": 8.0, "restitution": 0.02,
			}
		MATERIAL_GRAVEL:
			return {
				"name": "Compacted gravel", "yield_pa": 420000.0, "hardening": 1.9,
				"stiffness_pa_per_m": 1500000.0, "damping_pa_s_per_m": 110000.0,
				"plastic_rate_mps": 0.10, "shear_pa": 260000.0, "cut_rate": 0.20,
				"compaction_gain": 0.34, "damage_gain": 0.22, "rim_fraction": 0.68,
				"max_depth_m": 4.0, "restitution": 0.10,
			}
		MATERIAL_ROCK:
			return {
				"name": "Competent rock", "yield_pa": 18000000.0, "hardening": 0.5,
				"stiffness_pa_per_m": 90000000.0, "damping_pa_s_per_m": 450000.0,
				"plastic_rate_mps": 0.002, "shear_pa": 12000000.0, "cut_rate": 0.004,
				"compaction_gain": 0.02, "damage_gain": 0.08, "rim_fraction": 0.05,
				"max_depth_m": 1.5, "restitution": 0.20,
			}
		_:
			return {
				"name": "Dry topsoil", "yield_pa": 125000.0, "hardening": 2.4,
				"stiffness_pa_per_m": 520000.0, "damping_pa_s_per_m": 130000.0,
				"plastic_rate_mps": 0.28, "shear_pa": 85000.0, "cut_rate": 0.48,
				"compaction_gain": 0.52, "damage_gain": 0.26, "rim_fraction": 0.56,
				"max_depth_m": 6.0, "restitution": 0.05,
			}
