extends "res://scripts/terrain/gpu_terrain_height_query.gd"
## Strict contact-grade accessors layered over the pooled terrain query.
##
## General terrain lookups may accept a broad nearby cached sample while a precise
## GPU result is in flight. Rigid contact/AGL use a much tighter local sample.
## Distance matching is deliberately squared-chord based: aim() can perform many
## thousands of cache comparisons per frame, and trigonometric arc-distance calls
## there were a major CPU hot path after the contact cache became well populated.

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
	# For local terrain lookups chord and arc distance differ by an immeasurably
	# small amount, while this form needs no sqrt/asin/acos inside the GDScript loop.
	var radius: float = Planet.cfg.planet_radius
	return a.distance_squared_to(b) * radius * radius


## Override the pooled matcher for both broad and contact lookups. The caller still
## supplies its own distance/age policy; only the hot distance metric changes.
func _find_sample(direction: Vector3, max_distance_m: float, max_age_s: float) -> Dictionary:
	if Planet.cfg == null:
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


## Same no-trig metric for the inherited general queue dedupe path.
func _enqueue(direction: Vector3) -> void:
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
	var d: Vector3 = direction.normalized()
	request_contact_height(d)
	var found: Dictionary = _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S)
	if found.is_empty():
		return fallback
	return float(found["height"]) + Deltas.offset_at(d)


func has_contact_height(direction: Vector3) -> bool:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return false
	var d: Vector3 = direction.normalized()
	request_contact_height(d)
	return not _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S).is_empty()


## The terrain renderer asks this before it may classify the camera as underground.
## The inherited 64 m freshness test could take a height from a nearby slope/ridge,
## report a false negative AGL, then hide every terrain batch. Freshness for this
## autoload therefore means a genuine local contact sample.
func has_fresh_height(direction: Vector3) -> bool:
	return has_contact_height(direction)


## Cache lookups already reject stale samples by age, so physical removal does not
## need to scan the entire sample array every rendered frame. Staggering housekeeping
## removes a small but measurable CPU pulse without changing contact freshness.
func _prune_samples() -> void:
	var now_msec: int = Time.get_ticks_msec()
	if now_msec < _next_sample_prune_msec:
		return
	_next_sample_prune_msec = now_msec + SAMPLE_PRUNE_INTERVAL_MS
	super._prune_samples()
