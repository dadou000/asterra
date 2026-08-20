class_name FastPlanetTerrain
extends PlanetTerrain
## Coverage-first terrain streamer without temporary low-resolution geometry.
##
## Every active quadtree branch keeps its already-built parent mesh resident while
## finer children are generated. A parent is hidden only after all four child
## regions have real meshes, and it is retained as an instant fallback instead of
## being freed. This means worker timing can reduce detail, but cannot expose a
## hole in the planet.
##
## Expensive chunk work is queued by player relevance, bounded to a small number
## of concurrent worker jobs, and biased toward the player's direction of travel.

const MAX_CONCURRENT_BUILDS_CAP := 12
const INSTALL_BUDGET_USEC := 3500
const PREFETCH_SECONDS := 1.35
const PREFETCH_MAX_METRES := 120000.0
const TELEPORT_THRESHOLD_METRES := 250000.0

## Queue entries: { node, priority, missing }
var _request_queue: Array = []
## TerrainDetail owns four FastNoiseLite generators. Keep one per concurrently
## running worker and recycle it when that worker finishes.
var _free_details: Array[TerrainDetail] = []

var _max_concurrent_builds: int = 8
var _have_observer := false
var _motion_dir := Vec3D.new(0.0, 0.0, 0.0)
var _motion_speed := 0.0


func _ready() -> void:
	super._ready()
	# Leave at least two logical processors to the main/render/physics side while
	# still scaling up on high-core-count CPUs.
	_max_concurrent_builds = clampi(OS.get_processor_count() - 2, 2, MAX_CONCURRENT_BUILDS_CAP)
	# Mesh creation/upload stays on the main thread and is time-budgeted below.
	max_builds_per_frame = 24


func build_roots() -> void:
	_request_queue.clear()
	_have_observer = false
	_motion_speed = 0.0

	super.build_roots()

	# Build the six actual root chunks synchronously before streaming starts.
	# There are only six of them, and using the normal chunk resolution avoids the
	# huge faceted/warped temporary meshes caused by the old 8x8 coverage scheme.
	# From this point onward there is always a valid ancestor mesh to show.
	var detail: TerrainDetail = Planet.make_detail()
	for value in roots:
		var node: PlanetTerrain.QuadNode = value
		var data: Dictionary = ChunkBuilder.build(
			node.face, node.u0, node.v0, node.size,
			cfg.chunk_grid, detail, {}, false)
		super._instantiate(node, data)


func set_observer(world_pos: Vec3D) -> void:
	# Estimate motion from the observer itself so the terrain system remains
	# independent of the player/controller implementation.
	if _have_observer:
		var delta: Vec3D = world_pos.sub(observer)
		var moved: float = delta.length()
		if moved > 1e-4 and moved < TELEPORT_THRESHOLD_METRES:
			_motion_dir = delta.mul(1.0 / moved)
			var sample_dt: float = maxf(_dt, 1.0 / 240.0)
			var instant_speed: float = moved / sample_dt
			_motion_speed = lerpf(_motion_speed, instant_speed, 0.35)
		else:
			_motion_speed = 0.0
	else:
		_have_observer = true
	observer = world_pos


func _process(dt: float) -> void:
	if not Planet.ready_state or roots.is_empty():
		return
	_dt = dt

	# Completed chunks are installed before traversal so they can immediately
	# participate in a handoff during this frame.
	_drain_results()

	_stats["nodes"] = 0
	_stats["culled"] = 0
	_stats["handoffs"] = 0
	var r_obs: float = maxf(observer.length(), cfg.planet_radius + 1.0)
	_obs_dir = observer.normalized().to_v3()
	_horizon_angle = acos(clampf(cfg.planet_radius / r_obs, -1.0, 1.0)) \
		+ acos(clampf(cfg.planet_radius / (cfg.planet_radius + MAX_TERRAIN_HEIGHT), -1.0, 1.0))
	_stats["horizon_deg"] = rad_to_deg(_horizon_angle)
	_stats["horizon_km"] = _horizon_angle * cfg.planet_radius / 1000.0
	_ground_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_water_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)

	_morph_lock = 0
	for value in roots:
		var root: PlanetTerrain.QuadNode = value
		_update(root, false, 0.0)

	_pump_requests()
	_stats["queued"] = _request_queue.size()
	_stats["in_flight"] = _in_flight


