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

# ChunkBuilder is deliberately CPU-heavy: each tile evaluates the physical height
# function, normals and surface fields at more than a thousand points. Running one
# job per logical thread starves the main/render threads on SMT CPUs. Four heavy
# workers is enough to keep generation moving on a modern desktop while leaving
# real core time for the game.
const MAX_CONCURRENT_BUILDS_CAP := 4
# Do not let refinement manufacture thousands of jobs faster than ChunkBuilder can
# consume them. Resident parents already cover those regions, so deferring deeper
# splits is visually safe and turns the queue into explicit streaming backpressure.
const MAX_PENDING_BUILDS := 256
const INSTALL_BUDGET_USEC := 2200
const PREFETCH_SECONDS := 1.35
const PREFETCH_MAX_METRES := 120000.0
const TELEPORT_THRESHOLD_METRES := 250000.0
const TOPOLOGY_SCAN_INTERVAL := 0.15

## Queue entries: { node, priority, missing }
var _request_queue: Array = []
## TerrainDetail owns several FastNoiseLite generators. Keep one per concurrently
## running worker and recycle it when that worker finishes.
var _free_details: Array[TerrainDetail] = []

var _max_concurrent_builds: int = 4
var _have_observer := false
var _motion_dir := Vec3D.new(0.0, 0.0, 0.0)
var _motion_speed := 0.0
var _collision_streamer: TerrainCollisionStreamer
var _topology_scan_left := 0.0


func _ready() -> void:
	super._ready()
	# OS.get_processor_count() reports logical processors. ChunkBuilder jobs are
	# compute-bound, so using almost every SMT thread hurts frame time badly. Use
	# roughly one worker per four logical processors, capped at four.
	_max_concurrent_builds = clampi(OS.get_processor_count() / 4, 2, MAX_CONCURRENT_BUILDS_CAP)
	# Mesh creation/upload stays on the main thread and is separately time-budgeted.
	max_builds_per_frame = 8
	_collision_streamer = TerrainCollisionStreamer.new()
	add_child(_collision_streamer)


func build_roots() -> void:
	_request_queue.clear()
	_have_observer = false
	_motion_speed = 0.0
	_topology_scan_left = 0.0

	super.build_roots()
	_collision_streamer.configure(cfg)

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
	# Angular size of one pixel. The ground material picks which octaves of its
	# procedural detail to synthesise from this, so it decides how much texture a
	# given resolution actually gets -- and it is the difference between 4K
	# resolving leaf litter and 4K rendering the same blur as 900p, larger.
	var cam := get_viewport().get_camera_3d()
	if cam != null:
		var vh: float = maxf(get_viewport().get_visible_rect().size.y, 1.0)
		_lod_projection_scale = vh / maxf(2.0 * tan(deg_to_rad(cam.fov) * 0.5), 1e-4)
		_ground_mat.set_shader_parameter("u_pixel_angle",
			deg_to_rad(cam.fov) / vh)

	# Height above the ground under the observer. The terrain shader needs it to
	# know whether the camera is above the canopy looking at crowns or under it
	# looking at the forest floor -- opposite sides of the same forest, and only
	# one of them is what a top-down crown model draws.
	var camera_agl := maxf(
		observer.length() - cfg.planet_radius - Planet.terrain_height(_obs_dir), 0.0)
	_ground_mat.set_shader_parameter("u_camera_agl", camera_agl)
	_collision_streamer.set_observer(observer, camera_agl <= 320.0)

	_morph_lock = 0
	for value in roots:
		var root: PlanetTerrain.QuadNode = value
		_update(root, false, 0.0)

	# Neighbour discovery is one of the largest main-thread costs at high node
	# counts. Keep a wall-time cadence during normal play, but if a bad frame takes
	# longer than that interval do not immediately run the scan again next frame.
	# This prevents the old low-FPS feedback loop where the 75 ms scan happened on
	# every 200-300 ms frame and helped keep the game stuck there.
	_topology_scan_left -= dt
	if _topology_scan_left <= 0.0:
		_balance_quadtree()
		_refresh_stitch_masks()
		_topology_scan_left = maxf(TOPOLOGY_SCAN_INTERVAL, dt * 2.0)

	_pump_requests()
	_stats["queued"] = _request_queue.size()
	_stats["in_flight"] = _in_flight


