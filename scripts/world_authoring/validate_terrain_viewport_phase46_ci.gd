extends Node
## Phase 46 deterministic regression for viewport geography and guided-graph mutation.

const EDITOR := preload("res://scripts/world_authoring/world_authoring_editor_live_phase46.gd")
const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("TERRAIN_VIEWPORT_PHASE46_FAILED: " + error)
		get_tree().quit(1)
		return
	print("TERRAIN_VIEWPORT_PHASE46_OK: spherical placement, seam-safe region shifts, pole clamping and radius handles mutate the same guided graph")
	get_tree().quit(0)


func _validate() -> String:
	var editor: Node = EDITOR.new() as Node
	if editor == null:
		return "Phase 46 editor could not instantiate"

	# Coordinate convention must stay exactly aligned with Phase 41 masks:
	# 0 longitude = +Z, +90 = +X, with seam-safe wrapping.
	for sample: Vector2 in [
		Vector2(0.0, 0.0),
		Vector2(35.0, 90.0),
		Vector2(-42.5, 179.0),
		Vector2(68.0, -179.0),
	]:
		var direction: Vector3 = editor.call("_phase46_direction_from_lat_lon", sample.x, sample.y)
		var round_trip: Vector2 = editor.call("_phase46_lat_lon_from_direction", direction)
		if absf(round_trip.x - sample.x) > 0.001 or _longitude_error(round_trip.y, sample.y) > 0.001:
			editor.free()
			return "latitude/longitude conversion drifted at %s" % sample

	# Small-circle helper must create true great-circle angular offsets.
	var center: Vector3 = editor.call("_phase46_direction_from_lat_lon", 23.0, 177.0)
	var offset: Vector3 = editor.call("_phase46_offset_direction", center, 12.5, 0.37)
	var angle: float = rad_to_deg(acos(clampf(center.dot(offset), -1.0, 1.0)))
	if absf(angle - 12.5) > 0.001:
		editor.free()
		return "viewport radius handle is not a true spherical angular offset"

	# Radial placement writes the ordinary guided center parameters.
	var radial: Resource = _guided_graph({
		"area_kind":GUIDED.AREA_RADIAL,
		"center_latitude_deg":0.0,
		"center_longitude_deg":0.0,
		"radius_deg":11.0,
	})
	if radial == null:
		editor.free()
		return "could not build radial guided graph"
	var target: Vector3 = editor.call("_phase46_direction_from_lat_lon", 35.0, 179.0)
	editor.call("_phase46_set_center_in_graph", radial, target)
	var radial_config: Dictionary = GUIDED.config_from_graph(radial)
	if absf(float(radial_config.get("center_latitude_deg", 0.0)) - 35.0) > 0.001 \
			or _longitude_error(float(radial_config.get("center_longitude_deg", 0.0)), 179.0) > 0.001 \
			or absf(float(radial_config.get("radius_deg", 0.0)) - 11.0) > 0.001:
		editor.free()
		return "radial placement changed size or failed to move its center"

	# Region placement preserves geographic span while crossing the antimeridian.
	var region: Resource = _guided_graph({
		"area_kind":GUIDED.AREA_REGION,
		"south_deg":-8.0,
		"north_deg":8.0,
		"west_deg":-24.0,
		"east_deg":24.0,
	})
	editor.call("_phase46_set_center_in_graph", region, target)
	var region_config: Dictionary = GUIDED.config_from_graph(region)
	var region_span: float = editor.call("_phase46_eastward_span_deg",
		float(region_config.get("west_deg", 0.0)), float(region_config.get("east_deg", 0.0)))
	var region_center_lon: float = editor.call("_phase46_wrap_longitude",
		float(region_config.get("west_deg", 0.0)) + region_span * 0.5)
	if absf(float(region_config.get("south_deg", 0.0)) - 27.0) > 0.001 \
			or absf(float(region_config.get("north_deg", 0.0)) - 43.0) > 0.001 \
			or absf(region_span - 48.0) > 0.001 \
			or _longitude_error(region_center_lon, 179.0) > 0.001:
		editor.free()
		return "region placement did not preserve span across the ±180 seam"

	# Latitude bands clamp the moved center at the pole instead of shrinking/warping.
	var latitude: Resource = _guided_graph({
		"area_kind":GUIDED.AREA_LATITUDE,
		"south_deg":-30.0,
		"north_deg":30.0,
	})
	var near_pole: Vector3 = editor.call("_phase46_direction_from_lat_lon", 88.0, 10.0)
	editor.call("_phase46_set_center_in_graph", latitude, near_pole)
	var latitude_config: Dictionary = GUIDED.config_from_graph(latitude)
	if absf(float(latitude_config.get("south_deg", 0.0)) - 30.0) > 0.001 \
			or absf(float(latitude_config.get("north_deg", 0.0)) - 90.0) > 0.001:
		editor.free()
		return "latitude-band viewport move did not preserve width at the pole"

	# Longitude bands preserve eastward span through the seam.
	var longitude: Resource = _guided_graph({
		"area_kind":GUIDED.AREA_LONGITUDE,
		"west_deg":-45.0,
		"east_deg":45.0,
	})
	var seam_target: Vector3 = editor.call("_phase46_direction_from_lat_lon", 0.0, -179.0)
	editor.call("_phase46_set_center_in_graph", longitude, seam_target)
	var longitude_config: Dictionary = GUIDED.config_from_graph(longitude)
	var longitude_span: float = editor.call("_phase46_eastward_span_deg",
		float(longitude_config.get("west_deg", 0.0)), float(longitude_config.get("east_deg", 0.0)))
	var longitude_center: float = editor.call("_phase46_wrap_longitude",
		float(longitude_config.get("west_deg", 0.0)) + longitude_span * 0.5)
	if absf(longitude_span - 90.0) > 0.001 or _longitude_error(longitude_center, -179.0) > 0.001:
		editor.free()
		return "longitude-band viewport move did not preserve seam-crossing span"

	# Radius measurement is great-circle distance, not screen or planar distance.
	var radius_target: Vector3 = editor.call("_phase46_offset_direction", target, 9.75, 1.1)
	var measured: float = editor.call("_phase46_angle_from_center", radial_config, radius_target)
	if absf(measured - 9.75) > 0.001:
		editor.free()
		return "radius drag measurement is not great-circle distance"

	# Ring handles must remain independent and exactly angular.
	var ring_config: Dictionary = GUIDED.default_config()
	ring_config["area_kind"] = GUIDED.AREA_RING
	ring_config["center_latitude_deg"] = 20.0
	ring_config["center_longitude_deg"] = -170.0
	ring_config["inner_radius_deg"] = 5.0
	ring_config["outer_radius_deg"] = 12.0
	var handles: Dictionary = editor.call("_phase46_handle_directions", ring_config)
	if not handles.has("center") or not handles.has("inner_radius") or not handles.has("outer_radius"):
		editor.free()
		return "Ring Area is missing viewport center/inner/outer handles"
	var ring_center := Vector3(handles["center"])
	var inner_angle := rad_to_deg(acos(clampf(ring_center.dot(Vector3(handles["inner_radius"])), -1.0, 1.0)))
	var outer_angle := rad_to_deg(acos(clampf(ring_center.dot(Vector3(handles["outer_radius"])), -1.0, 1.0)))
	if absf(inner_angle - 5.0) > 0.001 or absf(outer_angle - 12.0) > 0.001:
		editor.free()
		return "Ring Area viewport handles do not match serialized angular radii"

	editor.free()
	return ""


func _guided_graph(overrides: Dictionary) -> Resource:
	var graph: Resource = GRAPH.new()
	var config: Dictionary = GUIDED.default_config()
	for key: Variant in overrides.keys():
		config[key] = overrides[key]
	if not GUIDED.rebuild(graph, config):
		return null
	return graph


func _longitude_error(a: float, b: float) -> float:
	return absf(fposmod(a - b + 180.0, 360.0) - 180.0)
