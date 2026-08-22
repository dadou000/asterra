extends "res://scripts/character/dense_facial_hair_groom.gd"

## Point-filtered direct-surface beard sampler.
##
## Important rule: triangle membership never decides whether a beard patch exists.
## The sampler only builds a broad pool of plausible lower-head triangles. Every
## individual follicle is then accepted by the continuous 3D beard field inherited
## by _build_beard_mesh(). This removes the polygon-shaped islands caused by
## rejecting whole triangles from centroid/depth/bone tests.
##
## Roots are still born directly on real source triangles and keep their exact
## triangle+barycentric binding for facial morph animation. There are no per-hair
## raycasts or nearest-triangle searches.

var _point_sampler_candidates := 0
var _point_sampler_kept := 0
var _point_sampler_rear_rejected := 0

func _clear_surface_beard_cache() -> void:
	_point_sampler_candidates = 0
	_point_sampler_kept = 0
	_point_sampler_rear_rejected = 0
	super._clear_surface_beard_cache()

func _ensure_beard_surface_sampler() -> bool:
	if _beard_sampler_ready:
		return not _beard_sampler_triangles.is_empty()
	var started_usec: int = Time.get_ticks_usec()
	_beard_sampler_ready = true

	if not _ensure_morph_source_cache() or _head.is_empty() or _mount == null:
		_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return false

	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var front: Vector3 = Vector3(0.0, 0.0, _front_sign)
	var total_area := 0.0

	for tri_index in _morph_triangles.size():
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var centroid: Vector3 = (a + b + c) / 3.0
		_point_sampler_candidates += 1

		# Only a deliberately broad anatomical envelope is allowed here. Do not
		# evaluate the beard density field at the triangle centroid: doing that
		# turns the source topology into visible beard islands.
		var ax: float = absf(centroid.x - center.x) / rx
		var down: float = (center.y - centroid.y) / ry
		if ax > 1.02 or down < -0.02 or down > 0.92:
			continue

		var cross_value: Vector3 = (b - a).cross(c - a)
		var twice_area: float = cross_value.length()
		if twice_area < 0.00000001:
			continue
		var normal: Vector3 = cross_value / twice_area
		var radial: Vector3 = centroid - center
		if radial.length_squared() > 0.000001 and normal.dot(radial) < 0.0:
			normal = -normal

		# Reject only surfaces that are unquestionably on the back of the head.
		# Side/jaw/underside triangles remain in the pool and are decided per point.
		if normal.dot(front) < -0.72:
			_point_sampler_rear_rejected += 1
			continue

		var area: float = twice_area * 0.5
		total_area += area
		_beard_sampler_triangles.append(tri_index)
		_beard_sampler_cumulative_area.append(total_area)
		_point_sampler_kept += 1

	_beard_sampler_total_area = total_area
	_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return not _beard_sampler_triangles.is_empty() and total_area > 0.0

