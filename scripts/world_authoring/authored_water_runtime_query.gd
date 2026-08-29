class_name AuthoredWaterRuntimeQuery
extends "res://scripts/world_authoring/authored_water_runtime.gd"
## Gameplay/query extension for authored Planet Studio water.
##
## Rendering remains owned by AuthoredWaterRuntime. This final layer exposes the
## same staged lake polygons and river Bezier ribbons as a cheap CPU query so
## buoyancy, swimming, particles and debugging can agree on feature membership,
## surface altitude, local depth and authored current. The query also evaluates
## the exact compact wave field used by authored_water_surface.gdshader; both CPU
## and shader consume the same explicit runtime clock rather than independent TIME
## sources, preventing visual/physics phase drift.

const RIVER_QUERY_COARSE_STEPS: int = 12
const RIVER_QUERY_REFINE_STEPS: int = 7
const RIVER_QUERY_DT: float = 0.0025
const WAVE_TIME_WRAP_S: float = 4096.0
const WAVE_AXIS_A := Vector3(0.827, 0.201, 0.525)
const WAVE_AXIS_B := Vector3(-0.436, 0.331, 0.837)
const LAKE_WAVE_AMPLITUDE_M: float = 0.095
const RIVER_WAVE_AMPLITUDE_SCALE: float = 0.24
const LAKE_WAVELENGTH_M: float = 18.0
const RIVER_WAVELENGTH_M: float = 5.5

var _author_time_s: float = 0.0


func _process(delta: float) -> void:
	_author_time_s = fposmod(float(Time.get_ticks_msec()) * 0.001, WAVE_TIME_WRAP_S)
	super._process(delta)
	# The base renderer has already synchronized all ordinary terrain/water
	# resources. Publish the same explicit clock to every authored-water material.
	for record: Dictionary in _records:
		var material: ShaderMaterial = record.get("material") as ShaderMaterial
		if material != null:
			material.set_shader_parameter("u_author_time_s", _author_time_s)


func query_render_point(render_point: Vector3) -> Dictionary:
	return query_world_point(Frames.to_world(render_point))


func query_world_point(world_point: Vec3D) -> Dictionary:
	var matches: Array[Dictionary] = query_all_world_point(world_point)
	if matches.is_empty():
		return {}
	# Overlapping authored features are resolved physically: the highest local
	# surface is the one a probe encounters first while moving down from the air.
	var best: Dictionary = matches[0]
	for index: int in range(1, matches.size()):
		var candidate: Dictionary = matches[index]
		if float(candidate.get("surface_altitude_m", -INF)) \
				> float(best.get("surface_altitude_m", -INF)):
			best = candidate
	return best


func query_all_world_point(world_point: Vec3D) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if world_point == null or _session == null:
		return out
	var body: Resource = _session.active_body() as Resource
	var water: Resource = _session.active_water_profile() as Resource
	if body == null or water == null:
		return out
	var point_radius: float = world_point.length()
	if point_radius <= 1.0:
		return out
	var body_radius: float = maxf(float(body.get(&"radius_m")), 1.0)
	var direction: Vector3 = world_point.normalized().to_v3()
	var point_altitude_m: float = point_radius - body_radius
	var features_value: Variant = water.get(&"authored_features")
	if not (features_value is Array):
		return out
	for feature_value: Variant in features_value as Array:
		var feature: Resource = feature_value as Resource
		if feature == null or not bool(feature.get(&"enabled")):
			continue
		var match: Dictionary
		if int(feature.get(&"feature_type")) == 1:
			match = _query_river(feature, direction, point_altitude_m, body_radius)
		else:
			match = _query_lake(feature, direction, point_altitude_m, body_radius)
		if not match.is_empty():
			_apply_wave_state(match, water, direction, body_radius)
			out.append(match)
	return out


func contains_render_point(render_point: Vector3) -> bool:
	return not query_render_point(render_point).is_empty()


func contains_world_point(world_point: Vec3D) -> bool:
	return not query_world_point(world_point).is_empty()


