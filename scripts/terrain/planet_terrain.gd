class_name PlanetTerrain
extends Node3D
## 1.1 Planetary surface and streaming.
##
## A cube-sphere quadtree: six roots, split by screen-relevant distance, so the
## same data structure serves the whole traversal from orbit to a 0.75 m editable
## surface. Four named representation bands fall out of the depth:
##
##   ORBITAL   depth 0-4    whole-planet silhouette
##   REGIONAL  depth 5-9    landforms, watersheds, coastlines
##   LOCAL     depth 10-13  walkable ground, collision
##   EDITABLE  depth 14+    matches the terrain-delta lattice
##
## Chunks are meshed on worker threads and only handed to the scene graph when
## complete. Neither direction of an LOD change is allowed to show: a parent is
## not released until every leaf under it is ready, a parent is rebuilt *before*
## its children are dropped, and the swap itself is dissolved over a fifth of a
## second by a screen-space dither in the terrain shader -- the two meshes
## partition the pixels between them, so the change reads as the surface
## resolving rather than as a pop.

enum Band { ORBITAL, REGIONAL, LOCAL, EDITABLE }

const BAND_MIN_DEPTH := [0, 5, 10, 14]

## Seconds a tile takes to relax from its parent's shape into its own. Long
## enough that the deformation is below notice, short enough that the finer
## shape arrives promptly.
const LOD_MORPH_TIME := 0.35
## The morph is committed at a hair above zero rather than at zero, so the frame
## the children appear on is one where they are still exactly the parent.
const MORPH_EPSILON := 0.0015
## CUSTOM0 carries the per-vertex offset onto the parent's surface, as four
## floats so the vertex shader can read it as a vec4.
const MORPH_FORMAT := Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT

class QuadNode extends RefCounted:
	var face: int
	var depth: int
	var u0: float
	var v0: float
	var size: float
	var center_dir: Vector3
	var center_world: Vec3D
	var arc: float                 ## chunk edge length in metres
	var children: Array = []
	var chunk: Node3D = null
	var ground_mi: MeshInstance3D = null
	var water_mi: MeshInstance3D = null
	var body: StaticBody3D = null
	var state: int = 0             ## 0 idle, 1 requested, 2 ready
	var task_id: int = -1
	var dirty: bool = false
	var abandoned: bool = false    ## dropped from the tree while its build was in flight
	## How far this subtree has moved from drawing *this* tile to drawing its
	## children: 0 = this tile, 1 = the children in their own shape. In between,
	## the children are on screen bent part-way onto this tile's surface.
	var fine_vis: float = 0.0
	## Whether this node's own mesh is the one currently on screen.
	var drawn: bool = false
	## The morph factor currently pushed to this node's mesh.
	var morph: float = 0.0
	## Weak on purpose: a node holds its children, so a strong link back would
	## make every parent-child pair a reference cycle and leak the whole tree.
	var parent_ref: WeakRef = null

	func parent() -> QuadNode:
		return parent_ref.get_ref() if parent_ref != null else null

	func is_leaf() -> bool:
		return children.is_empty()

var cfg: GenConfig
var observer: Vec3D = Vec3D.new(0, 0, 0)
## Debug: pin the whole tree to one depth instead of splitting by distance.
## -1 leaves the distance rule alone.
var forced_depth := -1
## A uniform depth over a visible hemisphere is a quadratic amount of ground, so
## the forced view stops splitting once it has this many nodes rather than
## locking the machine up.
const FORCED_NODE_BUDGET := 3000
var max_builds_per_frame := 8
var roots: Array = []

var _results: Array = []
var _res_mutex := Mutex.new()
var _ground_mat: ShaderMaterial
var _water_mat: ShaderMaterial
var _pending_tasks: Array[int] = []
## Highest terrain a node could contain; used for the horizon test.
const MAX_TERRAIN_HEIGHT := 6800.0

