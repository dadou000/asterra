class_name OceanBuoyancy3D
extends Node
## GPU-driven buoyancy/hydrodynamics component for a RigidBody3D parent.
##
## Add this below a RigidBody3D and define probe_points in body-local metres.
## The component samples detailed bathymetry only a few times per second, models
## it locally as a plane, batches all wave queries through OceanGPUPhysics, then
## applies buoyancy and water drag at the probe offsets. Applying forces at each
## probe naturally produces pitch/roll torque without a custom rigid-body solver.
##
## When Planet Studio exposes an authored-water query, the same probes transparently
## prefer lake/river surfaces over the ocean result. This keeps one force solver:
## lakes contribute fresh-water buoyancy and rivers additionally contribute their
## authored tangent current velocity. Outside authored features the original GPU
## ocean path is unchanged.

@export var probe_points: Array[Vector3] = [
	Vector3(-1.0, -0.35, -1.8), Vector3(1.0, -0.35, -1.8),
	Vector3(-1.0, -0.35, 0.0), Vector3(1.0, -0.35, 0.0),
	Vector3(-1.0, -0.35, 1.8), Vector3(1.0, -0.35, 1.8),
]
@export_range(0.01, 10000.0, 0.01) var displaced_volume_m3: float = 6.0
@export_range(0.05, 20.0, 0.01) var probe_depth_m: float = 0.8
@export_range(0.0, 1000.0, 0.1) var drag_area_m2: float = 5.0
@export_range(0.0, 5.0, 0.01) var drag_coefficient: float = 0.9
@export_range(100.0, 1400.0, 1.0) var water_density: float = 1025.0
@export_range(100.0, 1400.0, 1.0) var authored_water_density: float = 1000.0
@export_range(1.0, 30.0, 0.1) var max_drag_accel: float = 10.0
@export_range(1.0, 30.0, 0.1) var bathymetry_hz: float = 6.0
@export_range(2.0, 100.0, 0.5) var bathymetry_sample_m: float = 16.0
@export_range(0.0, 3.0, 0.01) var wave_scale: float = 1.0

const GRAVITY_M_S2 := 9.81
const AUTHORED_WATER_GROUP: StringName = &"authored_water_query"

var _body: RigidBody3D
var _gpu: OceanGPUPhysics
var _detail: TerrainDetail
var _authored_water_query: Node
var _bathy_left := 0.0
var _bathy_valid := false
var _bathy_center_dir := Vector3.RIGHT
var _bathy_center_height := -1000.0
var _bathy_gradient := Vector3.ZERO
var _bathy_landward := Vector3(0.0, 0.0, -1.0)
var _bathy_shore_distance := -6000.0

var _request_context: Dictionary = {}
var _ready_results: Array[Dictionary] = []
var _ready_probe_count := 0
var _last_submerged_fraction := 0.0
var _last_authored_probe_count := 0


func _ready() -> void:
	_body = get_parent() as RigidBody3D
	if _body == null:
		push_error("OceanBuoyancy3D must be a child of RigidBody3D")
		set_physics_process(false)
		return
	var ocean := get_node_or_null("/root/OceanSystem")
	if ocean != null:
		_gpu = ocean.get_node_or_null("OceanGPUPhysics") as OceanGPUPhysics
	if _gpu == null:
		push_warning("OceanBuoyancy3D: OceanGPUPhysics is unavailable")
		return
	_gpu.batch_ready.connect(_on_gpu_batch_ready)
	if Planet.ready_state:
		_detail = Planet.make_detail()


func _physics_process(dt: float) -> void:
	if _body == null or _gpu == null or not _gpu.available() or probe_points.is_empty():
		return
	_resolve_authored_water_query()
	if _detail == null and Planet.ready_state:
		_detail = Planet.make_detail()
	if _detail == null:
		return

	_bathy_left -= dt
	if _bathy_left <= 0.0 or not _bathy_valid:
		_bathy_left = 1.0 / maxf(bathymetry_hz, 1.0)
		_update_bathymetry()

	if not _ready_results.is_empty():
		_apply_water_forces(_ready_results, mini(_ready_probe_count, probe_points.size()))

	_queue_gpu_probe_batch()


func _resolve_authored_water_query() -> void:
	if _authored_water_query != null and is_instance_valid(_authored_water_query):
		return
	_authored_water_query = null
	var candidate: Node = get_tree().get_first_node_in_group(AUTHORED_WATER_GROUP)
	if candidate != null and candidate.has_method("query_render_point"):
		_authored_water_query = candidate


func _update_bathymetry() -> void:
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var center_planet := _body.global_position + origin
	if center_planet.length_squared() < 1.0:
		_bathy_valid = false
		return
	var d := center_planet.normalized()
	var tangent: Array = CubeSphere.tangent_basis(d)
	var right: Vector3 = tangent[0]
	var up_tangent: Vector3 = tangent[1]
	var s := maxf(bathymetry_sample_m, 2.0)
	var h := Planet.terrain_height(d, _detail)
	var hx0 := Planet.terrain_height(_offset_direction(d, right, -s), _detail)
	var hx1 := Planet.terrain_height(_offset_direction(d, right, s), _detail)
	var hy0 := Planet.terrain_height(_offset_direction(d, up_tangent, -s), _detail)
	var hy1 := Planet.terrain_height(_offset_direction(d, up_tangent, s), _detail)
	var gx := (hx1 - hx0) / (2.0 * s)
	var gy := (hy1 - hy0) / (2.0 * s)
	var gradient := right * gx + up_tangent * gy
	var gm := gradient.length()

	_bathy_center_dir = d
	_bathy_center_height = h
	_bathy_gradient = gradient
	_bathy_landward = gradient / gm if gm > 1e-4 else right
	_bathy_shore_distance = clampf(h / maxf(gm, 0.015), -6000.0, 6000.0)
	_bathy_valid = true