func _query_lake(feature: Resource, direction: Vector3, point_altitude_m: float,
		body_radius: float) -> Dictionary:
	var polygon_value: Variant = feature.get(&"lake_polygon_body_m")
	if not (polygon_value is PackedVector3Array):
		return {}
	var polygon: PackedVector3Array = polygon_value as PackedVector3Array
	if polygon.size() < 3:
		return {}

	var center_dir := Vector3.ZERO
	for point: Vector3 in polygon:
		if point.length_squared() > 1.0:
			center_dir += point.normalized()
	if center_dir.length_squared() < 1e-8:
		return {}
	center_dir = center_dir.normalized()
	var basis: Array = CubeSphere.tangent_basis(center_dir)
	var right: Vector3 = basis[0]
	var up: Vector3 = basis[1]
	var query_plane: Vector2 = _project_direction(direction, center_dir, right, up, body_radius)
	if not is_finite(query_plane.x) or not is_finite(query_plane.y):
		return {}

	var polygon_plane := PackedVector2Array()
	for point: Vector3 in polygon:
		if point.length_squared() <= 1.0:
			return {}
		var projected: Vector2 = _project_direction(
			point.normalized(), center_dir, right, up, body_radius)
		if not is_finite(projected.x) or not is_finite(projected.y):
			return {}
		polygon_plane.append(projected)
	if not _point_in_polygon(query_plane, polygon_plane):
		return {}

	var edge_distance_m: float = _distance_to_polygon_edges(query_plane, polygon_plane)
	var surface_altitude_m: float = float(feature.get(&"surface_level_m"))
	var depth_m: float = _resolved_water_depth(direction, surface_altitude_m,
		maxf(float(feature.get(&"default_depth_m")), 0.0))
	return _result_dictionary(
		feature,
		0,
		direction,
		point_altitude_m,
		surface_altitude_m,
		depth_m,
		0.0,
		Vector3.ZERO,
		edge_distance_m,
		0.0,
		-1,
		0.0)


func _query_river(feature: Resource, direction: Vector3, point_altitude_m: float,
		body_radius: float) -> Dictionary:
	var segment_count: int = int(feature.call("river_segment_count"))
	if segment_count <= 0:
		return {}
	var best_segment: int = -1
	var best_t: float = 0.0
	var best_arc_m: float = INF

	for segment: int in segment_count:
		var local_t: float = 0.0
		var local_arc_m: float = INF
		var local_index: int = 0
		for index: int in range(RIVER_QUERY_COARSE_STEPS + 1):
			var t: float = float(index) / float(RIVER_QUERY_COARSE_STEPS)
			var arc_m: float = _river_arc_distance(feature, segment, t, direction, body_radius)
			if arc_m < local_arc_m:
				local_arc_m = arc_m
				local_t = t
				local_index = index
		var coarse_step: float = 1.0 / float(RIVER_QUERY_COARSE_STEPS)
		var lo: float = maxf(0.0, float(local_index - 1) * coarse_step)
		var hi: float = minf(1.0, float(local_index + 1) * coarse_step)
		for _iteration: int in RIVER_QUERY_REFINE_STEPS:
			var t0: float = lerpf(lo, hi, 1.0 / 3.0)
			var t1: float = lerpf(lo, hi, 2.0 / 3.0)
			var d0: float = _river_arc_distance(feature, segment, t0, direction, body_radius)
			var d1: float = _river_arc_distance(feature, segment, t1, direction, body_radius)
			if d0 <= d1:
				hi = t1
			else:
				lo = t0
		local_t = (lo + hi) * 0.5
		local_arc_m = _river_arc_distance(feature, segment, local_t, direction, body_radius)
		if local_arc_m < best_arc_m:
			best_arc_m = local_arc_m
			best_segment = segment
			best_t = local_t

	if best_segment < 0:
		return {}
	var width_m: float = maxf(float(feature.call(
		"sample_river_width", best_segment, best_t)), 0.05)
	var half_width_m: float = width_m * 0.5
	if best_arc_m > half_width_m:
		return {}
	var center_position: Vector3 = feature.call("sample_river_segment", best_segment, best_t)
	if center_position.length_squared() <= 1.0:
		return {}
	var surface_altitude_m: float = center_position.length() - body_radius \
		+ float(feature.get(&"surface_level_m"))
	var fallback_depth_m: float = maxf(float(feature.call(
		"sample_river_depth", best_segment, best_t)), 0.0)
	var depth_m: float = _resolved_water_depth(direction, surface_altitude_m, fallback_depth_m)
	var current_m_s: float = maxf(float(feature.call(
		"sample_river_current", best_segment, best_t)), 0.0) \
		* maxf(float(feature.get(&"current_scale")), 0.0)
	var current_dir: Vector3 = _river_tangent(feature, best_segment, best_t, direction)
	var lateral_ratio: float = best_arc_m / maxf(half_width_m, 0.001)
	return _result_dictionary(
		feature,
		1,
		direction,
		point_altitude_m,
		surface_altitude_m,
		depth_m,
		current_m_s,
		current_dir * current_m_s,
		half_width_m - best_arc_m,
		lateral_ratio,
		best_segment,
		best_t)


