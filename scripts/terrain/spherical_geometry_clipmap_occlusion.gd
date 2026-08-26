extends "res://scripts/terrain/spherical_geometry_clipmap_horizon_safe.gd"
## Conservative depth-buffer occlusion for far terrain-ring suffixes.
##
## L0 and nearby rings are never hidden by this layer. Logical L7+ ring-sector
## bounds are tested asynchronously against the previous resolved scene depth. The
## existing MultiMesh layout is preserved: only a contiguous occluded suffix may
## reduce visible_instance_count, so no instance repacking or custom-data rewrite is
## required.

const OCCLUSION_MIN_LEVEL: int = 7
const OCCLUSION_CONFIRM_FRAMES: int = 2
const OCCLUSION_RESULT_MAX_AGE_MS: int = 260
const OCCLUSION_MAX_CAMERA_DELTA_M: float = 42.0
const OCCLUSION_MAX_CAMERA_ANGLE_RAD: float = 0.14
const OCCLUSION_ALTITUDE_GUARD_M: float = 1200.0
const OCCLUSION_SPHERE_INFLATE: float = 1.10
const OCCLUSION_SPHERE_EXTRA_M: float = 250.0
const OCCLUSION_NEAR_EXACT_MAX_M: float = 90000.0
const OCCLUSION_MAX_PROJECTED_THETA: float = 1.62
const OCCLUSION_CANDIDATE_COUNT: int = SECTOR_COUNT * LEVEL_COUNT

var _occlusion_effect: TerrainOcclusionCompositorEffect
var _occlusion_generation: int = 1
var _occlusion_last_anchor_dir := Vector3.ZERO
var _occlusion_last_min_level: int = -1
var _occlusion_last_max_level: int = -1
var _occlusion_level_centers: Dictionary = {}
var _occlusion_streak := PackedByteArray()
var _occlusion_confirmed := PackedByteArray()
var _occlusion_last_snapshot_msec: int = -1
var _occlusion_last_valid_msec: int = -1
var _occlusion_culled_instances: int = 0
var _occlusion_candidate_rebuilds: int = 0
var _occlusion_rejected_snapshots: int = 0
var _occlusion_installed := false


func _ready() -> void:
	_occlusion_streak.resize(OCCLUSION_CANDIDATE_COUNT)
	_occlusion_confirmed.resize(OCCLUSION_CANDIDATE_COUNT)
	_clear_occlusion_history()
	super._ready()

	_occlusion_effect = TerrainOcclusionCompositorEffect.new()
	if _occlusion_effect != null and _occlusion_effect.is_ready():
		if not get_tree().node_added.is_connected(_on_occlusion_node_added):
			get_tree().node_added.connect(_on_occlusion_node_added)
		call_deferred("_scan_occlusion_environments")


func _process(dt: float) -> void:
	# Do not replace any production clipmap work. Occlusion publication happens only
	# after the proven terrain/cache/handoff process has produced final state.
	super._process(dt)
	if _occlusion_effect == null or not _occlusion_effect.is_ready():
		return
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_occlusion_effect.set_floating_origin(origin)
	_update_occlusion_candidates(origin)


func _on_occlusion_node_added(node: Node) -> void:
	if node is WorldEnvironment:
		call_deferred("_try_install_occlusion_effect", node)


func _scan_occlusion_environments() -> void:
	_scan_occlusion_node(get_tree().root)


func _scan_occlusion_node(node: Node) -> void:
	if node is WorldEnvironment:
		_try_install_occlusion_effect(node as WorldEnvironment)
	for child: Node in node.get_children():
		_scan_occlusion_node(child)


func _try_install_occlusion_effect(world_environment: WorldEnvironment) -> void:
	if world_environment == null or _occlusion_effect == null \
			or not _occlusion_effect.is_ready():
		return
	var compositor: Compositor = world_environment.compositor
	if compositor == null:
		compositor = Compositor.new()
	var effects: Array[CompositorEffect] = compositor.compositor_effects
	if not effects.has(_occlusion_effect):
		effects.append(_occlusion_effect)
		compositor.compositor_effects = effects
	world_environment.compositor = compositor
	_occlusion_installed = true