## Angular radius around the observer inside which this node is worth drawing.
##
## Nodes that already have a mesh, and interior nodes, get a wider margin: they
## cost nothing to keep and the hysteresis stops a node flickering in and out
## along the horizon.
func _cull_angle(node: PlanetTerrain.QuadNode) -> float:
	var cull_at: float = _horizon_angle + node.size * (PI * 0.25) * 0.78
	if node.chunk != null or not node.is_leaf():
		cull_at *= HORIZON_CULL_HYSTERESIS
	return cull_at


## Has the horizon culler dropped this node? Uses exactly the same test
## `_update` applies, because coverage depends on agreeing with it.
func _is_culled(node: PlanetTerrain.QuadNode) -> bool:
	return node.center_dir.dot(_obs_dir) < cos(_cull_angle(node))


func _streaming_saturated() -> bool:
	return _request_queue.size() + _in_flight >= MAX_PENDING_BUILDS


func _update(node: PlanetTerrain.QuadNode, hidden: bool, morph: float) -> void:
	# Avoid acos/angle_to per node. When a subtree leaves the horizon, collapse
	# its children but KEEP this node's own mesh resident as the fallback for the
	# instant the player turns back toward it.
	if _is_culled(node):
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
	var want_split := _wants_split(node, dist)
	# Refinement is optional while a resident parent already covers this region.
	# Once the pending-work budget is full, keep that parent instead of creating
	# four more children and an exponentially growing backlog.
	if node.is_leaf() and node.chunk != null and want_split and _streaming_saturated():
		want_split = false

	# Retained parents are visible fallbacks and must be rebuilt after an edit;
	# updating only their leaf descendants briefly restores stale terrain whenever
	# a handoff falls back to the parent.
	if node.chunk != null and node.dirty and node.state != 1:
		_request(node)

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
	# Only count a handoff that is actually in progress. This used to increment
	# for every resident interior node, so the figure never reached zero once the
	# tree had any depth, and anything waiting on "the terrain has settled" waited
	# for ever instead.
	if node.fine_vis > 0.0 and node.fine_vis < 1.0:
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


const MAX_BALANCE_PASSES := 24
const NEIGHBOUR_PROBE := 1e-6

## Enforce a structural 2:1 quadtree before deriving edge topology. Distance
## heuristics usually produce this accidentally, but edits, cancellation and
## cross-face traversal do not. Splitting only resident nodes preserves the
## coverage-first invariant: a balance repair can never remove the fallback.
func _balance_quadtree() -> void:
	# During sustained streaming, do a small amount of balancing per scan instead
	# of potentially walking the whole tree 24 times in one frame.
	var pass_budget := 4 if _streaming_saturated() else MAX_BALANCE_PASSES
	for _pass in pass_budget:
		var leaves: Array[PlanetTerrain.QuadNode] = []
		for value in roots:
			_collect_leaves(value, leaves)
		var changed := false
		for leaf in leaves:
			if leaf.depth >= cfg.quadtree_max_depth or leaf.chunk == null or leaf.state == 1:
				continue
			for edge in 4:
				var neighbour := _leaf_at_direction(_edge_probe_dir(leaf, edge))
				if neighbour != null and neighbour.depth > leaf.depth + 1:
					_split(leaf)
					changed = true
					break
			if changed:
				break
		if not changed:
			return


func _collect_leaves(node: PlanetTerrain.QuadNode,
		out: Array[PlanetTerrain.QuadNode]) -> void:
	if node.is_leaf():
		out.append(node)
		return
	for value in node.children:
		_collect_leaves(value, out)


