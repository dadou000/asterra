class_name TerrainCollisionStreamer
extends Node3D
## Camera-local physics terrain, independent of visual quadtree residency.
##
## Collision consumes the same immutable GroundHeightStore pages as the visual
## geometry clipmap. Source pages are requested before a collision worker starts,
## preventing the worker from becoming an accidental first-time terrain baker.
## The active bubble is biased in the direction of travel so fast vehicles get
## physical ground ahead of them instead of only around their previous position.

const REFRESH_INTERVAL := 0.10
const MAX_CONCURRENT_BUILDS := 2
const MAX_SOURCE_CHECKS_PER_FRAME := 12
const KEY_AXIS_BITS := 20
const KEY_AXIS_MASK := (1 << KEY_AXIS_BITS) - 1
const HEIGHT_PRIORITY_COLLISION := -1000.0
const SOURCE_PROBE_STEPS := 2 # 3x3 probes: corners, edge middles, centre
const MOTION_FILTER := 0.30
const TELEPORT_SPEED_MPS := 2200.0
const COLLISION_LOOKAHEAD_SECONDS := 1.6
const COLLISION_LOOKAHEAD_MAX_M := 240.0

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
var _refreshes := 0
var _builds_started := 0
var _builds_installed := 0
var _invalidations := 0
var _retired := 0

var _have_motion_sample := false
var _last_motion_observer := Vec3D.new(0.0, 0.0, 0.0)
var _motion_dir := Vector3.ZERO
var _motion_speed := 0.0
var _priority_point := Vec3D.new(0.0, 0.0, 0.0)


func _ready() -> void:
	process_priority = 8
	Deltas.region_changed.connect(_on_region_changed)
	Frames.origin_shifted.connect(_on_origin_shifted)


func configure(value: GenConfig) -> void:
	_clear()
	cfg = value
	_refresh_left = 0.0
	_have_motion_sample = false
	_motion_speed = 0.0
	_refreshes = 0
	_builds_started = 0
	_builds_installed = 0
	_invalidations = 0
	_retired = 0


func set_observer(value: Vec3D, active := true) -> void:
	observer = value
	_have_observer = value.length_sq() > 1.0
	if _active and not active:
		_clear()
	_active = active


func stats() -> Dictionary:
	var resident := 0
	var queued := 0
	for value in _entries.values():
		var entry: Dictionary = value
		if int(entry["state"]) == 2 and entry["body"] != null:
			resident += 1
		elif int(entry["state"]) == 1 and int(entry["task_id"]) < 0:
			queued += 1
	return {
		"active": _active,
		"entries": _entries.size(),
		"resident": resident,
		"queued": queued,
		"in_flight": _in_flight,
		"missing": maxi(_entries.size() - resident - queued - _in_flight, 0),
		"refreshes": _refreshes,
		"builds_started": _builds_started,
		"builds_installed": _builds_installed,
		"invalidations": _invalidations,
		"retired": _retired,
	}


func _process(dt: float) -> void:
	_drain_results()
	if cfg == null or not _have_observer or not _active or _shutting_down:
		return
	_update_motion(dt)
	_refresh_left -= dt
	if _refresh_left <= 0.0:
		_refresh_left = REFRESH_INTERVAL
		_refresh_set()
	_pump()


func _update_motion(dt: float) -> void:
	if not _have_motion_sample:
		_last_motion_observer = observer.dup()
		_have_motion_sample = true
		_motion_speed = 0.0
		_motion_dir = Vector3.ZERO
		return

	var delta := observer.sub(_last_motion_observer)
	_last_motion_observer = observer.dup()
	var distance := delta.length()
	if distance <= 1e-4:
		_motion_speed = lerpf(_motion_speed, 0.0, MOTION_FILTER)
		return
	var safe_dt := maxf(dt, 1.0 / 240.0)
	var instant_speed := distance / safe_dt
	if instant_speed > TELEPORT_SPEED_MPS:
		_motion_speed = 0.0
		_motion_dir = Vector3.ZERO
		return

	var up := observer.normalized().to_v3()
	var raw_dir := delta.mul(1.0 / distance).to_v3()
	var tangent := raw_dir - up * raw_dir.dot(up)
	if tangent.length_squared() > 1e-8:
		_motion_dir = tangent.normalized()
	_motion_speed = lerpf(_motion_speed, instant_speed, MOTION_FILTER)


func _refresh_set() -> void:
	_refreshes += 1
	var depth := clampi(cfg.collision_stream_depth, 1, 19)
	var divisions := 1 << depth
	var tile_arc := PI * 0.5 * cfg.planet_radius / float(divisions)
	var base_radius := maxf(cfg.collision_stream_radius, tile_arc)
	var current_center := observer.normalized().to_v3()

	var lookahead := 0.0
	if _motion_speed > 1.0 and _motion_dir.length_squared() > 0.5:
		lookahead = minf(_motion_speed * COLLISION_LOOKAHEAD_SECONDS,
			COLLISION_LOOKAHEAD_MAX_M)
	var bias := lookahead * 0.5
	var center := current_center
	if bias > 0.0:
		center = (current_center + _motion_dir * (bias / cfg.planet_radius)).normalized()
	var radius := base_radius + bias
	_priority_point = observer.dup()
	if lookahead > 0.0:
		_priority_point = observer.add(Vec3D.from_v3(_motion_dir).mul(lookahead))

	var tangent := CubeSphere.tangent_basis(center)
	var east: Vector3 = tangent[0]
	var north: Vector3 = tangent[1]
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
					"cancel": null,
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
		var cancel: TerrainBuildCancel = old.get("cancel")
		if cancel != null:
			cancel.cancel()
		var body: StaticBody3D = old["body"]
		if body != null:
			body.queue_free()
		_entries.erase(key)
		_retired += 1

	_queue.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return _entry_distance_sq(a) < _entry_distance_sq(b))


