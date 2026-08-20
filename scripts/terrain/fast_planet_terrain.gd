class_name FastPlanetTerrain
extends PlanetTerrain
## Coverage-first terrain streamer.
##
## The original quadtree could descend to the final LOD before any ancestor mesh
## existed, so worker completion order became visible as holes. This streamer
## keeps a cheap coverage mesh at every handoff level, queues all expensive work
## by player relevance, and limits the number of WorkerThreadPool jobs in flight.
## A lower-detail parent is therefore always available while finer children load.

const COVERAGE_GRID := 8
const MAX_CONCURRENT_BUILDS := 8
const INSTALL_BUDGET_USEC := 3500
const PREFETCH_SECONDS := 1.35
const PREFETCH_MAX_METRES := 120000.0
const TELEPORT_THRESHOLD_METRES := 250000.0

## Queue entries are dictionaries:
## { node, coverage, n, priority }
var _request_queue: Array = []
## Node instance id -> true while its resident mesh is only a temporary
## low-resolution coverage mesh.
var _coverage_only: Dictionary = {}
## Node instance id -> bool; true = coverage request, false = full request.
var _request_kind: Dictionary = {}
## TerrainDetail contains four FastNoiseLite generators. Re-use one instance per
## simultaneously running job rather than constructing four generators per chunk.
var _free_details: Array = []

var _have_observer := false
var _motion_dir := Vec3D.new(0.0, 0.0, 0.0)
var _motion_speed := 0.0


func _ready() -> void:
	super._ready()
	# Mesh uploads stay on the main thread. A time budget is more useful than a
	# tiny fixed count, but retain this as a hard ceiling for pathological frames.
	max_builds_per_frame = 24


func build_roots() -> void:
	# Any queued nodes belong to the tree that is about to be abandoned. Running
	# jobs are still awaited through the base class' _pending_tasks and their
	# results are discarded because _clear_tree() marks those nodes abandoned.
	_request_queue.clear()
	_request_kind.clear()
	_coverage_only.clear()

	super.build_roots()

	# Guarantee coverage before the first streaming frame. Eight by eight is tiny
	# compared with a normal 32x32 chunk but is enough to make the complete planet
	# exist while the priority queue fills in the real LODs behind it.
	var n := mini(COVERAGE_GRID, cfg.chunk_grid)
	var detail := Planet.make_detail()
	for node in roots:
		var data := ChunkBuilder.build(node.face, node.u0, node.v0, node.size,
			n, detail, {}, false)
		super._instantiate(node, data)
		_coverage_only[node.get_instance_id()] = true


func set_observer(world_pos: Vec3D) -> void:
	# Estimate velocity here so scheduling can prefetch where the player is going
	# without coupling the streamer to the player controller.
	if _have_observer:
		var delta := world_pos.sub(observer)
		var moved := delta.length()
		if moved > 1e-4 and moved < TELEPORT_THRESHOLD_METRES:
			_motion_dir = delta.mul(1.0 / moved)
			var sample_dt := maxf(_dt, 1.0 / 240.0)
			var instant_speed := moved / sample_dt
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

	# Install completed work first. The quadtree can then react to newly available
	# coverage in the same frame instead of waiting an extra frame before splitting.
	_drain_results()

	_stats["nodes"] = 0
	_stats["culled"] = 0
	_stats["handoffs"] = 0
	var r_obs := maxf(observer.length(), cfg.planet_radius + 1.0)
	_obs_dir = observer.normalized().to_v3()
	_horizon_angle = acos(clampf(cfg.planet_radius / r_obs, -1.0, 1.0)) \
		+ acos(clampf(cfg.planet_radius / (cfg.planet_radius + MAX_TERRAIN_HEIGHT), -1.0, 1.0))
	_stats["horizon_deg"] = rad_to_deg(_horizon_angle)
	_stats["horizon_km"] = _horizon_angle * cfg.planet_radius / 1000.0
	_ground_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_water_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)

	_morph_lock = 0
	for r in roots:
		_update(r, false, 0.0)

	_pump_requests()
	_stats["queued"] = _request_queue.size()
	_stats["in_flight"] = _in_flight