func _queue_gpu_probe_batch() -> void:
	if not _bathy_valid:
		return
	var count := mini(probe_points.size(), _gpu.max_queries())
	var points := PackedVector3Array()
	var depths := PackedFloat32Array()
	var coast_dirs := PackedVector3Array()
	var shore_distances := PackedFloat32Array()
	points.resize(count)
	depths.resize(count)
	coast_dirs.resize(count)
	shore_distances.resize(count)

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var center_surface := _bathy_center_dir * Planet.cfg.planet_radius
	for i in count:
		var render_point := _body.to_global(probe_points[i])
		var planet_point := render_point + origin
		var surface_point := planet_point.normalized() * Planet.cfg.planet_radius
		var tangent_offset := surface_point - center_surface
		var local_ground := _bathy_center_height + tangent_offset.dot(_bathy_gradient)
		points[i] = planet_point
		depths[i] = maxf(-local_ground, 0.0)
		coast_dirs[i] = _bathy_landward
		shore_distances[i] = _bathy_shore_distance + tangent_offset.dot(_bathy_landward)

	var effective_wave_scale := wave_scale
	var ocean_system := _gpu.get_parent()
	if ocean_system != null and ocean_system.has_method("debug_wave_scale"):
		effective_wave_scale *= float(ocean_system.call("debug_wave_scale"))
	var request_id := _gpu.request_batch(
		points, depths, coast_dirs, shore_distances, effective_wave_scale)
	if request_id >= 0:
		_request_context[request_id] = count
		# Keep the tiny context map bounded even if a caller destroys/rebuilds the
		# compute device before callbacks arrive.
		if _request_context.size() > 12:
			var keys := _request_context.keys()
			keys.sort()
			while keys.size() > 12:
				_request_context.erase(keys.pop_front())


func _on_gpu_batch_ready(request_id: int, results: Array[Dictionary]) -> void:
	if not _request_context.has(request_id):
		return
	_ready_probe_count = int(_request_context[request_id])
	_request_context.erase(request_id)
	_ready_results = results


func _apply_water_forces(results: Array[Dictionary], count: int) -> void:
	if count <= 0:
		return
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var volume_per_probe := displaced_volume_m3 / float(count)
	var area_per_probe := drag_area_m2 / float(count)
	var max_drag_force := maxf(_body.mass, 0.01) * max_drag_accel / float(count)
	var submerged_sum := 0.0
	_last_authored_probe_count = 0

	for i in count:
		if i >= results.size():
			break
		var render_point := _body.to_global(probe_points[i])
		var planet_point := render_point + origin
		var radial := planet_point.length()
		if radial <= 1.0:
			continue
		var up := planet_point / radial
		var probe_alt := radial - Planet.cfg.planet_radius

		var water_alt: float
		var normal: Vector3
		var water_velocity: Vector3
		var local_density := water_density
		var authored: Dictionary = _authored_sample_for_probe(render_point)
		if not authored.is_empty():
			water_alt = float(authored.get("surface_altitude_m", probe_alt - probe_depth_m))
			normal = authored.get("surface_normal", up)
			water_velocity = authored.get("current_velocity", Vector3.ZERO)
			local_density = authored_water_density
			_last_authored_probe_count += 1
		else:
			var result: Dictionary = results[i]
			water_alt = float(result["height"])
			normal = result["normal"]
			water_velocity = result["velocity"]

		var submerged := clampf((water_alt - probe_alt) / maxf(probe_depth_m, 0.05) + 0.5, 0.0, 1.0)
		if submerged <= 0.0001:
			continue
		submerged_sum += submerged

		if normal.length_squared() < 0.25:
			normal = up
		else:
			normal = normal.normalized()
		# Archimedes force remains primarily radial (gravity-opposing); a small
		# normal contribution transfers breaker/slope impulse without making a hull
		# accelerate sideways on every long swell face.
		var buoyancy_dir := (up * 0.88 + normal * 0.12).normalized()
		var buoyancy_full := local_density * GRAVITY_M_S2 * volume_per_probe
		var buoyancy_force := buoyancy_dir * (buoyancy_full * submerged)

		var offset_global := render_point - _body.global_position
		var point_velocity := _body.linear_velocity + _body.angular_velocity.cross(offset_global)
		var relative := water_velocity - point_velocity
		var speed := relative.length()
		var drag_force := Vector3.ZERO
		if speed > 1e-4 and area_per_probe > 0.0:
			drag_force = relative * (0.5 * local_density * drag_coefficient
				* area_per_probe * speed * submerged)
			if drag_force.length() > max_drag_force:
				drag_force = drag_force.normalized() * max_drag_force

		_body.apply_force(buoyancy_force + drag_force, offset_global)

	_last_submerged_fraction = submerged_sum / float(count)


func _authored_sample_for_probe(render_point: Vector3) -> Dictionary:
	if _authored_water_query == null or not is_instance_valid(_authored_water_query):
		return {}
	var value: Variant = _authored_water_query.call("query_render_point", render_point)
	if not (value is Dictionary):
		return {}
	var sample: Dictionary = value as Dictionary
	if sample.is_empty() \
			or not bool(sample.get("clipmap_simulation_enabled", true)) \
			or float(sample.get("depth_m", 0.0)) <= 0.01:
		return {}
	return sample


func _offset_direction(center: Vector3, tangent: Vector3, metres: float) -> Vector3:
	var angle := metres / maxf(Planet.cfg.planet_radius, 1.0)
	return (center * cos(angle) + tangent.normalized() * sin(angle)).normalized()


func submerged_fraction() -> float:
	return _last_submerged_fraction


func authored_probe_count() -> int:
	return _last_authored_probe_count
