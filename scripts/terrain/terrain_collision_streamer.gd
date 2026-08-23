class_name TerrainCollisionStreamer
extends Node3D
## Camera-local physics terrain, independent of visual quadtree residency.
##
## Visual refinement can now split, morph, cancel, or retain parents without
## changing the collision world. A compact fixed-depth tile set follows the
## observer, gives edits revision-safe rebuilds, and uses high-priority workers.

const REFRESH_INTERVAL := 0.10
const MAX_CONCURRENT_BUILDS := 2
const KEY_AXIS_BITS := 20
const KEY_AXIS_MASK := (1 << KEY_AXIS_BITS) - 1

var cfg: GenConfig
var observer := Vec3D.new(0.0, 0.0, 0.0)
var _have_observer := false
var _active := false
var _refresh_left := 0.0
var _entries: Dictionary = {}
var _queue: Array[Dictionary] = []
var _results: Array[Dictionary] = []
var _result_mutex := Mutex.new()
var _pending_tasks: Array[int] = []
var _in_flight := 0
var _shutting_down := false


func _ready() -> void:
	process_priority = 8
	Deltas.region_changed.connect(_on_region_changed)
	Frames.origin_shifted.connect(_on_origin_shifted)


func configure(value: GenConfig) -> void:
	_clear()
	cfg = value
	_refresh_left = 0.0


func set_observer(value: Vec3D, active := true) -> void:
	observer = value
	_have_observer = value.length_sq() > 1.0
	if _active and not active:
		_clear()
	_active = active


func _process(dt: float) -> void:
	_drain_results()
	if cfg == null or not _have_observer or not _active or _shutting_down:
		return
	_refresh_left -= dt
	if _refresh_left <= 0.0:
		_refresh_left = REFRESH_INTERVAL
		_refresh_set()
	_pump()


func _refresh_set() -> void:
	var depth := clampi(cfg.collision_stream_depth, 1, 19)
	var divisions := 1 << depth
	var tile_arc := PI * 0.5 * cfg.planet_radius / float(divisions)
	var radius := maxf(cfg.collision_stream_radius, tile_arc)
	var center := observer.normalized().to_v3()
	var basis := CubeSphere.tangent_basis(center)
	var east: Vector3 = basis[0]
	var north: Vector3 = basis[1]
	var step := tile_arc * 0.72
	var reach := int(ceil(radius / step)) + 1
	var wanted := {}
	for y in range(-reach, reach + 1):
		for x in range(-reach, reach + 1):
			var mx := float(x) * step
			var my := float(y) * step
			if Vector2(mx, my).length() > radius + tile_arc:
				continue
			var d := (center + east * (mx / cfg.planet_radius)
				+ north * (my / cfg.planet_radius)).normalized()
			var address := _address_for_dir(d, divisions)
			var key := _key(address.x, address.y, address.z)
			wanted[key] = address
			if not _entries.has(key):
				var entry := {
					"key": key, "face": address.x, "i": address.y, "j": address.z,
					"depth": depth, "revision": 0, "state": 0,
					"task_id": -1, "body": null, "abandoned": false,
				}
				_entries[key] = entry
				_queue_entry(entry)

	for key_value in _entries.keys():
		var key := int(key_value)
		if wanted.has(key):
			continue
		var old: Dictionary = _entries[key]
		old["abandoned"] = true
		old["revision"] = int(old["revision"]) + 1
		var body: StaticBody3D = old["body"]
		if body != null:
			body.queue_free()
		_entries.erase(key)


func _queue_entry(entry: Dictionary) -> void:
	if int(entry["state"]) != 0 or bool(entry["abandoned"]):
		return
	entry["state"] = 1
	_queue.append(entry)


func _pump() -> void:
	while _in_flight < MAX_CONCURRENT_BUILDS and not _queue.is_empty():
		var entry: Dictionary = _queue.pop_front()
		if bool(entry["abandoned"]) or int(entry["state"]) != 1:
			continue
		_start(entry)


func _start(entry: Dictionary) -> void:
	var revision := int(entry["revision"])
	var face := int(entry["face"])
	var depth := int(entry["depth"])
	var divisions := 1 << depth
	var size := 2.0 / float(divisions)
	var u0 := -1.0 + float(int(entry["i"])) * size
	var v0 := -1.0 + float(int(entry["j"])) * size
	var center_dir := CubeSphere.face_uv_to_dir(face, u0 + size * 0.5, v0 + size * 0.5)
	var ang := size * PI * 0.25 * 1.5
	var snap := Deltas.snapshot_for_bounds(center_dir, ang)
	var n := maxi(cfg.collision_grid, 4)
	var radius := cfg.planet_radius
	var task := func() -> void:
		var built := _build_tile(face, u0, v0, size, n, radius, snap)
		_result_mutex.lock()
		_results.append({"entry": entry, "revision": revision, "built": built})
		_result_mutex.unlock()
	var tid := WorkerThreadPool.add_task(task, true, "asterra_collision")
	entry["task_id"] = tid
	_pending_tasks.append(tid)
	_in_flight += 1