func _edge_probe_dir(node: PlanetTerrain.QuadNode, edge: int) -> Vector3:
	var u := node.u0 + node.size * 0.5
	var v := node.v0 + node.size * 0.5
	match edge:
		FaceEdge.LEFT:
			u = node.u0 - NEIGHBOUR_PROBE
		FaceEdge.RIGHT:
			u = node.u0 + node.size + NEIGHBOUR_PROBE
		FaceEdge.BOTTOM:
			v = node.v0 - NEIGHBOUR_PROBE
		FaceEdge.TOP:
			v = node.v0 + node.size + NEIGHBOUR_PROBE
	return CubeSphere.face_uv_to_dir(node.face, u, v)


func _leaf_at_direction(d: Vector3) -> PlanetTerrain.QuadNode:
	var fuv := CubeSphere.dir_to_face_uv(d)
	var node: PlanetTerrain.QuadNode = roots[int(fuv[0])]
	var u := float(fuv[1])
	var v := float(fuv[2])
	while not node.is_leaf():
		var half := node.size * 0.5
		var east := 1 if u >= node.u0 + half else 0
		var north := 1 if v >= node.v0 + half else 0
		node = node.children[north * 2 + east]
	return node


## Return the deepest actually drawn resident at a direction. Retained children
## can exist below a visible parent during a handoff, so structural depth alone
## is not the topology currently reaching the framebuffer.
func _drawn_node_at_direction(d: Vector3) -> PlanetTerrain.QuadNode:
	var fuv := CubeSphere.dir_to_face_uv(d)
	var node: PlanetTerrain.QuadNode = roots[int(fuv[0])]
	var result: PlanetTerrain.QuadNode = node if node.chunk != null and node.drawn else null
	var u := float(fuv[1])
	var v := float(fuv[2])
	while not node.is_leaf():
		var half := node.size * 0.5
		var east := 1 if u >= node.u0 + half else 0
		var north := 1 if v >= node.v0 + half else 0
		node = node.children[north * 2 + east]
		if node.chunk != null and node.drawn:
			result = node
	return result


func _refresh_stitch_masks() -> void:
	var residents: Array[PlanetTerrain.QuadNode] = []
	for value in roots:
		_collect_residents(value, residents)
	for node in residents:
		if not node.drawn:
			continue
		var mask := 0
		for edge in 4:
			var neighbour := _drawn_node_at_direction(_edge_probe_dir(node, edge))
			if neighbour != null and neighbour.depth == node.depth - 1:
				mask |= 1 << edge
		if mask != node.stitch_mask:
			node.stitch_mask = mask
			if node.built_stitch_mask != mask:
				node.revision += 1
				node.dirty = true


func _collect_residents(node: PlanetTerrain.QuadNode,
		out: Array[PlanetTerrain.QuadNode]) -> void:
	if node.chunk != null:
		out.append(node)
	for value in node.children:
		_collect_residents(value, out)


func _subtree_covered(node: PlanetTerrain.QuadNode) -> bool:
	# Any resident node is enough to cover its entire region, regardless of how
	# much deeper refinement underneath it is still pending.
	if node.chunk != null:
		return true
	# A node the horizon culler has dropped cannot expose a hole, because nothing
	# inside it is drawn -- and demanding a mesh from it deadlocks the entire
	# branch above it.
	#
	# This is not a corner case, it is the normal state of the tree. `_update`
	# returns early for a culled node, before the request, so a chunkless leaf
	# out there stays in state 0 for ever: it is never drawn, so it never asks
	# for a mesh, so it never gets one. Coverage then fails at the root, the root
	# keeps `showing_self` and every refined chunk under it -- a thousand of them,
	# fully built and resident -- stays hidden. The whole planet renders from the
	# six root faces at 49 km vertex spacing, which is a smooth sphere: no relief
	# at any altitude, and below about 450 metres the camera ends up *inside* that
	# sphere and the ground vanishes entirely.
	#
	# The horizon shrinks as the observer descends, so the lower the camera the
	# more nodes are culled and the more certain the deadlock: it is worst exactly
	# where the terrain matters most. The parent stays resident as the fallback,
	# so if one of these nodes does come back over the horizon, coverage drops,
	# the parent shows itself again and the request goes out as normal.
	if _is_culled(node):
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
	# Missing refinement is deferrable because an ancestor is already resident.
	# Dirty resident rebuilds are deliberately exempt so edits can never starve.
	if missing and _streaming_saturated():
		return

	node.state = 1
	node.dirty = false
	node.task_id = -1
	node.requested_revision = node.revision
	node.request_token += 1
	_request_queue.append({
		"node": node,
		"missing": missing,
		"revision": node.requested_revision,
		"token": node.request_token,
		"priority": 0.0,
	})


