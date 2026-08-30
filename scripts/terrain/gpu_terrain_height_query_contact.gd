extends "res://scripts/terrain/gpu_terrain_height_query.gd"
## Strict contact-grade accessors layered over the pooled terrain query.
##
## Procedural terrain uses tight cached GPU samples. Blank terrain has no height
## texture to query, so contact is the analytic body sphere at height 0.

# 15 cm is below the active terrain's 25 cm physical texel size and remains stable
# with float32 unit directions on a ~1000 km-radius planet.
const CONTACT_CACHE_DISTANCE_M := 0.15
const CONTACT_CACHE_AGE_S := 0.45
const CONTACT_DEDUPE_DISTANCE_M := 0.10
const SAMPLE_PRUNE_INTERVAL_MS: int = 250

var _next_sample_prune_msec: int = 0


func _surface_distance_sq_m(a: Vector3, b: Vector3) -> float:
	if Planet.cfg == null:
		return INF
	var radius: float = Planet.cfg.planet_radius
	return a.distance_squared_to(b) * radius * radius


func _find_sample(direction: Vector3, max_distance_m: float, max_age_s: float) -> Dictionary:
	if Planet.cfg == null or _is_blank_backend():
		return {}
	var now_s: float = Time.get_ticks_msec() * 0.001
	var max_distance_sq_m: float = max_distance_m * max_distance_m
	var best: Dictionary = {}
	var best_distance_sq_m: float = INF
	var radius: float = Planet.cfg.planet_radius
	var radius_sq: float = radius * radius
	for sample: Dictionary in _samples:
		var age_s: float = now_s - float(sample["time"])
		if age_s > max_age_s:
			continue
		var sample_dir: Vector3 = sample["dir"]
		var distance_sq_m: float = sample_dir.distance_squared_to(direction) * radius_sq
		if distance_sq_m <= max_distance_sq_m and distance_sq_m < best_distance_sq_m:
			best_distance_sq_m = distance_sq_m
			best = sample
	return best


func _enqueue(direction: Vector3) -> void:
	if _is_blank_backend():
		return
	if direction.length_squared() <= 1e-12 or Planet.cfg == null:
		return
	var d: Vector3 = direction.normalized()
	if not _find_sample(d, DEDUPE_DISTANCE_M, 0.08).is_empty():
		return
	var dedupe_sq_m: float = DEDUPE_DISTANCE_M * DEDUPE_DISTANCE_M
	for queued: Vector3 in _pending:
		if _surface_distance_sq_m(queued, d) <= dedupe_sq_m:
			return
	if _pending.size() >= MAX_PENDING:
		_pending.pop_front()
	_pending.append(d)


func request_contact_height(direction: Vector3) -> void:
	if _is_blank_backend():
		return
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return
	var d: Vector3 = direction.normalized()
	if not _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S).is_empty():
		return
	var dedupe_sq_m: float = CONTACT_DEDUPE_DISTANCE_M * CONTACT_DEDUPE_DISTANCE_M
	for queued: Vector3 in _pending:
		if _surface_distance_sq_m(queued, d) <= dedupe_sq_m:
			return
	if _pending.size() >= MAX_PENDING:
		_pending.pop_back()
	_pending.push_front(d)


func contact_height_for_direction(direction: Vector3, fallback: float = NAN) -> float:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return fallback
	if _is_blank_backend():
		return 0.0
	var d: Vector3 = direction.normalized()
	request_contact_height(d)
	var found: Dictionary = _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S)
	if found.is_empty():
		return fallback
	return float(found["height"]) + Deltas.offset_at(d)


func has_contact_height(direction: Vector3) -> bool:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return false
	if _is_blank_backend():
		return true
	var d: Vector3 = direction.normalized()
	request_contact_height(d)
	return not _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S).is_empty()


func has_fresh_height(direction: Vector3) -> bool:
	return has_contact_height(direction)


func _prune_samples() -> void:
	if _is_blank_backend():
		_samples.clear()
		return
	var now_msec: int = Time.get_ticks_msec()
	if now_msec < _next_sample_prune_msec:
		return
	_next_sample_prune_msec = now_msec + SAMPLE_PRUNE_INTERVAL_MS
	super._prune_samples()
