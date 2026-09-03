extends "res://scripts/world_authoring/world_authoring_editor_live_phase46_core.gd"
## Public Phase 46 activation wrapper.
##
## The viewport tool core can be instantiated by deterministic/headless geometry
## tests before it is attached to a SceneTree. Camera discovery therefore has to be
## null-safe, and purely spherical Radial/Ring/Region handles must not depend on a
## camera at all. Latitude/Longitude bands use the camera only to choose a visible
## point along their otherwise-global edge.


func _phase46_has_live_camera() -> bool:
	if _camera != null and is_instance_valid(_camera):
		return true
	if _player == null and _world_host != null:
		_player = _world_host.get("player") as Node
	if _player != null:
		_camera = _player.get("camera") as Camera3D
		if _camera != null:
			return true
	if not is_inside_tree():
		return false
	var viewport: Viewport = get_viewport()
	if viewport == null:
		return false
	_camera = viewport.get_camera_3d()
	return _camera != null


func _phase46_handle_directions(config: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL:
			var center := _phase46_direction_from_lat_lon(
				float(config.get("center_latitude_deg", 0.0)),
				float(config.get("center_longitude_deg", 0.0)))
			out["center"] = center
			out["radius"] = _phase46_offset_direction(
				center, float(config.get("radius_deg", 15.0)))
		PHASE46_GUIDED.AREA_RING:
			var center := _phase46_direction_from_lat_lon(
				float(config.get("center_latitude_deg", 0.0)),
				float(config.get("center_longitude_deg", 0.0)))
			out["center"] = center
			out["outer_radius"] = _phase46_offset_direction(
				center, float(config.get("outer_radius_deg", 20.0)), 0.0)
			out["inner_radius"] = _phase46_offset_direction(
				center, float(config.get("inner_radius_deg", 8.0)), PI * 0.5)
		PHASE46_GUIDED.AREA_REGION:
			var west: float = float(config.get("west_deg", -45.0))
			var east: float = float(config.get("east_deg", 45.0))
			var center_lon := _phase46_wrap_longitude(
				west + _phase46_eastward_span_deg(west, east) * 0.5)
			var center_lat := (float(config.get("south_deg", -30.0))
				+ float(config.get("north_deg", 30.0))) * 0.5
			out["center"] = _phase46_direction_from_lat_lon(center_lat, center_lon)
			out["north"] = _phase46_direction_from_lat_lon(
				float(config.get("north_deg", 30.0)), center_lon)
			out["south"] = _phase46_direction_from_lat_lon(
				float(config.get("south_deg", -30.0)), center_lon)
			out["west"] = _phase46_direction_from_lat_lon(center_lat, west)
			out["east"] = _phase46_direction_from_lat_lon(center_lat, east)
		PHASE46_GUIDED.AREA_LATITUDE:
			var facing: Vector2 = _phase46_camera_facing_lat_lon()
			var center_lat := (float(config.get("south_deg", -30.0))
				+ float(config.get("north_deg", 30.0))) * 0.5
			out["center"] = _phase46_direction_from_lat_lon(center_lat, facing.y)
			out["north"] = _phase46_direction_from_lat_lon(
				float(config.get("north_deg", 30.0)), facing.y)
			out["south"] = _phase46_direction_from_lat_lon(
				float(config.get("south_deg", -30.0)), facing.y)
		PHASE46_GUIDED.AREA_LONGITUDE:
			var facing: Vector2 = _phase46_camera_facing_lat_lon()
			var west: float = float(config.get("west_deg", -45.0))
			var east: float = float(config.get("east_deg", 45.0))
			var center_lon := _phase46_wrap_longitude(
				west + _phase46_eastward_span_deg(west, east) * 0.5)
			out["center"] = _phase46_direction_from_lat_lon(facing.x, center_lon)
			out["west"] = _phase46_direction_from_lat_lon(facing.x, west)
			out["east"] = _phase46_direction_from_lat_lon(facing.x, east)
	return out