func _update(node: PlanetTerrain.QuadNode, hidden: bool, morph: float) -> void:
	var node_ang: float = node.size * (PI * 0.25) * 0.78
	var cull_at: float = _horizon_angle + node_ang
	if node.chunk != null or not node.is_leaf():
		cull_at *= HORIZON_CULL_HYSTERESIS

	# Avoid acos/angle_to per node. When a subtree leaves the horizon, collapse
	# its children but KEEP this node's own mesh resident as the fallback for the
	# instant the player turns back toward it.
	if node.center_dir.dot(_obs_dir) < cos(cull_at):
		_cancel_queued(node)
		if not node.is_leaf():
			_collapse(node)
			node.fine_vis = 0.0
		if node.chunk != null:
			node.drawn = false
			node.chunk.visible = false
			_set_body_enabled(node, false)
		_stats["culled"] += 1
		return
	_stats["nodes"] += 1

	var center_dist: float = _distance_to_node(node, observer)
	var dist: float = center_dist - node.arc * 0.6
	var want_split: bool
	if forced_depth >= 0:
		want_split = node.depth < forced_depth and int(_stats["nodes"]) < FORCED_NODE_BUDGET
	else:
		want_split = node.depth < cfg.quadtree_max_depth \
			and dist < node.arc * cfg.lod_split_factor

	if node.is_leaf():
		if want_split:
			# Critical coverage rule: never descend through a region which does not
			# already have a real mesh at this level. The nearest resident ancestor
			# remains visible until this request completes.
			if node.chunk == null:
				if node.state == 0 or node.dirty:
					_request(node)
				return
			# If this leaf is being rebuilt after an edit, finish that replacement
			# before using it as the fallback for a deeper split.
			if node.state == 1:
				_apply_chunk(node, not hidden, morph)
				return
			if _morph_lock == 0:
				_split(node)
			else:
				_apply_chunk(node, not hidden, morph)
				return
		else:
			if node.chunk == null:
				if node.state == 0 or node.dirty:
					_request(node)
			elif node.dirty and node.state != 1:
				_request(node)
			_apply_chunk(node, not hidden, morph)
			return

	# Non-leaf. Every child is allowed to request/build in parallel, but this
	# parent remains the visible fallback until every child region is covered.
	if want_split:
		var covered: bool = true
		for value in node.children:
			var child: PlanetTerrain.QuadNode = value
			if not _subtree_covered(child):
				covered = false
				break

		if not covered:
			node.fine_vis = 0.0
		else:
			if node.fine_vis <= 0.0 and not _subtree_morphing(node):
				node.fine_vis = MORPH_EPSILON
			else:
				node.fine_vis = move_toward(node.fine_vis, 1.0, _dt / LOD_MORPH_TIME)
	else:
		# Refinement is no longer needed. The retained parent is already available,
		# so morph children back into it and only then free the descendants.
		if node.chunk == null:
			# This should be rare (for example a legacy tree created before this
			# streamer took ownership), but repairing the fallback is safer than
			# exposing a hole.
			if node.state == 0 or node.dirty:
				_request(node)
		else:
			node.fine_vis = move_toward(node.fine_vis, 0.0, _dt / LOD_MORPH_TIME)
			if node.fine_vis <= 0.0 and not _subtree_morphing(node):
				_collapse(node)
				_apply_chunk(node, not hidden, morph)
				return

	var showing_self: bool = node.chunk != null and node.fine_vis <= 0.0
	if node.chunk != null or node.fine_vis < 1.0:
		_stats["handoffs"] += 1
	_apply_chunk(node, not hidden and showing_self, morph)

	var child_morph: float = 1.0 - node.fine_vis
	var lock: bool = node.fine_vis > 0.0 and node.fine_vis < 1.0
	if lock:
		_morph_lock += 1
	for value in node.children:
		var child: PlanetTerrain.QuadNode = value
		_update(child, hidden or showing_self, child_morph)
	if lock:
		_morph_lock -= 1


func _subtree_covered(node: PlanetTerrain.QuadNode) -> bool:
	# Any resident node is enough to cover its entire region, regardless of how
	# much deeper refinement underneath it is still pending.
	if node.chunk != null:
		return true
	if node.is_leaf():
		return false
	for value in node.children:
		var child: PlanetTerrain.QuadNode = value
		if not _subtree_covered(child):
			return false
	return true


func _request(node: PlanetTerrain.QuadNode) -> void:
	if _shutting_down or node.state == 1:
		return

	var missing: bool = node.chunk == null
	# A resident chunk may still be rebuilt because terrain edits marked it dirty.
	if node.state == 2 and not node.dirty:
		return

	node.state = 1
	node.dirty = false
	node.task_id = -1
	_request_queue.append({
		"node": node,
		"missing": missing,
		"priority": 0.0,
	})


func _cancel_queued(node: PlanetTerrain.QuadNode) -> void:
	if node.state != 1 or node.task_id >= 0:
		return
	# The queue entry is lazily discarded by _pump_requests().
	node.state = 2 if node.chunk != null else 0