func _result_dictionary(feature: Resource, feature_type: int, direction: Vector3,
		point_altitude_m: float, surface_altitude_m: float, depth_m: float,
		current_m_s: float, current_velocity: Vector3, edge_distance_m: float,
		lateral_ratio: float, segment: int, segment_t: float) -> Dictionary:
	return {
		"feature_id": String(feature.get(&"feature_id")),
		"display_name": String(feature.get(&"display_name")),
		"feature_type": feature_type,
		"direction": direction,
		"point_altitude_m": point_altitude_m,
		"mean_surface_altitude_m": surface_altitude_m,
		"surface_altitude_m": surface_altitude_m,
		"submergence_m": surface_altitude_m - point_altitude_m,
		"depth_m": depth_m,
		"current_m_s": current_m_s,
		"current_velocity": current_velocity,
		"surface_normal": direction,
		"wave_height_m": 0.0,
		"wave_slope": 0.0,
		"edge_distance_m": edge_distance_m,
		"lateral_ratio": lateral_ratio,
		"river_segment": segment,
		"river_t": segment_t,
		"wave_amplitude_scale": maxf(float(feature.get(&"wave_amplitude_scale")), 0.0),
		"wave_frequency_scale": maxf(float(feature.get(&"wave_frequency_scale")), 0.0),
		"clipmap_simulation_enabled": bool(feature.get(&"clipmap_simulation_enabled")),
	}


func _apply_wave_state(sample: Dictionary, water: Resource, direction: Vector3,
		body_radius: float) -> void:
	var feature_type: int = int(sample.get("feature_type", 0))
	var feature_amplitude: float = maxf(float(sample.get("wave_amplitude_scale", 1.0)), 0.0)
	var global_amplitude: float = maxf(float(water.get(&"wave_amplitude_scale")), 0.0)
	var amplitude_m: float = LAKE_WAVE_AMPLITUDE_M * feature_amplitude * global_amplitude
	if feature_type == 1:
		amplitude_m *= RIVER_WAVE_AMPLITUDE_SCALE
	if amplitude_m <= 1e-7:
		sample["surface_normal"] = direction
		return

	var feature_frequency: float = maxf(float(sample.get("wave_frequency_scale", 1.0)), 0.001)
	var global_frequency: float = maxf(float(water.get(&"wave_frequency_scale")), 0.001)
	var frequency_scale: float = feature_frequency * global_frequency
	var base_wavelength_m: float = RIVER_WAVELENGTH_M if feature_type == 1 else LAKE_WAVELENGTH_M
	var wavelength_m: float = base_wavelength_m / maxf(frequency_scale, 0.001)
	var k: float = TAU / maxf(wavelength_m, 0.25)
	var current_speed: float = maxf(float(sample.get("current_m_s", 0.0)), 0.0)
	var time_speed: float = 1.8 + current_speed * 0.22 if feature_type == 1 else 1.15
	var mean_altitude_m: float = float(sample.get("mean_surface_altitude_m", 0.0))
	# Match the tiny render-only surface bias in the phase coordinate without
	# treating that anti-z-fighting offset as physical water elevation.
	var render_bias_m: float = RIVER_SURFACE_BIAS_M if feature_type == 1 else LAKE_SURFACE_BIAS_M
	var phase_point: Vector3 = direction * (body_radius + mean_altitude_m + render_bias_m)
	var phase_a: float = phase_point.dot(WAVE_AXIS_A) * k + _author_time_s * time_speed
	var phase_b: float = phase_point.dot(WAVE_AXIS_B) * (k * 1.73) \
		- _author_time_s * (time_speed * 0.73)
	var wave_height_m: float = (sin(phase_a) * 0.72 + sin(phase_b) * 0.28) * amplitude_m
	var tangent_a: Vector3 = _safe_wave_tangent(direction, WAVE_AXIS_A)
	var tangent_b: Vector3 = _safe_wave_tangent(direction, WAVE_AXIS_B)
	var slope_a: float = cos(phase_a) * amplitude_m * k * 0.72
	var slope_b: float = cos(phase_b) * amplitude_m * (k * 1.73) * 0.28
	var surface_normal: Vector3 = (direction - tangent_a * slope_a - tangent_b * slope_b).normalized()

	var base_depth_m: float = float(sample.get("depth_m", 0.0))
	var surface_altitude_m: float = mean_altitude_m + wave_height_m
	sample["wave_height_m"] = wave_height_m
	sample["wave_slope"] = absf(slope_a) + absf(slope_b)
	sample["surface_normal"] = surface_normal
	sample["surface_altitude_m"] = surface_altitude_m
	sample["submergence_m"] = surface_altitude_m - float(sample.get("point_altitude_m", 0.0))
	sample["depth_m"] = maxf(base_depth_m + wave_height_m, 0.0)


