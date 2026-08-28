extends "res://scripts/terrain/gpu_terrain_height_query.gd"
## Strict contact-grade accessors layered over the pooled terrain query.
##
## General terrain lookups intentionally accept a broad nearby cached sample while
## a precise GPU result is in flight. Rigid contacts and altitude/culling decisions
## must not: only a sample within centimetres of the requested world point is
## accepted, and that request is pushed to the front of the pooled GPU queue instead
## of being deduplicated against an unrelated aiming/sample point.

const CONTACT_CACHE_DISTANCE_M := 0.035
const CONTACT_CACHE_AGE_S := 0.80
const CONTACT_DEDUPE_DISTANCE_M := 0.025


func request_contact_height(direction: Vector3) -> void:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return
	var d: Vector3 = direction.normalized()
	if not _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S).is_empty():
		return
	for queued: Vector3 in _pending:
		var distance_m: float = acos(clampf(queued.dot(d), -1.0, 1.0)) \
			* Planet.cfg.planet_radius
		if distance_m <= CONTACT_DEDUPE_DISTANCE_M:
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


## `spherical_geometry_clipmap_global_gpu.gd` uses has_fresh_height() before it is
## allowed to classify the camera as underground. The inherited implementation
## accepted a sample as far as 64 m away, which could make a camera physically above
## the local surface report negative AGL on sloped/rough terrain and hide all ground.
## On this production contact subclass, "fresh" therefore means contact-grade.
func has_fresh_height(direction: Vector3) -> bool:
	return has_contact_height(direction)