func _update_occlusion_candidates(origin: Vector3) -> void:
	if Planet.cfg == null or not Planet.ready_state or not _have_anchor:
		return

	var changed: bool = _occlusion_last_min_level != _active_min_level \
		or _occlusion_last_max_level != _active_max_level \
		or _occlusion_last_anchor_dir.distance_squared_to(_anchor_dir) > 1e-12

	var next_centers: Dictionary = {}
	var first_level: int = maxi(OCCLUSION_MIN_LEVEL, _active_min_level + 1)
	for level: int in range(first_level, _active_max_level + 1):
		var spacing: float = _base_spacing * pow(2.0, float(level))
		var center := Vector2(
			round(_center_plane.x / spacing) * spacing,
			round(_center_plane.y / spacing) * spacing)
		next_centers[level] = center
		if not _occlusion_level_centers.has(level) \
				or (next_centers[level] as Vector2).distance_squared_to(
					_occlusion_level_centers[level]) > 1e-6:
			changed = true

	if next_centers.size() != _occlusion_level_centers.size():
		changed = true
	if not changed:
		return

	_occlusion_generation += 1
	_occlusion_candidate_rebuilds += 1
	_occlusion_last_anchor_dir = _anchor_dir
	_occlusion_last_min_level = _active_min_level
	_occlusion_last_max_level = _active_max_level
	_occlusion_level_centers = next_centers
	_clear_occlusion_history()

	var spheres := PackedFloat32Array()
	spheres.resize(OCCLUSION_CANDIDATE_COUNT * 4)
	for candidate: int in OCCLUSION_CANDIDATE_COUNT:
		spheres[candidate * 4 + 3] = -1.0

	for level: int in range(first_level, _active_max_level + 1):
		for sector: int in SECTOR_COUNT:
			var sphere: Vector4 = _build_ring_sector_sphere(level, sector)
			var candidate: int = _candidate_index(sector, level)
			var base: int = candidate * 4
			spheres[base] = sphere.x
			spheres[base + 1] = sphere.y
			spheres[base + 2] = sphere.z
			spheres[base + 3] = sphere.w

	_occlusion_effect.set_candidates(spheres, _occlusion_generation, origin)


func _build_ring_sector_sphere(level: int, sector: int) -> Vector4:
	var spacing: float = _base_spacing * pow(2.0, float(level))
	var level_center: Vector2 = _occlusion_level_centers.get(level, Vector2.ZERO)
	var inner_radius: float = float(RING_INNER_HALF_CELLS) * spacing
	var outer_radius: float = float(HALF_CELLS) * spacing
	var middle_radius: float = (inner_radius + outer_radius) * 0.5
	var angle0: float = float(sector) * TAU / float(SECTOR_COUNT)
	var angle1: float = float(sector + 1) * TAU / float(SECTOR_COUNT)
	var angle_mid: float = (angle0 + angle1) * 0.5

	var cfg: GenConfig = Planet.cfg
	var relief_guard: float = OCCLUSION_ALTITUDE_GUARD_M \
		+ absf(cfg.detail_amplitude) * 2.0
	var min_altitude: float = cfg.abyssal_depth - relief_guard
	var max_altitude: float = cfg.max_uplift \
		+ cfg.uplift_per_step * float(cfg.erosion_iterations) + relief_guard

	var points: Array[Vector3] = []
	for radius_m: float in [inner_radius, middle_radius, outer_radius]:
		for angle: float in [angle0, angle_mid, angle1]:
			var local_offset := Vector2(cos(angle), sin(angle)) * radius_m
			var offset: Vector2 = level_center + local_offset
			points.append(_occlusion_surface_world(offset, min_altitude))
			points.append(_occlusion_surface_world(offset, max_altitude))

	var center := Vector3.ZERO
	for point: Vector3 in points:
		center += point
	center /= maxf(float(points.size()), 1.0)
	var radius: float = 0.0
	for point: Vector3 in points:
		radius = maxf(radius, center.distance_to(point))
	# Inflate beyond the sampled wedge corners/midpoints. This deliberately makes
	# occlusion less aggressive in exchange for never under-bounding curved terrain.
	radius = radius * OCCLUSION_SPHERE_INFLATE \
		+ maxf(OCCLUSION_SPHERE_EXTRA_M, spacing * 2.0)
	return Vector4(center.x, center.y, center.z, radius)


func _occlusion_surface_world(offset_m: Vector2, altitude_m: float) -> Vector3:
	var radius: float = maxf(Planet.cfg.planet_radius, 1.0)
	var arc: float = offset_m.length()
	var direction: Vector3
	if arc <= 1e-5:
		direction = _anchor_dir
	elif arc <= OCCLUSION_NEAR_EXACT_MAX_M:
		direction = (_anchor_dir
			+ _anchor_right * (offset_m.x / radius)
			+ _anchor_up * (offset_m.y / radius)).normalized()
	else:
		var theta: float = minf(arc / radius,
			minf(RENDER_HEMISPHERE_CAP_RAD * 1.02, OCCLUSION_MAX_PROJECTED_THETA))
		var tangent: Vector3 = (
			_anchor_right * offset_m.x + _anchor_up * offset_m.y).normalized()
		direction = (_anchor_dir * cos(theta) + tangent * sin(theta)).normalized()
	return direction * (radius + altitude_m)


func _candidate_index(sector: int, logical_level: int) -> int:
	return sector * LEVEL_COUNT + logical_level


func _clear_occlusion_history() -> void:
	for i: int in _occlusion_streak.size():
		_occlusion_streak[i] = 0
		_occlusion_confirmed[i] = 0
	_occlusion_last_valid_msec = -1