func _update(node, hidden: bool, morph: float) -> void:
	var node_ang := node.size * (PI * 0.25) * 0.78
	var cull_at := _horizon_angle + node_ang
	if node.chunk != null or not node.is_leaf():
		cull_at *= HORIZON_CULL_HYSTERESIS

	# dot/cos avoids angle_to()'s acos for every quadtree node.
	if node.center_dir.dot(_obs_dir) < cos(cull_at):
		_cancel_queued(node)
		if not node.is_leaf():
			_collapse(node)
		_free_chunk(node)
		_stats["culled"] += 1
		return
	_stats["nodes"] += 1

	var center_dist := _distance_to_node(node, observer)
	var dist := center_dist - node.arc * 0.6

	var want_split: bool
	if forced_depth >= 0:
		want_split = node.depth < forced_depth and _stats["nodes"] < FORCED_NODE_BUDGET
	else:
		want_split = node.depth < cfg.quadtree_max_depth \
			and dist < node.arc * cfg.lod_split_factor

	if node.is_leaf():
		if want_split:
			# If a full-resolution replacement was queued while this was a terminal
			# leaf and the observer moved closer, cancel it if it has not started.
			# A running full build is allowed to finish before splitting so it cannot
			# arrive later and reset a non-leaf's handoff state.
			var request_id := node.get_instance_id()
			if node.state == 1 and not bool(_request_kind.get(request_id, true)):
				if node.task_id < 0:
					_cancel_queued(node)
				else:
					_apply_chunk(node, not hidden, morph)
					return

			# Queue a cheap mesh for this level, but do not wait for it before
			# descending. The already resident ancestor remains visible. Shallow
			# coverage jobs receive the highest scheduler priority, so handoff walks
			# down the tree rapidly while deep work is already queued in parallel.
			if node.chunk == null and (node.state == 0 or node.dirty):
				_request(node, true)
			if _morph_lock == 0:
				_split(node)
			else:
				_apply_chunk(node, not hidden, morph)
				return
		else:
			# Terminal leaves get the real mesh. If this leaf currently carries a
			# temporary coverage mesh, keep drawing it while the full replacement
			# is generated.
			var id := node.get_instance_id()
			if node.chunk == null:
				if node.state == 0 or node.dirty:
					_request(node, false)
			elif _coverage_only.has(id) and node.state != 1:
				_request(node, false)
			elif node.dirty and node.state != 1:
				_request(node, false)
			_apply_chunk(node, not hidden, morph)
			return

	# A node can become non-leaf before its coverage task starts. If it was culled
	# in between, its queued task may have been cancelled; restore that fallback
	# whenever it becomes relevant again.
	if node.chunk == null and node.state == 0:
		_request(node, want_split)

	if want_split:
		if node.fine_vis <= 0.0:
			if node.chunk == null:
				# An ancestor is still covering this region. Descendants may continue
				# loading, but this level cannot take over until its own coverage exists.
				node.fine_vis = 1.0
			else:
				var covered := true
				for c in node.children:
					if not _subtree_covered(c):
						covered = false
				if covered and not _subtree_morphing(node):
					_free_chunk(node)
					node.fine_vis = MORPH_EPSILON
		else:
			node.fine_vis = move_toward(node.fine_vis, 1.0, _dt / LOD_MORPH_TIME)
			if node.chunk != null:
				_free_chunk(node)
	else:
		var id := node.get_instance_id()
		if node.chunk == null:
			if _morph_lock == 0 and (node.state == 0 or node.dirty):
				_request(node, false)
		elif _coverage_only.has(id) and node.state != 1:
			# The camera stopped at this level: upgrade the cheap fallback without
			# taking it away first.
			_request(node, false)
		else:
			node.fine_vis = move_toward(node.fine_vis, 0.0, _dt / LOD_MORPH_TIME)
			if node.fine_vis <= 0.0 and not _subtree_morphing(node):
				_collapse(node)
				_apply_chunk(node, not hidden, morph)
				return

	var showing_self := node.chunk != null and node.fine_vis <= 0.0
	if not node.is_leaf() and (node.chunk != null or node.fine_vis < 1.0):
		_stats["handoffs"] += 1
	_apply_chunk(node, not hidden and showing_self, morph)

	var child_morph := 1.0 - node.fine_vis
	var lock := node.fine_vis > 0.0 and node.fine_vis < 1.0
	if lock:
		_morph_lock += 1
	for c in node.children:
		_update(c, hidden or showing_self, child_morph)
	if lock:
		_morph_lock -= 1


func _subtree_covered(node) -> bool:
	# The nearest resident ancestor is a valid coverage surface. Do not make a
	# parent wait for every grandchild and great-grandchild before handing one
	# quadrant to its child; that was needlessly serialising LOD refinement.
	if node.chunk != null:
		return true
	if node.is_leaf():
		return false
	for c in node.children:
		if not _subtree_covered(c):
			return false
	return true


func _request(node, coverage: bool = false) -> void:
	if _shutting_down:
		return
	var id := node.get_instance_id()
	var replacing_coverage := node.state == 2 and node.chunk != null \
		and _coverage_only.has(id) and not coverage
	if node.state == 1:
		return
	if node.state == 2 and not replacing_coverage:
		return

	node.state = 1
	node.dirty = false
	node.task_id = -1
	_request_kind[id] = coverage
	_request_queue.append({
		"node": node,
		"coverage": coverage,
		"n": mini(COVERAGE_GRID, cfg.chunk_grid) if coverage else cfg.chunk_grid,
		"priority": 0.0,
	})


func _cancel_queued(node) -> void:
	if node.state != 1 or node.task_id >= 0:
		return
	_request_kind.erase(node.get_instance_id())
	# A full upgrade can be cancelled while its temporary mesh remains resident.
	node.state = 2 if node.chunk != null else 0