var _stats := {"chunks": 0, "nodes": 0, "queued": 0, "in_flight": 0, "culled": 0,
	"handoffs": 0, "horizon_deg": 0.0, "horizon_km": 0.0}
var _obs_dir := Vector3(1, 0, 0)
var _horizon_angle := 0.0
var _in_flight := 0
var _shutting_down := false
var _dt := 0.0
## Non-zero while the recursion is inside a subtree that is mid-morph. The
## structure under a morph is frozen for its duration: a tile is bent onto its
## parent's surface, and re-cutting the tree underneath it would leave that
## deformation describing a shape nothing is drawing. A node merely *waiting*
## for its children to build does not freeze anything -- they are not on screen
## yet, so they are free to refine further, and a dive from orbit resolves all
## the way down instead of one level per morph.
var _morph_lock := 0

func _ready() -> void:
	process_priority = 10
	_ground_mat = ShaderMaterial.new()
	_ground_mat.shader = load("res://shaders/terrain_ground.gdshader")
	_water_mat = ShaderMaterial.new()
	_water_mat.shader = load("res://shaders/ocean.gdshader")
	_push_origin()
	Frames.origin_shifted.connect(_on_origin_shifted)
	Deltas.region_changed.connect(_on_region_changed)

func build_roots() -> void:
	cfg = Planet.cfg
	_clear_tree()
	# Vertex spacing per metre of view distance, straight off the split rule the
	# quadtree actually uses, so the ocean shader can size its shoreline fade the
	# same way the mesher sized the mesh.
	_water_mat.set_shader_parameter("u_cell_per_metre",
		1.0 / (cfg.lod_split_factor * float(cfg.chunk_grid)))
	for mat in [_ground_mat, _water_mat]:
		mat.set_shader_parameter("u_planet_radius", cfg.planet_radius)
		mat.set_shader_parameter("u_atmosphere_height", cfg.atmosphere_height)
		mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	roots.clear()
	for face in 6:
		roots.append(_make_node(face, 0, -1.0, -1.0, 2.0))

## Drop the whole tree and every mesh it owns.
##
## Without this, rebuilding the roots leaves the old chunk holders parented to
## this node for good: a holder is only ever released through its QuadNode, and
## clearing `roots` drops those nodes on the floor. Anything still meshing is
## marked abandoned so its result is discarded rather than instantiated into an
## orphan nothing will ever manage again.
func _clear_tree() -> void:
	for r in roots:
		_abandon(r)
	roots.clear()

func _abandon(node: QuadNode) -> void:
	for c in node.children:
		_abandon(c)
	node.children.clear()
	_free_chunk(node)
	if node.state == 1:
		node.abandoned = true
	node.state = 0

func _make_node(face: int, depth: int, u0: float, v0: float, size: float) -> QuadNode:
	var q := QuadNode.new()
	q.face = face
	q.depth = depth
	q.u0 = u0
	q.v0 = v0
	q.size = size
	q.center_dir = CubeSphere.face_uv_to_dir(face, u0 + size * 0.5, v0 + size * 0.5)
	var h := Planet.macro_height(q.center_dir)
	var r := cfg.planet_radius + h
	q.center_world = Vec3D.new(q.center_dir.x * r, q.center_dir.y * r, q.center_dir.z * r)
	q.arc = size * (PI * 0.25) * cfg.planet_radius
	return q

static func band_for_depth(depth: int) -> int:
	if depth >= BAND_MIN_DEPTH[Band.EDITABLE]:
		return Band.EDITABLE
	if depth >= BAND_MIN_DEPTH[Band.LOCAL]:
		return Band.LOCAL
	if depth >= BAND_MIN_DEPTH[Band.REGIONAL]:
		return Band.REGIONAL
	return Band.ORBITAL

func set_observer(world_pos: Vec3D) -> void:
	observer = world_pos

