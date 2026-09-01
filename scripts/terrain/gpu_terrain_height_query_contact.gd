extends "res://scripts/terrain/gpu_terrain_height_query.gd"
## Strict contact-grade accessors layered over the pooled terrain query.
##
## Authored displacement is authoritative in both terrain backends. Procedural
## bodies evaluate the same graph after generated height and before sparse Deltas;
## Blank bodies use the graph as the complete height above the analytic sphere.

const CONTACT_CACHE_DISTANCE_M := 0.15
const CONTACT_CACHE_AGE_S := 0.45
const CONTACT_DEDUPE_DISTANCE_M := 0.10
const SAMPLE_PRUNE_INTERVAL_MS: int = 250
const AUTHOR_NORMAL_SAMPLE_M: float = 1.5
const AUTHOR_CONTACT_LEVEL: int = 0

var _next_sample_prune_msec: int = 0
var _author_runtime: Node


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


func height_for_direction(direction: Vector3, fallback: float) -> float:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return fallback
	var d: Vector3 = direction.normalized()
	if _is_blank_backend():
		return _authored_displacement(d, 0.0, -1)
	var generated: float = super.pristine_height_for_direction(d, NAN)
	if is_nan(generated):
		return fallback
	return generated + _authored_displacement(d, generated, _procedural_biome_id(d)) \
		+ Deltas.offset_at(d)


func pristine_height_for_direction(direction: Vector3, fallback: float = NAN) -> float:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return fallback
	var d: Vector3 = direction.normalized()
	if _is_blank_backend():
		return _authored_displacement(d, 0.0, -1)
	var generated: float = super.pristine_height_for_direction(d, NAN)
	if is_nan(generated):
		return fallback
	return generated + _authored_displacement(d, generated, _procedural_biome_id(d))


func surface_for_direction(direction: Vector3, fallback_height: float) -> Dictionary:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return {"height": fallback_height, "normal": direction.normalized(), "precise": false}
	var d: Vector3 = direction.normalized()
	if _is_blank_backend():
		return _authored_surface(d, fallback_height, true)

	request_surface(d)
	var basis: Array[Vector3] = _tangent_basis(d)
	var radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	var theta: float = AUTHOR_NORMAL_SAMPLE_M / radius
	var dx: Vector3 = (d + basis[0] * theta).normalized()
	var dy: Vector3 = (d + basis[1] * theta).normalized()
	var generated0: float = super.pristine_height_for_direction(d, NAN)
	var generated_x: float = super.pristine_height_for_direction(dx, NAN)
	var generated_y: float = super.pristine_height_for_direction(dy, NAN)
	if is_nan(generated0):
		return {"height": fallback_height, "normal": d, "precise": false}
	var h0: float = generated0 + _authored_displacement(d, generated0,
		_procedural_biome_id(d)) + Deltas.offset_at(d)
	if is_nan(generated_x) or is_nan(generated_y):
		return {"height": h0, "normal": d, "precise": true, "shader_displacement": true}
	var hx: float = generated_x + _authored_displacement(dx, generated_x,
		_procedural_biome_id(dx)) + Deltas.offset_at(dx)
	var hy: float = generated_y + _authored_displacement(dy, generated_y,
		_procedural_biome_id(dy)) + Deltas.offset_at(dy)
	return _surface_from_samples(d, dx, dy, h0, hx, hy, radius, false)


func _authored_surface(direction: Vector3, fallback_height: float,
		analytic_base: bool) -> Dictionary:
	var d: Vector3 = direction.normalized()
	var radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	var basis: Array[Vector3] = _tangent_basis(d)
	var theta: float = AUTHOR_NORMAL_SAMPLE_M / radius
	var dx: Vector3 = (d + basis[0] * theta).normalized()
	var dy: Vector3 = (d + basis[1] * theta).normalized()
	var h0: float = _authored_displacement(d, 0.0, -1)
	var hx: float = _authored_displacement(dx, 0.0, -1)
	var hy: float = _authored_displacement(dy, 0.0, -1)
	if not is_finite(h0):
		h0 = fallback_height
	return _surface_from_samples(d, dx, dy, h0, hx, hy, radius, analytic_base)


func _surface_from_samples(d: Vector3, dx: Vector3, dy: Vector3,
		h0: float, hx: float, hy: float, radius: float,
		analytic_base: bool) -> Dictionary:
	var p0: Vector3 = d * (radius + h0)
	var px: Vector3 = dx * (radius + hx)
	var py: Vector3 = dy * (radius + hy)
	var normal: Vector3 = (px - p0).cross(py - p0)
	if normal.length_squared() <= 1e-10:
		normal = d
	else:
		normal = normal.normalized()
		if normal.dot(d) < 0.0:
			normal = -normal
	return {
		"height": h0,
		"normal": normal,
		"precise": true,
		"analytic_base": analytic_base,
		"shader_displacement": true,
	}


func contact_height_for_direction(direction: Vector3, fallback: float = NAN) -> float:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return fallback
	var d: Vector3 = direction.normalized()
	if _is_blank_backend():
		return _authored_displacement(d, 0.0, -1)
	request_contact_height(d)
	var found: Dictionary = _find_sample(d, CONTACT_CACHE_DISTANCE_M, CONTACT_CACHE_AGE_S)
	if found.is_empty():
		return fallback
	var generated: float = float(found["height"])
	return generated + _authored_displacement(d, generated, _procedural_biome_id(d)) \
		+ Deltas.offset_at(d)


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


func _authored_displacement(direction: Vector3, base_height_m: float,
		biome_id: int) -> float:
	if _author_runtime == null or not is_instance_valid(_author_runtime):
		_author_runtime = get_tree().get_first_node_in_group(&"terrain_displacement_runtime")
	if _author_runtime == null or not _author_runtime.has_method("evaluate_height"):
		return 0.0
	return float(_author_runtime.call("evaluate_height", direction, base_height_m,
		AUTHOR_CONTACT_LEVEL, biome_id, NAN))


func _procedural_biome_id(direction: Vector3) -> int:
	if Planet.get("ready_state") and Planet.has_method("sample_info"):
		var info: Dictionary = Planet.call("sample_info", direction) as Dictionary
		return clampi(int(info.get("biome", 0)), 0, 17)
	return 0


func _prune_samples() -> void:
	if _is_blank_backend():
		_samples.clear()
		return
	var now_msec: int = Time.get_ticks_msec()
	if now_msec < _next_sample_prune_msec:
		return
	_next_sample_prune_msec = now_msec + SAMPLE_PRUNE_INTERVAL_MS
	super._prune_samples()
