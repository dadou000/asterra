class_name WorldAuthoringLiveEditorPhase3
extends "res://scripts/world_authoring/world_authoring_editor_live_phase2.gd"
## Phase 3 live authoring feedback.
##
## Phase 2 deliberately batches a complete biome drag into one staged history
## action. This layer mirrors each newly pending stamp to the disposable runtime
## biome preview, so terrain material and ecology respond while the mouse is down
## without reintroducing per-motion preset snapshots/autosaves or repeatedly
## copying the whole growing drag history.
##
## It also resolves the first important live-water authoring semantic: a brand-new
## lake with the untouched 0 m default level adopts the altitude of its first
## terrain click. Rivers keep their path-following semantics and use
## surface_level_m as an explicit vertical offset from the authored Bezier path.

signal biome_preview_stroke_added(layer_id: String, stroke: Dictionary)
signal biome_preview_transient_cleared


func _place_biome_stroke(direction: Vector3, continuous: bool) -> void:
	var before_count: int = _pending_biome_strokes.size()
	super._place_biome_stroke(direction, continuous)
	if _pending_biome_strokes.size() == before_count:
		return
	var newest: Dictionary = _pending_biome_strokes[_pending_biome_strokes.size() - 1]
	biome_preview_stroke_added.emit(_selected_biome_layer_id, newest.duplicate(true))


func _commit_biome_transaction() -> void:
	super._commit_biome_transaction()
	biome_preview_transient_cleared.emit()


func _discard_interactive_transactions() -> void:
	super._discard_interactive_transactions()
	biome_preview_transient_cleared.emit()


func _place_water_point() -> void:
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		_set_status("Select a lake or river before placing control points.")
		return
	var world: Vec3D = _last_hit.get("world") as Vec3D
	if world == null:
		return
	var body_point := Vector3(float(world.x), float(world.y), float(world.z))
	var is_river: bool = _placement_mode == PlacementMode.RIVER
	var seed_lake_level := false
	var clicked_altitude_m := 0.0
	if not is_river:
		var polygon: PackedVector3Array = feature.get(&"lake_polygon_body_m")
		var body: Resource = _session.active_body() as Resource
		var body_radius: float = maxf(float(body.get(&"radius_m")), 1.0) if body != null \
			else maxf(float(Planet.cfg.planet_radius), 1.0)
		clicked_altitude_m = body_point.length() - body_radius
		# Preserve an explicitly entered level. Only the untouched zero default is
		# replaced by the first shoreline/centre click altitude.
		seed_lake_level = polygon.is_empty() \
			and absf(float(feature.get(&"surface_level_m"))) <= 0.001

	_session.stage_action("Place water control point", func() -> void:
		if is_river:
			feature.call("add_river_knot", body_point, 20.0,
				float(feature.get(&"default_depth_m")), 1.0)
		else:
			if seed_lake_level:
				feature.set(&"surface_level_m", clicked_altitude_m)
			feature.call("add_lake_point", body_point)
	, WorldAuthoringSession.ApplyScope.TILES)
	if seed_lake_level:
		_set_status("Added lake control point • surface initialized to %.2f m MSL." % clicked_altitude_m)
	else:
		_set_status("Added %s control point." % ("river" if is_river else "lake"))