func _process(dt: float) -> void:
	if not Planet.ready_state or roots.is_empty():
		return
	_dt = dt
	_stats["nodes"] = 0
	_stats["queued"] = 0
	_stats["culled"] = 0
	_stats["handoffs"] = 0
	# Horizon: how far around the planet the observer can see, allowing for the
	# tallest terrain that could be poking up over the curve. Everything beyond it
	# is neither drawn nor meshed -- on a planet that is most of the surface.
	var r_obs := maxf(observer.length(), cfg.planet_radius + 1.0)
	_obs_dir = observer.normalized().to_v3()
	_horizon_angle = acos(clampf(cfg.planet_radius / r_obs, -1.0, 1.0)) \
		+ acos(clampf(cfg.planet_radius / (cfg.planet_radius + MAX_TERRAIN_HEIGHT), -1.0, 1.0))
	_stats["horizon_deg"] = rad_to_deg(_horizon_angle)
	_stats["horizon_km"] = _horizon_angle * cfg.planet_radius / 1000.0
	# Cheap enough to push every frame, and it means a moving sun lights the air
	# in front of the terrain at the same moment it lights the sky.
	_ground_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_water_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_morph_lock = 0
	for r in roots:
		_update(r, false, 0.0)
	_drain_results()
	_stats["in_flight"] = _in_flight

## Chunks already resident get to survive a bit past the point they'd be
## newly built at, so a fast-moving observer doesn't thrash a chunk right at
## the horizon line build-cull-build every frame -- at orbital fly speeds
## (tens of km/s) the boundary itself sweeps fast enough for that to happen
## continuously without this margin.
const HORIZON_CULL_HYSTERESIS := 1.3

func _update(node: QuadNode, hidden: bool, morph: float) -> void:
	# Over-the-horizon rejection, generous by the node's own angular size.
	var node_ang := node.size * (PI * 0.25) * 0.78
	var cull_at := _horizon_angle + node_ang
	if node.chunk != null or not node.is_leaf():
		cull_at *= HORIZON_CULL_HYSTERESIS
	if node.center_dir.angle_to(_obs_dir) > cull_at:
		if not node.is_leaf():
			_collapse(node)
		_free_chunk(node)
		_stats["culled"] += 1
		return
	_stats["nodes"] += 1
	var dist := observer.distance_to(node.center_world) - node.arc * 0.6
	var want_split: bool
	if forced_depth >= 0:
		want_split = node.depth < forced_depth and _stats["nodes"] < FORCED_NODE_BUDGET
	else:
		want_split = node.depth < cfg.quadtree_max_depth and dist < node.arc * cfg.lod_split_factor

	if node.is_leaf():
		# Splitting inside a subtree that is mid-morph would put a third shape
		# into a two-shape transition, so that is held until the morph lands.
		if want_split and _morph_lock == 0:
			_split(node)
		else:
			if node.state == 0 or node.dirty:
				_request(node)
			_apply_chunk(node, not hidden, morph)
			return

	if want_split:
		if node.fine_vis <= 0.0:
			if node.chunk == null:
				# Nothing to hand over from -- this tile was split before it ever
				# had a mesh of its own. There is no parent shape for the children
				# to start from, so they draw their own from the first frame.
				node.fine_vis = 1.0
			else:
				# Still drawing this tile. The hand-over happens the moment the
				# finer set can cover it: at a hair above zero the children are
				# morphed all the way onto *this* tile's surface, so dropping this
				# mesh at that instant changes nothing on screen. Everything after
				# it is the children relaxing into their own shape.
				var covered := true
				for c in node.children:
					if not _subtree_covered(c):
						covered = false
				if covered and not _subtree_morphing(node):
					_free_chunk(node)
					node.fine_vis = MORPH_EPSILON
		else:
			node.fine_vis = move_toward(node.fine_vis, 1.0, _dt / LOD_MORPH_TIME)
			# A merge that turned around leaves this mesh behind the children and
			# off screen; it is no longer anything's morph target, so it can go.
			if node.chunk != null:
				_free_chunk(node)
	else:
		# Merging. This level is built while the children are still on screen, and
		# only then do they walk onto its shape -- collapsing first would open a
		# hole for the whole rebuild.
		if node.chunk == null:
			if _morph_lock == 0 and (node.state == 0 or node.dirty):
				_request(node)
		else:
			node.fine_vis = move_toward(node.fine_vis, 0.0, _dt / LOD_MORPH_TIME)
			if node.fine_vis <= 0.0 and not _subtree_morphing(node):
				# The children are now exactly this tile's surface; swapping is
				# again a no-op on screen.
				_collapse(node)
				_apply_chunk(node, not hidden, morph)
				return

	var showing_self := node.chunk != null and node.fine_vis <= 0.0
	if not node.is_leaf() and (node.chunk != null or node.fine_vis < 1.0):
		_stats["handoffs"] += 1
	_apply_chunk(node, not hidden and showing_self, morph)

	# A tile only ever morphs against its own parent, so the factor is handed
	# down exactly one level; anything deeper is drawing its own shape.
	var child_morph := 1.0 - node.fine_vis
	var lock := node.fine_vis > 0.0 and node.fine_vis < 1.0
	if lock:
		_morph_lock += 1
	for c in node.children:
		_update(c, hidden or showing_self, child_morph)
	if lock:
		_morph_lock -= 1