func _pump_requests() -> void:
	if _request_queue.is_empty() or _shutting_down:
		return

	# Drop stale queue entries and rank the survivors against the current and
	# predicted observer positions. This is deliberately done immediately before
	# launch, not when a node first requests work, so moving the camera instantly
	# changes what the workers do next.
	var kept: Array = []
	for entry in _request_queue:
		var node = entry["node"]
		if not is_instance_valid(node):
			continue
		if node.abandoned or node.state != 1 or node.task_id >= 0:
			continue
		entry["priority"] = _priority(node, bool(entry["coverage"]))
		kept.append(entry)
	_request_queue = kept

	# Descending sort + pop_back gives the smallest score without O(n) front pops.
	_request_queue.sort_custom(func(a, b):
		return float(a["priority"]) > float(b["priority"]))

	while _in_flight < MAX_CONCURRENT_BUILDS and not _request_queue.is_empty():
		var entry: Dictionary = _request_queue.pop_back()
		var node = entry["node"]
		if node.abandoned or node.state != 1 or node.task_id >= 0:
			continue
		_start_request(entry)


func _start_request(entry: Dictionary) -> void:
	var node = entry["node"]
	var coverage: bool = entry["coverage"]
	var n: int = entry["n"]
	var band := band_for_depth(node.depth)
	# Coverage meshes exist only to prevent holes and never need a physics cook.
	# The player already resolves ground contact from the height field.
	var want_collision := (not coverage) and node.depth >= cfg.collision_depth
	var ang := node.size * (PI * 0.25) * 1.5
	var snap := Deltas.snapshot_for_bounds(node.center_dir, ang) if band >= Band.LOCAL else {}

	var detail: TerrainDetail
	if _free_details.is_empty():
		detail = Planet.make_detail()
	else:
		detail = _free_details.pop_back()

	var task := func() -> void:
		var data := ChunkBuilder.build(node.face, node.u0, node.v0, node.size,
			n, detail, snap, want_collision)
		_res_mutex.lock()
		_results.append({
			"node": node,
			"data": data,
			"detail": detail,
			"coverage": coverage,
		})
		_res_mutex.unlock()

	# Coverage near the observer is the only terrain work promoted to the worker
	# pool's high-priority lane. Deep cosmetic refinement stays low priority so it
	# cannot starve gameplay jobs that may share the pool later.
	var high_priority := coverage and float(entry["priority"]) < 1.0
	var tid := WorkerThreadPool.add_task(task, high_priority, "asterra_chunk")
	node.task_id = tid
	_pending_tasks.append(tid)
	_in_flight += 1


func _drain_results() -> void:
	var built := 0
	var start_usec := Time.get_ticks_usec()
	while built < max_builds_per_frame:
		if built > 0 and Time.get_ticks_usec() - start_usec >= INSTALL_BUDGET_USEC:
			break

		_res_mutex.lock()
		if _results.is_empty():
			_res_mutex.unlock()
			break

		# Worker finish order is irrelevant. Install the completed chunk the player
		# needs most, so a slow far-away job cannot sit in front of a nearby result.
		var best_i := 0
		var best_p := INF
		for i in _results.size():
			var candidate: Dictionary = _results[i]
			var p := _priority(candidate["node"], bool(candidate["coverage"]))
			if p < best_p:
				best_p = p
				best_i = i
		var item: Dictionary = _results[best_i]
		_results.remove_at(best_i)
		_res_mutex.unlock()

		var node = item["node"]
		var tid: int = node.task_id
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
			_pending_tasks.erase(tid)
			node.task_id = -1
		_in_flight = maxi(0, _in_flight - 1)
		_free_details.append(item["detail"])
		_request_kind.erase(node.get_instance_id())

		if not node.abandoned:
			super._instantiate(node, item["data"])
			var id := node.get_instance_id()
			if bool(item["coverage"]):
				_coverage_only[id] = true
				# The base instantiate path assumes a non-leaf mesh arriving late is
				# obsolete and sets fine_vis to 1. Here it is intentionally the next
				# fallback level, so make it eligible to cover its subtree.
				if not node.is_leaf():
					node.fine_vis = 0.0
			else:
				_coverage_only.erase(id)
		else:
			node.state = 0
		built += 1


func _priority(node, coverage: bool) -> float:
	var arc := maxf(node.arc, 1.0)
	var current := _distance_to_node(node, observer) / arc
	var score := current

	if _motion_speed > 0.5 and _motion_dir.length_sq() > 0.5:
		var ahead := minf(_motion_speed * PREFETCH_SECONDS, PREFETCH_MAX_METRES)
		var predicted := observer.add(_motion_dir.mul(ahead))
		var future := _distance_to_node(node, predicted) / arc
		# Future terrain may outrank current terrain, but never by so much that
		# turning around makes the area under the player wait.
		score = minf(score, future * 0.82)

	if coverage:
		# Coverage is structural: it prevents a hole. Shallow coverage gets an
		# additional bias so refinement proceeds level-by-level from a guaranteed
		# fallback even though deep requests are already queued.
		score -= 3.0
		score += float(node.depth) * 0.10
	else:
		score += float(node.depth) * 0.015

	if node.chunk == null:
		score -= 1.0
	return score


func _distance_to_node(node, from: Vec3D) -> float:
	var best := 1e30
	for p_value in node.lod_centers:
		var p: Vec3D = p_value
		best = minf(best, from.distance_to(p))
	return best
