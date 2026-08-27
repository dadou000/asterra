extends "res://scripts/terrain/gpu_terrain_height_query.gd"
## Strict contact-grade accessors layered over the pooled terrain query.
##
## General terrain lookups intentionally accept a broad nearby cached sample while
## a precise GPU result is still in flight. That is useful for aiming/streaming but
## is unsafe for rigid contacts: a sample tens of metres away can differ by metres
## in height and make an object appear to touch terrain while it is still airborne.

const CONTACT_CACHE_DISTANCE_M := 0.45
const CONTACT_CACHE_AGE_S := 0.55


func contact_height_for_direction(direction: Vector3, fallback: float = NAN) -> float:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return fallback
	var d: Vector3 = direction.normalized()
	var found: Dictionary = _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S)
	if found.is_empty():
		return fallback
	return float(found["height"]) + Deltas.offset_at(d)


func has_contact_height(direction: Vector3) -> bool:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return false
	var d: Vector3 = direction.normalized()
	return not _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S).is_empty()