func _refresh_occlusion_results(camera: Camera3D) -> void:
	if _occlusion_effect == null or not _occlusion_effect.is_ready() or camera == null:
		_clear_occlusion_history()
		return
	var snapshot: Dictionary = _occlusion_effect.get_snapshot()
	if snapshot.is_empty():
		return
	var snapshot_msec: int = int(snapshot.get("received_msec", -1))
	if snapshot_msec <= _occlusion_last_snapshot_msec:
		return
	_occlusion_last_snapshot_msec = snapshot_msec

	if int(snapshot.get("generation", -1)) != _occlusion_generation:
		_occlusion_rejected_snapshots += 1
		_clear_occlusion_history()
		return
	var now: int = Time.get_ticks_msec()
	if now - snapshot_msec > OCCLUSION_RESULT_MAX_AGE_MS:
		_occlusion_rejected_snapshots += 1
		_clear_occlusion_history()
		return

	var sampled_world: Vector3 = snapshot.get("camera_world", Vector3.ZERO)
	var current_world: Vector3 = Frames.to_world(camera.global_position).to_v3()
	var sampled_forward: Vector3 = snapshot.get("camera_forward", Vector3.ZERO)
	var current_forward: Vector3 = -camera.global_transform.basis.z.normalized()
	if sampled_world.distance_to(current_world) > OCCLUSION_MAX_CAMERA_DELTA_M \
			or sampled_forward.length_squared() < 0.5 \
			or sampled_forward.normalized().dot(current_forward) < cos(OCCLUSION_MAX_CAMERA_ANGLE_RAD):
		_occlusion_rejected_snapshots += 1
		_clear_occlusion_history()
		return

	var mask: PackedByteArray = snapshot.get("occluded", PackedByteArray())
	if mask.size() < OCCLUSION_CANDIDATE_COUNT:
		_occlusion_rejected_snapshots += 1
		_clear_occlusion_history()
		return

	for sector: int in SECTOR_COUNT:
		for level: int in range(OCCLUSION_MIN_LEVEL, MAX_LEVEL + 1):
			var candidate: int = _candidate_index(sector, level)
			if mask[candidate] != 0:
				_occlusion_streak[candidate] = mini(
					int(_occlusion_streak[candidate]) + 1, 255)
				if int(_occlusion_streak[candidate]) >= OCCLUSION_CONFIRM_FRAMES:
					_occlusion_confirmed[candidate] = 1
			else:
				# Visibility recovers immediately; only hiding is hysteretic.
				_occlusion_streak[candidate] = 0
				_occlusion_confirmed[candidate] = 0
	_occlusion_last_valid_msec = now


func _update_sector_visibility() -> void:
	super._update_sector_visibility()
	if _view_surface_culled or _debug_side_cut:
		return
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		return
	_refresh_occlusion_results(camera)

	var now: int = Time.get_ticks_msec()
	if _occlusion_last_valid_msec >= 0 \
			and now - _occlusion_last_valid_msec > OCCLUSION_RESULT_MAX_AGE_MS:
		_clear_occlusion_history()

	var ring_count: int = mini(_physical_ring_count, _active_ring_count())
	_occlusion_culled_instances = 0
	for sector: int in SECTOR_COUNT:
		var batch: MultiMeshInstance3D = _sector_batches[sector]
		if batch.multimesh == null:
			continue
		if not batch.visible:
			batch.multimesh.visible_instance_count = 0
			continue

		var prefix: int = ring_count
		# MultiMesh slots are ordered near -> far. Only remove a contiguous confirmed
		# occluded suffix; an outer visible level keeps every inner slot intact.
		while prefix > 0:
			var logical_level: int = _active_min_level + prefix
			if logical_level < OCCLUSION_MIN_LEVEL:
				break
			var candidate: int = _candidate_index(sector, logical_level)
			if _occlusion_confirmed[candidate] == 0:
				break
			prefix -= 1
		batch.multimesh.visible_instance_count = prefix
		_occlusion_culled_instances += ring_count - prefix


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_occlusion"] = _occlusion_installed \
		and _occlusion_effect != null and _occlusion_effect.is_ready()
	out["terrain_occlusion_min_level"] = OCCLUSION_MIN_LEVEL
	out["terrain_occlusion_generation"] = _occlusion_generation
	out["terrain_occlusion_culled_ring_instances"] = _occlusion_culled_instances
	out["terrain_occlusion_candidate_rebuilds"] = _occlusion_candidate_rebuilds
	out["terrain_occlusion_rejected_snapshots"] = _occlusion_rejected_snapshots
	out["terrain_occlusion_l0"] = false
	out["terrain_occlusion_fail_open"] = true
	if _occlusion_effect != null:
		var effect_stats: Dictionary = _occlusion_effect.stats()
		out["terrain_occlusion_dispatch_frames"] = effect_stats.get("dispatch_frames", 0)
		out["terrain_occlusion_readbacks"] = effect_stats.get("readback_requests", 0)
		out["terrain_occlusion_readback_failures"] = effect_stats.get("readback_failures", 0)
		out["terrain_occlusion_tile_res"] = "%dx%d" % [
			int(effect_stats.get("tile_width", 0)), int(effect_stats.get("tile_height", 0))]
	return out