func _cancel_queued(node: PlanetTerrain.QuadNode) -> void:
	if node.state != 1 or node.task_id >= 0:
		return
	# The queue entry is lazily discarded by _pump_requests(). If a resident
	# chunk was queued for an edit/stitch rebuild, _request() cleared dirty while
	# it waited. Cancelling that queue entry must restore dirty or the old resident
	# mesh can incorrectly become the permanent current revision.
	var resident := node.chunk != null
	node.request_token += 1
	node.state = 2 if resident else 0
	node.requested_revision = -1
	node.dirty = resident


func _pump_requests() -> void:
	if _request_queue.is_empty() or _shutting_down:
		return

	# Re-rank every waiting request against the latest observer position. The queue
	# is hard-bounded, so this remains cheap instead of sorting thousands of entries
	# on the main thread every frame.
	var kept: Array = []
	for value in _request_queue:
		var entry: Dictionary = value
		var node: PlanetTerrain.QuadNode = entry["node"]
		if not is_instance_valid(node):
			continue
		if node.abandoned or node.state != 1 or node.task_id >= 0 \
				or int(entry.get("token", -1)) != node.request_token:
			continue
		# A queued request has not consumed worker time yet, so update it in place
		# when an edit or stitch transition supersedes its captured generation.
		if int(entry.get("revision", -1)) != node.revision:
			entry["revision"] = node.revision
			node.requested_revision = node.revision
			node.dirty = false
		entry["priority"] = _priority(node, bool(entry["missing"]))
		kept.append(entry)
	_request_queue = kept

	# Descending order lets pop_back() take the smallest (best) score cheaply.
	_request_queue.sort_custom(func(a, b):
		return float(a["priority"]) > float(b["priority"]))

	while _in_flight < _max_concurrent_builds and not _request_queue.is_empty():
		var entry: Dictionary = _request_queue.pop_back()
		var node: PlanetTerrain.QuadNode = entry["node"]
		if node.abandoned or node.state != 1 or node.task_id >= 0 \
				or int(entry.get("token", -1)) != node.request_token:
			continue
		_start_request(entry)


func _start_request(entry: Dictionary) -> void:
	var node: PlanetTerrain.QuadNode = entry["node"]
	var request_revision := int(entry["revision"])
	node.requested_revision = request_revision
	var stitch_mask := node.stitch_mask
	var band: int = band_for_depth(node.depth)
	# Physics has its own camera-local stream; visual LOD churn must not create or
	# retire collision bodies.
	var want_collision := false
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
			cfg.chunk_grid, detail, snap, want_collision, stitch_mask)
		_res_mutex.lock()
		_results.append({"node": node, "data": data, "detail": detail,
			"revision": request_revision})
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

		var result_revision := int(item.get("revision", -1))
		if not node.abandoned and result_revision == node.revision:
			super._instantiate(node, item["data"])
			node.dirty = false
		else:
			# Logical cancellation: workers are not force-killed, but obsolete output
			# can never replace a newer edit/topology generation.
			node.state = 2 if node.chunk != null else 0
			node.dirty = not node.abandoned
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