## Which mesh of this subtree is on screen, and how far it has been bent toward
## the other one. Exactly one level is ever drawn: there is no blending, no
## dither and no second draw of the same ground.
func _apply_chunk(node: QuadNode, drawn: bool, morph: float) -> void:
	if node.chunk == null:
		return
	if node.drawn != drawn:
		node.drawn = drawn
		node.chunk.visible = drawn
		# Collision belongs to whatever is on screen, and to nothing else.
		_set_body_enabled(node, drawn)
	if not drawn:
		return
	if absf(node.morph - morph) > 1e-4:
		node.morph = morph
		if node.ground_mi != null:
			node.ground_mi.set_instance_shader_parameter("morph", morph)
		if node.water_mi != null:
			node.water_mi.set_instance_shader_parameter("morph", morph)

## True while anything below this node is part-way between two shapes. A tile
## can only hand over to a subtree that has settled, or the shape it hands over
## to is not the one being drawn.
func _subtree_morphing(node: QuadNode) -> bool:
	for c in node.children:
		if not c.is_leaf() and c.fine_vis > 0.0 and c.fine_vis < 1.0:
			return true
		if _subtree_morphing(c):
			return true
	return false

## True once every leaf under `node` has a built chunk -- recurses past
## grandchildren, unlike checking `node.state` alone.
func _subtree_covered(node: QuadNode) -> bool:
	if node.is_leaf():
		return node.state == 2
	for c in node.children:
		if not _subtree_covered(c):
			return false
	return true

func _set_body_enabled(node: QuadNode, on: bool) -> void:
	if node.body != null:
		node.body.collision_layer = 1 if on else 0

func _split(node: QuadNode) -> void:
	var h := node.size * 0.5
	node.children = [
		_make_node(node.face, node.depth + 1, node.u0, node.v0, h),
		_make_node(node.face, node.depth + 1, node.u0 + h, node.v0, h),
		_make_node(node.face, node.depth + 1, node.u0, node.v0 + h, h),
		_make_node(node.face, node.depth + 1, node.u0 + h, node.v0 + h, h),
	]
	# This tile stays on screen until the finer set is complete.
	node.fine_vis = 0.0
	for c in node.children:
		c.parent_ref = weakref(node)

func _collapse(node: QuadNode) -> void:
	for c in node.children:
		_collapse(c)
		_free_chunk(c)
		# A build already queued on a worker thread can't be cancelled; mark it
		# so the result is discarded instead of instantiated into an orphan
		# chunk nothing will ever manage again.
		if c.state == 1:
			c.abandoned = true
		c.state = 0
	node.children.clear()

