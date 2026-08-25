class_name SparsePlanetTerrain
extends PlanetTerrain
## Non-visual terrain compatibility shell.
##
## Visual terrain is owned exclusively by the GroundGeometryClipmap autoload,
## which is now the planet-wide spherical geometry clipmap. This node keeps only
## the harness API, observer/horizon statistics, and CPU collision streamer.
## It creates no visual mesh, quadtree, cube-face batch, or ChunkBuilder work.

var _collision_streamer: TerrainCollisionStreamer


func _ready() -> void:
	process_priority = 10
	add_to_group("sparse_planet_terrain")
	_collision_streamer = TerrainCollisionStreamer.new()
	add_child(_collision_streamer)
	Planet.world_ready.connect(_on_sparse_world_ready)
	Planet.coast_profile_changed.connect(_on_sparse_profile_changed)
	if Planet.ready_state and Planet.cfg != null:
		build_roots()


## Compatibility name retained for the Phase-1 harness. There are no visual roots.
## TerrainDebug also calls this after enabling generated wireframe indices, so ask
## the spherical renderer to recreate only its immutable static topology.
func build_roots() -> void:
	cfg = Planet.cfg
	roots.clear()
	if cfg == null:
		return
	if _collision_streamer != null:
		_collision_streamer.configure(cfg)
	var clipmap: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if clipmap != null and clipmap.has_method("rebuild_static_topology"):
		clipmap.rebuild_static_topology()
	_stats["chunks"] = 0
	_stats["nodes"] = 0
	_stats["queued"] = 0
	_stats["in_flight"] = 0
	_stats["culled"] = 0
	_stats["handoffs"] = 0


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

	if _collision_streamer != null:
		var agl: float = maxf(observer.length() - radius - Planet.macro_height(_obs_dir), 0.0)
		_collision_streamer.set_observer(observer, agl <= 320.0)


func stats() -> Dictionary:
	var out := _stats.duplicate()
	if _collision_streamer == null:
		return out
	var collision := _collision_streamer.stats()
	out["chunks"] = collision["resident"]
	out["nodes"] = collision["entries"]
	out["queued"] = collision["queued"]
	out["in_flight"] = collision["in_flight"]
	out["collision"] = collision
	return out


func debug_materials() -> Array:
	# TerrainDebug controls the spherical clipmap directly for wireframe/height.
	return []


func _on_sparse_world_ready(_fields: PlanetFields) -> void:
	build_roots()


func _on_sparse_profile_changed() -> void:
	build_roots()
