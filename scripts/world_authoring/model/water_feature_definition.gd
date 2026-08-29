class_name WaterFeatureDefinition
extends Resource
## Authored hydrology geometry. Lakes use a freeform body-space polygon; rivers
## use cubic 3D Bézier knots. Both carry clipmap/simulation parameters so the
## runtime can rasterize them into the same water field rather than spawning an
## unrelated mesh-water system.

enum FeatureType {
	LAKE,
	RIVER,
}

@export var feature_id: String = ""
@export var display_name: String = "Water Feature"
@export_enum("Lake", "River") var feature_type: int = FeatureType.LAKE
@export var enabled: bool = true
@export var clipmap_simulation_enabled: bool = true
@export var surface_level_m: float = 0.0
@export_range(0.0, 100000.0, 0.05) var shore_falloff_m: float = 3.0
@export_range(0.0, 100000.0, 0.05) var default_depth_m: float = 5.0
@export_range(0.0, 1000.0, 0.001) var wave_amplitude_scale: float = 1.0
@export_range(0.001, 1000.0, 0.001) var wave_frequency_scale: float = 1.0
@export_range(0.0, 1000.0, 0.001) var current_scale: float = 1.0
@export var lake_polygon_body_m: PackedVector3Array = PackedVector3Array()
@export var river_knots: Array[Dictionary] = []

func ensure_valid() -> void:
	if feature_id.is_empty():
		feature_id = make_feature_id(display_name)
	shore_falloff_m = maxf(0.0, shore_falloff_m)
	default_depth_m = maxf(0.0, default_depth_m)
	wave_amplitude_scale = maxf(0.0, wave_amplitude_scale)
	wave_frequency_scale = maxf(0.001, wave_frequency_scale)
	current_scale = maxf(0.0, current_scale)
	for index: int in river_knots.size():
		var knot: Dictionary = river_knots[index]
		knot["width_m"] = maxf(0.05, float(knot.get("width_m", 10.0)))
		knot["depth_m"] = maxf(0.0, float(knot.get("depth_m", default_depth_m)))
		knot["current_m_s"] = maxf(0.0, float(knot.get("current_m_s", 1.0)))
		river_knots[index] = knot

func add_lake_point(position_body_m: Vector3) -> void:
	lake_polygon_body_m.append(position_body_m)

func set_lake_point(index: int, position_body_m: Vector3) -> bool:
	if index < 0 or index >= lake_polygon_body_m.size():
		return false
	lake_polygon_body_m[index] = position_body_m
	return true

func remove_lake_point(index: int) -> bool:
	if index < 0 or index >= lake_polygon_body_m.size():
		return false
	lake_polygon_body_m.remove_at(index)
	return true

func add_river_knot(position_body_m: Vector3, width_m: float = 10.0, depth_m: float = 2.0, current_m_s: float = 1.0) -> int:
	river_knots.append({
		"position_body_m": position_body_m,
		"handle_in_offset_m": Vector3.ZERO,
		"handle_out_offset_m": Vector3.ZERO,
		"width_m": maxf(0.05, width_m),
		"depth_m": maxf(0.0, depth_m),
		"current_m_s": maxf(0.0, current_m_s),
	})
	return river_knots.size() - 1

func set_river_knot(index: int, knot: Dictionary) -> bool:
	if index < 0 or index >= river_knots.size():
		return false
	river_knots[index] = knot.duplicate(true)
	ensure_valid()
	return true

func remove_river_knot(index: int) -> bool:
	if index < 0 or index >= river_knots.size():
		return false
	river_knots.remove_at(index)
	return true

func river_segment_count() -> int:
	return maxi(0, river_knots.size() - 1)

func sample_river_segment(segment_index: int, t: float) -> Vector3:
	if segment_index < 0 or segment_index >= river_segment_count():
		return Vector3.ZERO
	var a: Dictionary = river_knots[segment_index]
	var b: Dictionary = river_knots[segment_index + 1]
	var p0: Vector3 = a.get("position_body_m", Vector3.ZERO)
	var p1 := p0 + Vector3(a.get("handle_out_offset_m", Vector3.ZERO))
	var p3: Vector3 = b.get("position_body_m", Vector3.ZERO)
	var p2 := p3 + Vector3(b.get("handle_in_offset_m", Vector3.ZERO))
	var u := clampf(t, 0.0, 1.0)
	var inv := 1.0 - u
	return p0 * (inv * inv * inv) + p1 * (3.0 * inv * inv * u) + p2 * (3.0 * inv * u * u) + p3 * (u * u * u)

func sample_river_width(segment_index: int, t: float) -> float:
	return _sample_knot_scalar(segment_index, t, "width_m", 10.0)

func sample_river_depth(segment_index: int, t: float) -> float:
	return _sample_knot_scalar(segment_index, t, "depth_m", default_depth_m)

func sample_river_current(segment_index: int, t: float) -> float:
	return _sample_knot_scalar(segment_index, t, "current_m_s", 1.0) * current_scale

func _sample_knot_scalar(segment_index: int, t: float, key: String, fallback: float) -> float:
	if segment_index < 0 or segment_index >= river_segment_count():
		return fallback
	var a: Dictionary = river_knots[segment_index]
	var b: Dictionary = river_knots[segment_index + 1]
	return lerpf(float(a.get(key, fallback)), float(b.get(key, fallback)), clampf(t, 0.0, 1.0))

static func make_feature_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "water-feature"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]
