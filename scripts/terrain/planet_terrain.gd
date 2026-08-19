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

enum Band { ORBITAL, REGIONAL, LOCAL, EDITABLE }

const BAND_MIN_DEPTH := [0, 5, 10, 14]
const LOD_MORPH_TIME := 0.35
const MORPH_EPSILON := 0.0015
const MORPH_FORMAT := Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT
const FACE_EDGE_EPS := 1e-7

class QuadNode extends RefCounted:
	var face: int
	var depth: int
	var u0: float
	var v0: float
	var size: float
	var center_dir: Vector3
	var center_world: Vec3D
	var arc: float
	## LOD distance reference points. Interior nodes contain only their own
	## centre. A node touching a cube-face boundary also contains the centre of
	## the same-depth patch on the adjacent face. Both sides therefore evaluate
	## the exact same distance set and cannot choose different LODs at the seam.
	## Corner nodes contain the three face centres meeting at that cube corner.
	var lod_centers: Array = []
	var children: Array = []
	var chunk: Node3D = null
	var ground_mi: MeshInstance3D = null
	var water_mi: MeshInstance3D = null
	var body: StaticBody3D = null
	var state: int = 0
	var task_id: int = -1
	var dirty: bool = false
	var abandoned: bool = false
	var fine_vis: float = 0.0
	var drawn: bool = false
	var morph: float = 0.0
	var parent_ref: WeakRef = null

	func parent() -> QuadNode:
		return parent_ref.get_ref() if parent_ref != null else null

	func is_leaf() -> bool:
		return children.is_empty()

var cfg: GenConfig
var observer: Vec3D = Vec3D.new(0, 0, 0)
var forced_depth := -1
const FORCED_NODE_BUDGET := 3000
var max_builds_per_frame := 8
var roots: Array = []

var _results: Array = []
var _res_mutex := Mutex.new()
var _ground_mat: ShaderMaterial
var _water_mat: ShaderMaterial
var _pending_tasks: Array[int] = []
const MAX_TERRAIN_HEIGHT := 6800.0

var _stats := {"chunks": 0, "nodes": 0, "queued": 0, "in_flight": 0, "culled": 0,
	"handoffs": 0, "horizon_deg": 0.0, "horizon_km": 0.0}
var _obs_dir := Vector3(1, 0, 0)
var _horizon_angle := 0.0
var _in_flight := 0
var _shutting_down := false
var _dt := 0.0
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
	_water_mat.set_shader_parameter("u_cell_per_metre",
		1.0 / (cfg.lod_split_factor * float(cfg.chunk_grid)))
	for mat in [_ground_mat, _water_mat]:
		mat.set_shader_parameter("u_planet_radius", cfg.planet_radius)
		mat.set_shader_parameter("u_atmosphere_height", cfg.atmosphere_height)
		mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	roots.clear()
	for face in 6:
		roots.append(_make_node(face, 0, -1.0, -1.0, 2.0))

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
	var cu := u0 + size * 0.5
	var cv := v0 + size * 0.5
	q.center_dir = CubeSphere.face_uv_to_dir(face, cu, cv)
	q.center_world = _surface_reference(q.center_dir)
	q.arc = size * (PI * 0.25) * cfg.planet_radius
	q.lod_centers = [q.center_world]

	# The six roots are a special case: each one touches four cube edges. Give
	# every root the same six reference centres so all visible roots make the same
	# coarse LOD decision regardless of which face the observer happens to be over.
	if depth == 0:
		q.lod_centers.clear()
		for f in 6:
			q.lod_centers.append(_surface_reference(CubeSphere.AXIS[f]))
		return q

	# For an edge patch, stepping half a patch beyond the face boundary lands at
	# the centre of the corresponding same-depth patch on the adjacent face. The
	# equi-angular mapping is continuous under this extension. At a cube corner we
	# add both adjacent partners, producing the same three-centre set on all three
	# faces that meet there.
	var half := size * 0.5
	if absf(u0 + 1.0) <= FACE_EDGE_EPS:
		_add_lod_partner(q, CubeSphere.face_uv_to_dir(face, -1.0 - half, cv))
	if absf((u0 + size) - 1.0) <= FACE_EDGE_EPS:
		_add_lod_partner(q, CubeSphere.face_uv_to_dir(face, 1.0 + half, cv))
	if absf(v0 + 1.0) <= FACE_EDGE_EPS:
		_add_lod_partner(q, CubeSphere.face_uv_to_dir(face, cu, -1.0 - half))
	if absf((v0 + size) - 1.0) <= FACE_EDGE_EPS:
		_add_lod_partner(q, CubeSphere.face_uv_to_dir(face, cu, 1.0 + half))
	return q

