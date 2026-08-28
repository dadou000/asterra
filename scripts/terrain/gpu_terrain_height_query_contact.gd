extends "res://scripts/terrain/gpu_terrain_height_query.gd"
## Strict contact-grade accessors layered over the pooled terrain query.
##
## General terrain lookups intentionally accept a broad nearby cached sample while
## a precise GPU result is in flight. Rigid contacts and altitude/culling decisions
## use a much tighter local sample. Distance matching uses chord length instead of
## acos(dot): at a ~1000 km planet radius the latter loses far too much precision
## for sub-metre contact queries when directions are stored as float Vector3 values.

# 15 cm is below the active terrain's 25 cm physical texel size and remains stable
# with float32 unit directions on a 1000 km-radius planet. The previous 3.5 cm gate
# was below the reliable angular precision of Vector3 and could starve exact contact.
const CONTACT_CACHE_DISTANCE_M := 0.15
const CONTACT_CACHE_AGE_S := 0.45
const CONTACT_DEDUPE_DISTANCE_M := 0.10


func _surface_distance_m(a: Vector3, b: Vector3) -> float:
	if Planet.cfg == null:
		return INF
	# For two unit vectors, chord = 2*sin(theta/2). This is well conditioned near
	# theta=0, unlike acos(dot), and is effectively arc length at contact scales.
	var chord: float = (a - b).length()
	var half_chord: float = clampf(chord * 0.5, 0.0, 1.0)
	return 2.0 * Planet.cfg.planet_radius * asin(half_chord)


## Override the pooled cache matcher for the production contact subclass. All
## inherited broad lookups still pass their own max_distance_m; only the numerical
## distance metric changes.
func _find_sample(direction: Vector3, max_distance_m: float, max_age_s: float) -> Dictionary:
	if Planet.cfg == null:
		return {}
	var now_s: float = Time.get_ticks_msec() * 0.001
	var best: Dictionary = {}
	var best_distance_m: float = INF
	for sample: Dictionary in _samples:
		var age_s: float = now_s - float(sample["time"])
		if age_s > max_age_s:
			continue
		var sample_dir: Vector3 = sample["dir"]
		var distance_m: float = _surface_distance_m(sample_dir, direction)
		if distance_m <= max_distance_m and distance_m < best_distance_m:
			best_distance_m = distance_m
			best = sample
	return best


## Same numerical fix for the inherited general queue dedupe path. This prevents a
## nearby-but-not-identical aim/terrain request from suppressing the contact sample.
func _enqueue(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12 or Planet.cfg == null:
		return
	var d: Vector3 = direction.normalized()
	if not _find_sample(d, DEDUPE_DISTANCE_M, 0.08).is_empty():
		return
	for queued: Vector3 in _pending:
		if _surface_distance_m(queued, d) <= DEDUPE_DISTANCE_M:
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
	for queued: Vector3 in _pending:
		if _surface_distance_m(queued, d) <= CONTACT_DEDUPE_DISTANCE_M:
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