## Continuous point-level beard field. All cutoffs operate on the sampled skin
## point, never on its triangle, so boundaries remain smooth across topology.
func _beard_field(world_point: Vector3, coverage: float, fullness: float) -> Vector4:
	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var rz: float = maxf(float(_head["rz"]), 0.001)
	var dx: float = world_point.x - center.x
	var ax: float = absf(dx) / rx
	var down: float = (center.y - world_point.y) / ry
	var depth: float = (world_point.z - center.z) * _front_sign / rz
	if ax > 0.92 or down < 0.015 or down > 0.84:
		return Vector4.ZERO

	var side: float = -1.0 if dx < 0.0 else 1.0
	var outerness: float = clampf((ax - 0.22) / 0.66, 0.0, 1.0)

	# Mouth exclusion stays exact and point-local.
	var mouth_center_y: float = center.y - ry * 0.22
	var mouth_x: float = absf(dx) / maxf(rx * 0.40, 0.001)
	var mouth_y: float = absf(world_point.y - mouth_center_y) / maxf(ry * 0.125, 0.001)
	if mouth_x * mouth_x + mouth_y * mouth_y < 1.0:
		return Vector4.ZERO

	# Continuous depth gate. Central chin must be on the frontal shell, while the
	# mandibular angle is allowed to wrap farther toward the side. Using a smooth
	# weight instead of rejecting triangles removes topology-shaped gaps.
	var min_depth: float = lerpf(0.015, -0.22, smoothstep(0.30, 0.88, ax))
	if down > 0.60:
		min_depth += lerpf(0.015, -0.035, smoothstep(0.0, 0.70, ax))
	var depth_gate: float = smoothstep(min_depth - 0.10, min_depth + 0.035, depth)

	# Suppress the lateral neck progressively below the jaw. This remains a smooth
	# point-level fade, so no triangle-shaped patches can appear.
	var lower_amount: float = smoothstep(0.58, 0.82, down)
	var lateral_amount: float = smoothstep(0.42, 0.76, ax)
	var neck_gate: float = 1.0 - lower_amount * lateral_amount

	# Cheek coverage. The top edge rises toward the sideburn and overlaps the jaw.
	var cheek_top: float = lerpf(0.235, 0.080, coverage) - 0.050 * outerness
	var cheek_bottom := 0.64
	var cheek_vertical: float = smoothstep(cheek_top, cheek_top + 0.055, down)
	cheek_vertical *= 1.0 - smoothstep(cheek_bottom - 0.060, cheek_bottom, down)
	var cheek_side: float = smoothstep(0.22, 0.31, ax)
	cheek_side *= 1.0 - smoothstep(0.84, 0.92, ax)
	var cheek: float = cheek_vertical * cheek_side * lerpf(0.76, 1.0, coverage)

	# Broad jaw band; deliberately overlaps both cheek and chin.
	var jaw_u: float = clampf(ax / 0.86, 0.0, 1.0)
	var jaw_center: float = lerpf(0.735, 0.505, pow(jaw_u, 1.30))
	var jaw_half_width: float = 0.105 * lerpf(0.86, 1.18, clampf(fullness, 0.15, 1.35))
	var jaw_distance: float = absf(down - jaw_center)
	var jaw: float = 1.0 - smoothstep(jaw_half_width * 0.58, jaw_half_width, jaw_distance)
	jaw *= 1.0 - smoothstep(0.84, 0.92, ax)

	# Wider chin/goatee field than before so the front chin cannot disappear when
	# viewed obliquely. Its lower edge fades before the throat.
	var chin_x: float = 1.0 - smoothstep(0.36, 0.47, ax)
	var chin_y: float = smoothstep(0.30, 0.37, down)
	chin_y *= 1.0 - smoothstep(0.80, 0.85, down)
	var chin: float = chin_x * chin_y

	# Fill the mouth-corner / lower-cheek transition explicitly.
	var connector_x: float = smoothstep(0.12, 0.20, ax) * (1.0 - smoothstep(0.56, 0.64, ax))
	var connector_y: float = smoothstep(0.35, 0.42, down) * (1.0 - smoothstep(0.68, 0.75, down))
	var connector: float = connector_x * connector_y

	# Probabilistic union instead of max() makes overlapping zones genuinely
	# denser and prevents seams between independently-shaped masks.
	var weight: float = 1.0 - (1.0 - cheek) * (1.0 - jaw) * (1.0 - chin) * (1.0 - connector)
	weight *= depth_gate * neck_gate
	weight *= clampf(0.82 + fullness * 0.16, 0.75, 1.0)
	if weight <= 0.0005:
		return Vector4.ZERO

	var lateral := 0.0
	if chin >= jaw and chin >= cheek and chin >= connector:
		lateral = side * ax * 0.075
	elif connector >= cheek and connector >= jaw:
		lateral = -side * 0.05
	elif jaw >= cheek:
		lateral = -side * 0.19 * jaw_u
	else:
		var cheek_down: float = clampf((down - cheek_top) / maxf(cheek_bottom - cheek_top, 0.001), 0.0, 1.0)
		lateral = -side * lerpf(0.16, 0.06, cheek_down)

	var chin_mix: float = maxf(chin, connector * 0.50)
	chin_mix *= smoothstep(0.38, 0.78, down)
	return Vector4(clampf(weight, 0.0, 1.0), lateral, -1.0, clampf(chin_mix, 0.0, 1.0))

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • point sampler %d/%d triangles kept (%d rear rejected)" % [
		base,
		_point_sampler_kept,
		_point_sampler_candidates,
		_point_sampler_rear_rejected
	]