func _safe_wave_tangent(up: Vector3, axis: Vector3) -> Vector3:
	var tangent: Vector3 = axis - up * axis.dot(up)
	if tangent.length_squared() < 1e-8:
		var fallback: Vector3 = Vector3.UP if absf(up.y) < 0.92 else Vector3.RIGHT
		tangent = fallback - up * fallback.dot(up)
	return tangent.normalized()


func _resolved_water_depth(direction: Vector3, surface_altitude_m: float,
		fallback_depth_m: float) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return fallback_depth_m
	var coarse_height: float = TerrainContactSampler.coarse_height(direction)
	var ground_height: float = TerrainContactSampler.contact_height(direction, coarse_height)
	return maxf(surface_altitude_m - ground_height, 0.0)


func _river_arc_distance(feature: Resource, segment: int, t: float,
		direction: Vector3, body_radius: float) -> float:
	var position: Vector3 = feature.call("sample_river_segment", segment, t)
	if position.length_squared() <= 1.0:
		return INF
	return acos(clampf(position.normalized().dot(direction), -1.0, 1.0)) * body_radius


func _river_tangent(feature: Resource, segment: int, t: float,
		up_direction: Vector3) -> Vector3:
	var lo: float = maxf(0.0, t - RIVER_QUERY_DT)
	var hi: float = minf(1.0, t + RIVER_QUERY_DT)
	if hi - lo < 1e-6:
		return Vector3.ZERO
	var p0: Vector3 = feature.call("sample_river_segment", segment, lo)
	var p1: Vector3 = feature.call("sample_river_segment", segment, hi)
	var tangent: Vector3 = p1 - p0
	tangent -= up_direction * tangent.dot(up_direction)
	return tangent.normalized() if tangent.length_squared() > 1e-10 else Vector3.ZERO


func _project_direction(direction: Vector3, center: Vector3, right: Vector3,
		up: Vector3, radius: float) -> Vector2:
	var d: Vector3 = direction.normalized()
	var denom: float = d.dot(center)
	if denom <= 0.01:
		return Vector2(INF, INF)
	return Vector2(d.dot(right), d.dot(up)) / denom * radius


func _point_in_polygon(point: Vector2, polygon: PackedVector2Array) -> bool:
	if polygon.size() < 3:
		return false
	var inside := false
	var j: int = polygon.size() - 1
	for i: int in polygon.size():
		var a: Vector2 = polygon[i]
		var b: Vector2 = polygon[j]
		var crosses: bool = (a.y > point.y) != (b.y > point.y)
		if crosses:
			var denom: float = b.y - a.y
			if absf(denom) > 1e-12:
				var x_at_y: float = (b.x - a.x) * (point.y - a.y) / denom + a.x
				if point.x < x_at_y:
					inside = not inside
		j = i
	return inside


func _distance_to_polygon_edges(point: Vector2, polygon: PackedVector2Array) -> float:
	if polygon.size() < 2:
		return 0.0
	var nearest := INF
	var previous: Vector2 = polygon[polygon.size() - 1]
	for current: Vector2 in polygon:
		nearest = minf(nearest, _point_segment_distance(point, previous, current))
		previous = current
	return nearest


func _point_segment_distance(point: Vector2, a: Vector2, b: Vector2) -> float:
	var ab: Vector2 = b - a
	var denom: float = ab.length_squared()
	if denom <= 1e-12:
		return point.distance_to(a)
	var t: float = clampf((point - a).dot(ab) / denom, 0.0, 1.0)
	return point.distance_to(a + ab * t)