static func _build_tile(face: int, u0: float, v0: float, size: float, n: int,
		radius: float, snap: Dictionary) -> Dictionary:
	var detail := Planet.make_detail()
	# Collision represents the physical terrain function, not whichever visual LOD
	# happens to be resident. The fixed grid itself provides its band limit.
	detail.set_sample_spacing(size * PI * 0.25 * radius / float(n))
	var center := CubeSphere.face_uv_to_dir_d(face, u0 + size * 0.5, v0 + size * 0.5)
	var center_h := Planet.terrain_height(center.to_v3(), detail, snap)
	var pivot := center.mul(radius + center_h)
	var vertices := PackedVector3Array()
	vertices.resize((n + 1) * (n + 1))
	for j in n + 1:
		var v := v0 + size * float(j) / float(n)
		for i in n + 1:
			var u := u0 + size * float(i) / float(n)
			var d := CubeSphere.face_uv_to_dir_d(face, u, v)
			var h := Planet.terrain_height(d.to_v3(), detail, snap)
			var p := d.mul(radius + h)
			vertices[j * (n + 1) + i] = Vector3(
				float(p.x - pivot.x), float(p.y - pivot.y), float(p.z - pivot.z))
	var faces := PackedVector3Array()
	faces.resize(n * n * 6)
	var w := 0
	for j in n:
		for i in n:
			var a := j * (n + 1) + i
			var b := a + 1
			var c := a + n + 1
			var d := c + 1
			faces[w] = vertices[a]
			faces[w + 1] = vertices[c]
			faces[w + 2] = vertices[b]
			faces[w + 3] = vertices[b]
			faces[w + 4] = vertices[c]
			faces[w + 5] = vertices[d]
			w += 6
	return {"pivot": pivot, "faces": faces}


func _drain_results() -> void:
	while true:
		_result_mutex.lock()
		if _results.is_empty():
			_result_mutex.unlock()
			return
		var result: Dictionary = _results.pop_front()
		_result_mutex.unlock()
		var entry: Dictionary = result["entry"]
		var tid := int(entry["task_id"])
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
			_pending_tasks.erase(tid)
			entry["task_id"] = -1
			_in_flight = maxi(0, _in_flight - 1)
		if bool(entry["abandoned"]):
			continue
		if int(result["revision"]) != int(entry["revision"]):
			entry["state"] = 0
			_queue_entry(entry)
			continue
		_install(entry, result["built"])


func _install(entry: Dictionary, built: Dictionary) -> void:
	var previous: StaticBody3D = entry["body"]
	if previous != null:
		previous.queue_free()
	var body := StaticBody3D.new()
	var collision := CollisionShape3D.new()
	var shape := ConcavePolygonShape3D.new()
	shape.set_faces(built["faces"])
	collision.shape = shape
	body.add_child(collision)
	body.set_meta("pivot", built["pivot"])
	body.position = Frames.to_render(built["pivot"])
	add_child(body)
	entry["body"] = body
	entry["state"] = 2


func _on_region_changed(center: Vector3, radius_m: float) -> void:
	if cfg == null:
		return
	for value in _entries.values():
		var entry: Dictionary = value
		var divisions := 1 << int(entry["depth"])
		var size := 2.0 / float(divisions)
		var d := CubeSphere.face_uv_to_dir(int(entry["face"]),
			-1.0 + (float(int(entry["i"])) + 0.5) * size,
			-1.0 + (float(int(entry["j"])) + 0.5) * size)
		var tile_radius := size * PI * 0.25 * cfg.planet_radius
		if d.angle_to(center) * cfg.planet_radius > radius_m + tile_radius:
			continue
		entry["revision"] = int(entry["revision"]) + 1
		if int(entry["state"]) != 1:
			entry["state"] = 0
			_queue_entry(entry)


func _on_origin_shifted(_delta: Vector3) -> void:
	for value in _entries.values():
		var body: StaticBody3D = value["body"]
		if body != null:
			body.position = Frames.to_render(body.get_meta("pivot"))


static func _address_for_dir(d: Vector3, divisions: int) -> Vector3i:
	var fuv := CubeSphere.dir_to_face_uv(d)
	return Vector3i(int(fuv[0]),
		clampi(int(floor((float(fuv[1]) * 0.5 + 0.5) * divisions)), 0, divisions - 1),
		clampi(int(floor((float(fuv[2]) * 0.5 + 0.5) * divisions)), 0, divisions - 1))


static func _key(face: int, i: int, j: int) -> int:
	return (face << (KEY_AXIS_BITS * 2)) | ((j & KEY_AXIS_MASK) << KEY_AXIS_BITS) \
		| (i & KEY_AXIS_MASK)


func _clear() -> void:
	for value in _entries.values():
		var entry: Dictionary = value
		entry["abandoned"] = true
		var body: StaticBody3D = entry["body"]
		if body != null:
			body.queue_free()
	_entries.clear()
	_queue.clear()


func _exit_tree() -> void:
	_shutting_down = true
	for tid in _pending_tasks:
		WorkerThreadPool.wait_for_task_completion(tid)
	_pending_tasks.clear()
	_results.clear()