func _free_chunk(node: QuadNode) -> void:
	if node.chunk != null:
		node.chunk.queue_free()
		node.chunk = null
		node.ground_mi = null
		node.water_mi = null
		node.body = null
		_stats["chunks"] -= 1
	# Releasing the mesh demotes the node back to idle. Leaving it at 2 would
	# claim a chunk that is no longer there: the node would never re-request
	# one when it collapses back to a leaf, and _subtree_covered would tell its
	# parent it is covered, so the parent drops its own mesh over the gap too.
	# A build still in flight (1) keeps its state -- the result is still coming.
	if node.state == 2:
		node.state = 0

func _request(node: QuadNode) -> void:
	if node.state == 1:
		return
	if _shutting_down:
		return
	node.state = 1
	node.dirty = false
	_stats["queued"] += 1
	_in_flight += 1
	var band := band_for_depth(node.depth)
	var want_collision := node.depth >= cfg.collision_depth
	var ang := node.size * (PI * 0.25) * 1.5
	var snap := Deltas.snapshot_for_bounds(node.center_dir, ang) if band >= Band.LOCAL else {}
	var n := cfg.chunk_grid
	var task := func() -> void:
		var detail := Planet.make_detail()
		var data := ChunkBuilder.build(node.face, node.u0, node.v0, node.size, n, detail, snap, want_collision)
		_res_mutex.lock()
		_results.append({"node": node, "data": data})
		_res_mutex.unlock()
	var tid := WorkerThreadPool.add_task(task, false, "asterra_chunk")
	node.task_id = tid
	_pending_tasks.append(tid)

func _drain_results() -> void:
	var built := 0
	while built < max_builds_per_frame:
		_res_mutex.lock()
		if _results.is_empty():
			_res_mutex.unlock()
			break
		var item: Dictionary = _results.pop_front()
		_res_mutex.unlock()
		var node: QuadNode = item["node"]
		_in_flight -= 1
		if node.task_id >= 0:
			# Every task must be waited on exactly once, even though we already
			# have its output: WorkerThreadPool keeps the task record alive until
			# it is, and unclaimed records abort the engine at shutdown.
			WorkerThreadPool.wait_for_task_completion(node.task_id)
			_pending_tasks.erase(node.task_id)
			node.task_id = -1
		if not node.abandoned:
			_instantiate(node, item["data"])
		built += 1

