class_name PlanetStudioMapViewSchema
extends "res://scripts/world_authoring/planet_studio_map_view.gd"
## Pre-0.1.0 WaterAuthoringProfile stores lakes/rivers in authored_features.

func _draw_water_overlays() -> void:
	var water: Resource = session.call("active_water_profile") as Resource
	if water == null:
		return
	for feature_value: Variant in water.get(&"authored_features") as Array:
		var feature: Resource = feature_value as Resource
		if feature == null or not bool(feature.get(&"enabled")):
			continue
		var points: PackedVector3Array = feature.get(&"lake_polygon_body_m")
		if points.size() >= 2:
			var polygon := PackedVector2Array()
			for point: Vector3 in points:
				if point.length_squared() > 1.0:
					polygon.append(_dir_to_local(point.normalized()))
			if polygon.size() >= 2:
				_overlay.draw_polyline(polygon, Color(0.20, 0.72, 1.0, 0.95), 2.0)
		var knots: Array = feature.get(&"river_knots") as Array
		if knots.size() >= 2:
			var river := PackedVector2Array()
			for knot_value: Variant in knots:
				var knot: Dictionary = knot_value as Dictionary
				var position: Vector3 = knot.get("position_body_m", Vector3.ZERO)
				if position.length_squared() > 1.0:
					river.append(_dir_to_local(position.normalized()))
			if river.size() >= 2:
				_overlay.draw_polyline(river, Color(0.12, 0.62, 1.0, 0.95), 2.5)