func _queue_entry(entry: Dictionary) -> void:
	if int(entry["state"]) != 0 or bool(entry["abandoned"]):
		return
	entry["state"] = 1
	_request_entry_source(entry)
	_queue.append(entry)


func _entry_distance_sq(entry: Dictionary) -> float:
	var divisions := 1 << int(entry["depth"])
	var size := 2.0 / float(divisions)
	var d := CubeSphere.face_uv_to_dir(int(entry["face"]),
		-1.0 + (float(int(entry["i"])) + 0.5) * size,
		-1.0 + (float(int(entry["j"])) + 0.5) * size)
	var p := Vec3D.from_v3(d).mul(cfg.planet_radius)
	return p.sub(_priority_point).length_sq()


func _entry_cache_level(entry: Dictionary) -> int:
	var depth := int(entry["depth"])
	var divisions := 1 << depth
	var size := 2.0 / float(divisions)
	var n := maxi(cfg.collision_grid, 4)
	var sample_spacing := size * PI * 0.25 * cfg.planet_radius / float(n)
	return GroundHeightStore.level_for_spacing(sample_spacing)


func _entry_probe_dirs(entry: Dictionary) -> Array[Vector3]:
	var out: Array[Vector3] = []
	var depth := int(entry["depth"])
	var divisions := 1 << depth
	var size := 2.0 / float(divisions)
	var u0 := -1.0 + float(int(entry["i"])) * size
	var v0 := -1.0 + float(int(entry["j"])) * size
	var face := int(entry["face"])
	for py in SOURCE_PROBE_STEPS + 1:
		var v := v0 + size * float(py) / float(SOURCE_PROBE_STEPS)
		for px in SOURCE_PROBE_STEPS + 1:
			var u := u0 + size * float(px) / float(SOURCE_PROBE_STEPS)
			out.append(CubeSphere.face_uv_to_dir(face, u, v))
	return out


func _request_entry_source(entry: Dictionary) -> void:
	if not Planet.ready_state:
		return
	var level := _entry_cache_level(entry)
	for d in _entry_probe_dirs(entry):
		GroundHeightStore.request_sample(d, level, HEIGHT_PRIORITY_COLLISION)


func _entry_source_ready(entry: Dictionary) -> bool:
	if not Planet.ready_state:
		return false
	var level := _entry_cache_level(entry)
	for d in _entry_probe_dirs(entry):
		if not GroundHeightStore.is_sample_resident(d, level):
			return false
	return true


func _pump() -> void:
	# A large high-speed collision bubble can contain many waiting entries. Check a
	# fixed number per rendered frame so RAM-residency probes themselves cannot
	# become a new low-altitude CPU spike.
	var attempts := mini(_queue.size(), MAX_SOURCE_CHECKS_PER_FRAME)
	while _in_flight < MAX_CONCURRENT_BUILDS and attempts > 0 and not _queue.is_empty():
		var entry: Dictionary = _queue.pop_front()
		attempts -= 1
		if bool(entry["abandoned"]) or int(entry["state"]) != 1:
			continue
		if not _entry_source_ready(entry):
			_request_entry_source(entry)
			_queue.append(entry)
			continue
		_start(entry)


func _start(entry: Dictionary) -> void:
	_builds_started += 1
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
	var cancel := TerrainBuildCancel.new()
	entry["cancel"] = cancel
	var task := func() -> void:
		var built := _build_tile(face, u0, v0, size, n, radius, snap, cancel)
		_result_mutex.lock()
		_results.append({"entry": entry, "revision": revision, "built": built})
		_result_mutex.unlock()
	var tid := WorkerThreadPool.add_task(task, true, "asterra_collision")
	entry["task_id"] = tid
	_pending_tasks.append(tid)
	_in_flight += 1


static func _build_tile(face: int, u0: float, v0: float, size: float, n: int,
		radius: float, snap: Dictionary, cancel: TerrainBuildCancel = null) -> Dictionary:
	if cancel != null and cancel.is_cancelled():
		return {"cancelled": true}

	var sample_spacing := size * PI * 0.25 * radius / float(n)
	var cache_level := GroundHeightStore.level_for_spacing(sample_spacing)
	var center := CubeSphere.face_uv_to_dir_d(face, u0 + size * 0.5, v0 + size * 0.5)
	var center_h := GroundHeightStore.sample_height(center.to_v3(), cache_level, snap)
	var pivot := center.mul(radius + center_h)
	var vertices := PackedVector3Array()
	vertices.resize((n + 1) * (n + 1))
	for j in n + 1:
		if cancel != null and cancel.is_cancelled():
			return {"cancelled": true}
		var v := v0 + size * float(j) / float(n)
		for i in n + 1:
			var u := u0 + size * float(i) / float(n)
			var d := CubeSphere.face_uv_to_dir_d(face, u, v)
			var h := GroundHeightStore.sample_height(d.to_v3(), cache_level, snap)
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
			entry["cancel"] = null
			_in_flight = maxi(0, _in_flight - 1)
		if bool(entry["abandoned"]):
			continue
		if int(result["revision"]) != int(entry["revision"]):
			entry["state"] = 0
			_queue_entry(entry)
			continue
		if bool((result["built"] as Dictionary).get("cancelled", false)):
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
	_builds_installed += 1


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
		_invalidations += 1
		entry["revision"] = int(entry["revision"]) + 1
		var cancel: TerrainBuildCancel = entry.get("cancel")
		if cancel != null:
			cancel.cancel()
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
		var cancel: TerrainBuildCancel = entry.get("cancel")
		if cancel != null:
			cancel.cancel()
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