func _surface_reference(d: Vector3) -> Vec3D:
	var h := Planet.macro_height(d)
	var r := cfg.planet_radius + h
	return Vec3D.new(d.x * r, d.y * r, d.z * r)

func _add_lod_partner(node: QuadNode, d: Vector3) -> void:
	var p := _surface_reference(d)
	# Corner bookkeeping can generate the same partner twice through numerical
	# ties. It is harmless, but avoiding duplicates keeps the distance loop tiny.
	for existing in node.lod_centers:
		if (existing as Vec3D).distance_to(p) < 0.01:
			return
	node.lod_centers.append(p)

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
	_drain_results()
	_stats["in_flight"] = _in_flight

const HORIZON_CULL_HYSTERESIS := 1.3

func _update(node: QuadNode, hidden: bool, morph: float) -> void:
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

	# Interior patches use their own centre. Along a cube edge, the corresponding
	# patches on both faces store the same pair of physical centres; at a cube
	# corner all three store the same triplet. Taking the minimum observer distance
	# across that shared set makes the split/merge decision exactly face-invariant.
	var center_dist := 1e30
	for p in node.lod_centers:
		center_dist = minf(center_dist, observer.distance_to(p))
	var dist := center_dist - node.arc * 0.6

	var want_split: bool
	if forced_depth >= 0:
		want_split = node.depth < forced_depth and _stats["nodes"] < FORCED_NODE_BUDGET
	else:
		want_split = node.depth < cfg.quadtree_max_depth and dist < node.arc * cfg.lod_split_factor

	if node.is_leaf():
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
		if node.chunk == null:
			if _morph_lock == 0 and (node.state == 0 or node.dirty):
				_request(node)
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

func _apply_chunk(node: QuadNode, drawn: bool, morph: float) -> void:
	if node.chunk == null:
		return
	if node.drawn != drawn:
		node.drawn = drawn
		node.chunk.visible = drawn
		_set_body_enabled(node, drawn)
	if not drawn:
		return
	if absf(node.morph - morph) > 1e-4:
		node.morph = morph
		if node.ground_mi != null:
			node.ground_mi.set_instance_shader_parameter("morph", morph)
		if node.water_mi != null:
			node.water_mi.set_instance_shader_parameter("morph", morph)

func _subtree_morphing(node: QuadNode) -> bool:
	for c in node.children:
		if not c.is_leaf() and c.fine_vis > 0.0 and c.fine_vis < 1.0:
			return true
		if _subtree_morphing(c):
			return true
	return false

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
	node.fine_vis = 0.0
	for c in node.children:
		c.parent_ref = weakref(node)

func _collapse(node: QuadNode) -> void:
	for c in node.children:
		_collapse(c)
		_free_chunk(c)
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
		node.fine_vis = 1.0

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

func _push_origin() -> void:
	var o := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_ground_mat.set_shader_parameter("u_origin", o)
	_water_mat.set_shader_parameter("u_origin", o)

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

func debug_materials() -> Array:
	return [_ground_mat, _water_mat]

func stats() -> Dictionary:
	return _stats.duplicate()

func _exit_tree() -> void:
	_shutting_down = true
	for tid in _pending_tasks:
		WorkerThreadPool.wait_for_task_completion(tid)
	_pending_tasks.clear()
	_results.clear()
