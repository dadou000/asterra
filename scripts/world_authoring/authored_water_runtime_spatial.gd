class_name AuthoredWaterRuntimeSpatial
extends "res://scripts/world_authoring/authored_water_runtime_query.gd"
## Spatially-pruned authored-water gameplay queries.
##
## Detailed lake point-in-polygon tests and river Bezier minimization are only
## evaluated after a conservative spherical bounding-cap test. The cap cache is
## rebuilt with the disposable render meshes whenever staged water data changes.
## Missing/degenerate caps deliberately fall back to the exact inherited query so
## this optimization can never make a valid feature disappear.

const QUERY_CAP_SAFETY_M: float = 64.0
const RIVER_CAP_SAMPLES_PER_SEGMENT: int = 8

var _query_caps: Dictionary = {}
var _cap_rejects: int = 0
var _cap_accepts: int = 0


func _rebuild() -> void:
	super._rebuild()
	_rebuild_query_caps()


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
		if not _feature_cap_contains(feature, direction):
			_cap_rejects += 1
			continue
		_cap_accepts += 1
		var match: Dictionary
		if int(feature.get(&"feature_type")) == 1:
			match = _query_river(feature, direction, point_altitude_m, body_radius)
		else:
			match = _query_lake(feature, direction, point_altitude_m, body_radius)
		if not match.is_empty():
			_apply_wave_state(match, water, direction, body_radius)
			out.append(match)
	return out


func _rebuild_query_caps() -> void:
	_query_caps.clear()
	if _session == null:
		return
	var body: Resource = _session.active_body() as Resource
	var water: Resource = _session.active_water_profile() as Resource
	if body == null or water == null:
		return
	var body_radius: float = maxf(float(body.get(&"radius_m")), 1.0)
	var features_value: Variant = water.get(&"authored_features")
	if not (features_value is Array):
		return
	for feature_value: Variant in features_value as Array:
		var feature: Resource = feature_value as Resource
		if feature == null or not bool(feature.get(&"enabled")):
			continue
		var directions: Array[Vector3] = []
		var margin_m: float = QUERY_CAP_SAFETY_M
		if int(feature.get(&"feature_type")) == 1:
			var segment_count: int = int(feature.call("river_segment_count"))
			for segment: int in segment_count:
				for sample_index: int in range(RIVER_CAP_SAMPLES_PER_SEGMENT + 1):
					if segment > 0 and sample_index == 0:
						continue
					var t: float = float(sample_index) / float(RIVER_CAP_SAMPLES_PER_SEGMENT)
					var point: Vector3 = feature.call("sample_river_segment", segment, t)
					if point.length_squared() > 1.0:
						directions.append(point.normalized())
					margin_m = maxf(margin_m,
						maxf(float(feature.call("sample_river_width", segment, t)), 0.0) * 0.5
						+ QUERY_CAP_SAFETY_M)
		else:
			var polygon_value: Variant = feature.get(&"lake_polygon_body_m")
			if polygon_value is PackedVector3Array:
				for point: Vector3 in polygon_value as PackedVector3Array:
					if point.length_squared() > 1.0:
						directions.append(point.normalized())
		if directions.is_empty():
			continue
		var center_sum := Vector3.ZERO
		for direction: Vector3 in directions:
			center_sum += direction
		if center_sum.length_squared() < 1e-10:
			continue
		var center_dir: Vector3 = center_sum.normalized()
		var max_angle: float = 0.0
		for direction: Vector3 in directions:
			max_angle = maxf(max_angle,
				acos(clampf(center_dir.dot(direction), -1.0, 1.0)))
		max_angle = minf(PI, max_angle + margin_m / body_radius)
		_query_caps[String(feature.get(&"feature_id"))] = {
			"center_dir": center_dir,
			"cos_max_angle": cos(max_angle),
			"max_angle_rad": max_angle,
		}


func _feature_cap_contains(feature: Resource, direction: Vector3) -> bool:
	var feature_id: String = String(feature.get(&"feature_id"))
	if not _query_caps.has(feature_id):
		return true
	var cap: Dictionary = _query_caps[feature_id]
	var center_dir: Vector3 = cap.get("center_dir", Vector3.ZERO)
	if center_dir.length_squared() < 0.99:
		return true
	return center_dir.dot(direction) >= float(cap.get("cos_max_angle", -1.0))


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["query_caps"] = _query_caps.size()
	out["query_cap_rejects"] = _cap_rejects
	out["query_cap_accepts"] = _cap_accepts
	out["query_spatial_pruning"] = true
	return out
