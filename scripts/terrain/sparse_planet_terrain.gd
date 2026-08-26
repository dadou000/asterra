class_name SparsePlanetTerrain
extends PlanetTerrain
## Non-visual terrain compatibility shell.
##
## Visual terrain is owned exclusively by the GroundGeometryClipmap autoload.
## This node keeps only the Phase-1 harness API plus observer/horizon statistics.
##
## IMPORTANT: no CPU terrain collision streamer is created here. Runtime detailed
## terrain is GPU-defined; player/vehicle contact must use GPU height/contact
## queries rather than rebuilding procedural ConcavePolygon tiles on worker threads.


func _ready() -> void:
	process_priority = 10
	add_to_group("sparse_planet_terrain")
	Planet.world_ready.connect(_on_sparse_world_ready)
	Planet.coast_profile_changed.connect(_on_sparse_profile_changed)
	if Planet.ready_state and Planet.cfg != null:
		build_roots()


## Compatibility name retained for the Phase-1 harness. There are no visual roots
## and no collision tiles to rebuild. TerrainDebug may still call this after static
## topology changes, so forward that request only to the GPU clipmap.
func build_roots() -> void:
	cfg = Planet.cfg
	roots.clear()
	if cfg == null:
		return
	var clipmap: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if clipmap != null and clipmap.has_method("rebuild_static_topology"):
		clipmap.rebuild_static_topology()
	_stats["chunks"] = 0
	_stats["nodes"] = 0
	_stats["queued"] = 0
	_stats["in_flight"] = 0
	_stats["culled"] = 0
	_stats["handoffs"] = 0
	_stats["cpu_collision_streamer"] = false


func set_observer(world_pos: Vec3D) -> void:
	observer = world_pos
	if cfg == null:
		return
	var radius: float = cfg.planet_radius
	var r_obs: float = maxf(observer.length(), radius + 1.0)
	_obs_dir = observer.normalized().to_v3()
	_horizon_angle = acos(clampf(radius / r_obs, -1.0, 1.0))
	_stats["horizon_deg"] = rad_to_deg(_horizon_angle)
	_stats["horizon_km"] = _horizon_angle * radius / 1000.0
	_stats["cpu_collision_streamer"] = false


func stats() -> Dictionary:
	return _stats.duplicate()


func debug_materials() -> Array:
	# TerrainDebug controls the spherical clipmap directly for wireframe/height.
	return []


func _on_sparse_world_ready(_fields: PlanetFields) -> void:
	build_roots()


func _on_sparse_profile_changed() -> void:
	build_roots()