func _instantiate(node: QuadNode, data: Dictionary) -> void:
	_free_chunk(node)
	var holder := Node3D.new()

	var mesh := ArrayMesh.new()
	var arr := []
	arr.resize(Mesh.ARRAY_MAX)
	arr[Mesh.ARRAY_VERTEX] = data["vertices"]
	arr[Mesh.ARRAY_NORMAL] = data["normals"]
	arr[Mesh.ARRAY_COLOR] = data["colors"]
	arr[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arr[Mesh.ARRAY_CUSTOM0] = data["morph"]
	arr[Mesh.ARRAY_INDEX] = data["indices"]
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr, [], {}, MORPH_FORMAT)
	mesh.surface_set_material(0, _ground_mat)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON if node.depth >= 9 else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.set_instance_shader_parameter("chunk_depth", float(node.depth))
	holder.add_child(mi)
	node.ground_mi = mi

	# Water is its own instance, not a second surface: it is transparent, it is
	# sorted separately, and both meshes carry their own per-instance dissolve.
	if data.get("has_water", false):
		var wmesh := ArrayMesh.new()
		var warr := []
		warr.resize(Mesh.ARRAY_MAX)
		warr[Mesh.ARRAY_VERTEX] = data["water_vertices"]
		warr[Mesh.ARRAY_NORMAL] = data["water_normals"]
		warr[Mesh.ARRAY_COLOR] = data["water_colors"]
		warr[Mesh.ARRAY_TEX_UV] = data["water_uvs"]
		warr[Mesh.ARRAY_CUSTOM0] = data["water_morph"]
		warr[Mesh.ARRAY_INDEX] = data["water_indices"]
		wmesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, warr, [], {}, MORPH_FORMAT)
		wmesh.surface_set_material(0, _water_mat)
		var wmi := MeshInstance3D.new()
		wmi.mesh = wmesh
		wmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		holder.add_child(wmi)
		node.water_mi = wmi

	if data.has("collision_faces"):
		var body := StaticBody3D.new()
		var shape := CollisionShape3D.new()
		var poly := ConcavePolygonShape3D.new()
		poly.set_faces(data["collision_faces"])
		shape.shape = poly
		body.add_child(shape)
		holder.add_child(body)
		node.body = body

	holder.set_meta("pivot", data["pivot"])
	holder.position = Frames.to_render(data["pivot"])
	add_child(holder)
	node.chunk = holder
	node.state = 2
	_stats["chunks"] += 1

	if not node.is_leaf():
		# Built to merge: the children are still the ones on screen, and this mesh
		# waits behind them until they have been walked onto its shape.
		node.fine_vis = 1.0

	# Work out on the spot whether this mesh is the one that should be drawn --
	# waiting a frame for the next _update would either flash it over the level
	# already on screen or blink a hole where it is the only cover.
	var drawn := node.is_leaf() or node.fine_vis <= 0.0
	var above := node.parent()
	while above != null and drawn:
		if above.chunk != null and above.fine_vis <= 0.0:
			drawn = false
		above = above.parent()
	node.drawn = drawn
	holder.visible = drawn
	var up := node.parent()
	node.morph = 1.0 - up.fine_vis if up != null else 0.0
	mi.set_instance_shader_parameter("morph", node.morph)
	if node.water_mi != null:
		node.water_mi.set_instance_shader_parameter("morph", node.morph)
	_set_body_enabled(node, drawn)

func _on_origin_shifted(_delta: Vector3) -> void:
	for child in get_children():
		if child.has_meta("pivot"):
			child.position = Frames.to_render(child.get_meta("pivot"))
	_push_origin()

## Both terrain shaders generate their detail -- ground mottling, ocean waves --
## in planet coordinates, of which the render frame is only a moving offset.
## Handing them that offset is what stops the detail sliding across the surface
## every time the floating origin re-bases underfoot.
func _push_origin() -> void:
	var o := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_ground_mat.set_shader_parameter("u_origin", o)
	_water_mat.set_shader_parameter("u_origin", o)

## An edit invalidates every chunk whose extent it touches.
func _on_region_changed(center: Vector3, radius_m: float) -> void:
	var ang := radius_m / cfg.planet_radius + 1e-5
	for r in roots:
		_mark_dirty(r, center, ang)

func _mark_dirty(node: QuadNode, center: Vector3, ang: float) -> void:
	if node.center_dir.angle_to(center) > ang + node.size * (PI * 0.25) * 1.2:
		return
	if node.is_leaf():
		if node.state == 2:
			node.dirty = true
			node.state = 0
	else:
		for c in node.children:
			_mark_dirty(c, center, ang)

## The two materials every chunk is drawn with, for the debug overlay to poke
## uniforms into. Nothing else should be reaching in here.
func debug_materials() -> Array:
	return [_ground_mat, _water_mat]

func stats() -> Dictionary:
	return _stats.duplicate()

func _exit_tree() -> void:
	# Chunk tasks capture this node; none of them may still be running when it is
	# freed, or the worker will write into a dead object.
	_shutting_down = true
	for tid in _pending_tasks:
		WorkerThreadPool.wait_for_task_completion(tid)
	_pending_tasks.clear()
	_results.clear()