func _pump_requests() -> void:
	if _request_queue.is_empty() or _shutting_down:
		return

	# Re-rank every waiting request against the latest observer position. Turning
	# or accelerating therefore changes the next job immediately instead of after
	# an old FIFO backlog has drained.
	var kept: Array = []
	for value in _request_queue:
		var entry: Dictionary = value
		var node: PlanetTerrain.QuadNode = entry["node"]
		if not is_instance_valid(node):
			continue
		if node.abandoned or node.state != 1 or node.task_id >= 0:
			continue
		entry["priority"] = _priority(node, bool(entry["missing"]))
		kept.append(entry)
	_request_queue = kept

	# Descending order lets pop_back() take the smallest (best) score cheaply.
	_request_queue.sort_custom(func(a, b):
		return float(a["priority"]) > float(b["priority"]))

	while _in_flight < _max_concurrent_builds and not _request_queue.is_empty():
		var entry: Dictionary = _request_queue.pop_back()
		var node: PlanetTerrain.QuadNode = entry["node"]
		if node.abandoned or node.state != 1 or node.task_id >= 0:
			continue
		_start_request(entry)


func _start_request(entry: Dictionary) -> void:
	var node: PlanetTerrain.QuadNode = entry["node"]
	var band: int = band_for_depth(node.depth)
	var want_collision: bool = node.depth >= cfg.collision_depth
	var ang: float = node.size * (PI * 0.25) * 1.5
	var snap: Dictionary = Deltas.snapshot_for_bounds(node.center_dir, ang) if band >= Band.LOCAL else {}

	var detail: TerrainDetail
	if _free_details.is_empty():
		detail = Planet.make_detail()
	else:
		detail = _free_details.pop_back()

	var task := func() -> void:
		var data: Dictionary = ChunkBuilder.build(
			node.face, node.u0, node.v0, node.size,
			cfg.chunk_grid, detail, snap, want_collision)
		_res_mutex.lock()
		_results.append({"node": node, "data": data, "detail": detail})
		_res_mutex.unlock()

	# Missing geometry gets the worker pool's high-priority lane. Refinements of
	# already-resident/edited chunks stay normal priority.
	var high_priority: bool = bool(entry["missing"]) and float(entry["priority"]) < 2.0
	var tid: int = WorkerThreadPool.add_task(task, high_priority, "asterra_chunk")
	node.task_id = tid
	_pending_tasks.append(tid)
	_in_flight += 1


func _drain_results() -> void:
	var built: int = 0
	var start_usec: int = Time.get_ticks_usec()
	while built < max_builds_per_frame:
		if built > 0 and Time.get_ticks_usec() - start_usec >= INSTALL_BUDGET_USEC:
			break

		_res_mutex.lock()
		if _results.is_empty():
			_res_mutex.unlock()
			break

		# Finished workers can complete out of order. Install whichever completed
		# chunk is most relevant now rather than whichever thread happened to win.
		var best_i: int = 0
		var best_p: float = INF
		for i in _results.size():
			var candidate: Dictionary = _results[i]
			var candidate_node: PlanetTerrain.QuadNode = candidate["node"]
			var p: float = _priority(candidate_node, candidate_node.chunk == null)
			if p < best_p:
				best_p = p
				best_i = i
		var item: Dictionary = _results[best_i]
		_results.remove_at(best_i)
		_res_mutex.unlock()

		var node: PlanetTerrain.QuadNode = item["node"]
		var tid: int = node.task_id
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
			_pending_tasks.erase(tid)
			node.task_id = -1
		_in_flight = maxi(0, _in_flight - 1)
		_free_details.append(item["detail"])

		if not node.abandoned:
			super._instantiate(node, item["data"])
		else:
			node.state = 0
		built += 1


func _priority(node: PlanetTerrain.QuadNode, missing: bool) -> float:
	var arc: float = maxf(node.arc, 1.0)
	var current: float = _distance_to_node(node, observer) / arc
	var score: float = current

	if _motion_speed > 0.5 and _motion_dir.length_sq() > 0.5:
		var ahead: float = minf(_motion_speed * PREFETCH_SECONDS, PREFETCH_MAX_METRES)
		var predicted: Vec3D = observer.add(_motion_dir.mul(ahead))
		var future: float = _distance_to_node(node, predicted) / arc
		score = minf(score, future * 0.82)

	# A missing child determines whether a parent is allowed to hand off, so it is
	# structurally more important than rebuilding an already visible chunk.
	if missing:
		score -= 2.0
	# Slight shallow-level bias prevents a deep request from outrunning all four
	# siblings of the level that is currently supplying coverage.
	score += float(node.depth) * 0.025
	return score


func _distance_to_node(node: PlanetTerrain.QuadNode, from: Vec3D) -> float:
	var best: float = 1e30
	for value in node.lod_centers:
		var p: Vec3D = value
		best = minf(best, from.distance_to(p))
	return best
