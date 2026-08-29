class_name WorldAuthoringLiveEditorPhase14
extends "res://scripts/world_authoring/world_authoring_editor_live_phase13.gd"
## Phase 14: low-overhead spherical brush candidate collection.
##
## The previous hot loop allocated a String key and evaluated acos() for every
## candidate in the enclosing lattice square. Large 150 m brushes can inspect well
## over one hundred thousand lattice points before asynchronous shaping even starts.
##
## This layer keeps the same canonical cube-sphere lattice and geodesic falloff but:
## - de-duplicates canonical addresses with one packed 45-bit integer key;
## - rejects outside points with one precomputed cosine threshold;
## - for small angular brushes, reconstructs arc angle from chord length with a
##   fifth-order expansion instead of acos() per accepted sample;
## - automatically uses exact acos() when brush angular radius is large enough for
##   the local approximation to matter.

const FAST_ARC_MAX_ANGLE_RAD: float = 0.02
const ADDRESS_AXIS_BITS: int = 21
const ADDRESS_FACE_SHIFT: int = ADDRESS_AXIS_BITS * 2

var _phase14_last_collect_ms: float = 0.0
var _phase14_last_examined: int = 0
var _phase14_last_accepted: int = 0
var _phase14_last_fast_arc: bool = false


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Brush sampler")
	var label := Label.new()
	label.modulate = Color(0.64, 0.76, 0.86)
	if _phase14_last_examined <= 0:
		label.text = "No brush candidate pass measured yet."
	else:
		label.text = "%.3f ms • %d examined • %d accepted • %s arc metric" % [
			_phase14_last_collect_ms,
			_phase14_last_examined,
			_phase14_last_accepted,
			"polynomial" if _phase14_last_fast_arc else "exact",
		]
	_workspace.add_child(label)
	_add_note("Candidate collection uses packed seam-canonical lattice addresses and a cosine boundary test. Normal planetary brushes avoid per-sample trigonometric calls; large angular brushes on small bodies automatically retain exact acos geodesics.")


func _collect_sculpt_samples(center_dir: Vector3, planet_radius: float) -> Array[Dictionary]:
	var started_us: int = Time.get_ticks_usec()
	var out: Array[Dictionary] = []
	_phase14_last_examined = 0
	_phase14_last_accepted = 0
	_phase14_last_fast_arc = false
	if center_dir.length_squared() < 0.5 or _sculpt_radius_m <= 0.0 or planet_radius <= 1.0:
		_phase14_last_collect_ms = float(Time.get_ticks_usec() - started_us) / 1000.0
		return out

	var center: Vector3 = center_dir.normalized()
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var center_lattice: Array = Deltas.dir_to_lattice(center)
	var face: int = int(center_lattice[0])
	var center_i: int = int(round(float(center_lattice[1])))
	var center_j: int = int(round(float(center_lattice[2])))
	var extent: int = maxi(1, int(ceil(_sculpt_radius_m / spacing_m)) + 2)
	var hard: float = clampf(_sculpt_hardness, 0.0, 0.98)
	var angular_radius: float = minf(_sculpt_radius_m / planet_radius, PI)
	var minimum_dot: float = cos(angular_radius)
	var inverse_angular_radius: float = 1.0 / maxf(angular_radius, 1e-12)
	var fast_arc: bool = angular_radius <= FAST_ARC_MAX_ANGLE_RAD
	_phase14_last_fast_arc = fast_arc
	var visited: Dictionary = {}

	for source_j: int in range(center_j - extent, center_j + extent + 1):
		for source_i: int in range(center_i - extent, center_i + extent + 1):
			_phase14_last_examined += 1
			var address: Vector3i = Deltas.canonical_address(face, source_i, source_j)
			if address.x < 0:
				continue
			var packed_key: int = _phase14_address_key(address)
			if visited.has(packed_key):
				continue
			visited[packed_key] = true
			var sample_dir: Vector3 = Deltas.lattice_to_dir(
				address.x, float(address.y), float(address.z))
			var dot_value: float = clampf(center.dot(sample_dir), -1.0, 1.0)
			if dot_value < minimum_dot:
				continue

			var angle_rad: float
			if fast_arc:
				# Unit-sphere chord c = sqrt(2 - 2 cos(theta)). Inverting
				# c=2sin(theta/2) gives theta = c + c^3/24 + 3c^5/640 + O(c^7).
				# At the 0.02 rad switch point the omitted term is far below the
				# edit lattice's physical resolution, while avoiding acos in the hot loop.
				var chord_sq: float = maxf(0.0, 2.0 - 2.0 * dot_value)
				var chord: float = sqrt(chord_sq)
				var chord2: float = chord_sq
				angle_rad = chord * (1.0 + chord2 * (1.0 / 24.0) \
					+ chord2 * chord2 * (3.0 / 640.0))
			else:
				angle_rad = acos(dot_value)
			var normalized_distance: float = clampf(angle_rad * inverse_angular_radius, 0.0, 1.0)
			var weight: float = _sculpt_profile_weight(normalized_distance, hard)
			if weight <= 0.0001:
				continue
			out.append({"address": address, "dir": sample_dir, "weight": weight})

	_phase14_last_accepted = out.size()
	_phase14_last_collect_ms = float(maxi(0, Time.get_ticks_usec() - started_us)) / 1000.0
	# Phase 10's synchronous telemetry override is bypassed by this optimized
	# collector, so preserve its candidate-count contract explicitly.
	if not _telemetry_active_tool.is_empty():
		_telemetry_candidate_count = out.size()
	return out


func _phase14_address_key(address: Vector3i) -> int:
	return (int(address.x) << ADDRESS_FACE_SHIFT) \
		| (int(address.y) << ADDRESS_AXIS_BITS) \
		| int(address.z)
