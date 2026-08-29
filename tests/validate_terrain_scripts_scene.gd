extends Node
## Scene-launched counterpart of validate_terrain_scripts.gd. Autoload globals are
## available when Godot launches a normal scene, matching the production runtime.

const SCRIPT_CHAIN := [
	"res://scripts/terrain/gpu_terrain_scatter.gd",
	"res://scripts/terrain/gpu_terrain_scatter_compact.gd",
	"res://scripts/terrain/gpu_terrain_scatter_global.gd",
	"res://scripts/terrain/spherical_geometry_clipmap.gd",
	"res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd",
]
const ACTIVE_SCULPT_EDITOR := preload("res://scripts/world_authoring/world_authoring_editor_live_phase15.gd")

func _ready() -> void:
	for script_path: String in SCRIPT_CHAIN:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			_fail("TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			return
		var script := resource as Script
		if not script.can_instantiate():
			_fail("TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			return
		print("TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)
	if not _validate_phase7_sculpt_math():
		return
	if not _validate_phase9_falloff_profiles():
		return
	if not _validate_phase11_thermal_erosion():
		return
	if not _validate_phase13_async_shape_math():
		return
	if not _validate_phase14_sampler_equivalence():
		return
	if not _validate_phase15_packed_deltas():
		return
	print("TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_CHAIN.size())
	get_tree().quit(0)

func _validate_phase7_sculpt_math() -> bool:
	if not Planet.has_method("configure_for_sculpt_ci"):
		_fail("PHASE7_SCULPT_FAILED: CI Planet sampler unavailable")
		return false
	Planet.call("configure_for_sculpt_ci", 1000000.0)
	Deltas.clear()
	var editor: Control = ACTIVE_SCULPT_EDITOR.new()
	editor.set("_sculpt_radius_m", 4.0)
	editor.set("_sculpt_hardness", 0.35)
	editor.set("_flatten_strength", 1.0)
	editor.set("_smooth_strength", 1.0)
	var center := Vector3.RIGHT
	var target_height := 31.5
	var before_height: float = float(Planet.call("pristine_height", center)) + Deltas.offset_at(center)
	var flatten_changed: int = int(editor.call("_flatten_final_height_brush", center, 1000000.0, target_height))
	var after_height: float = float(Planet.call("pristine_height", center)) + Deltas.offset_at(center)
	if flatten_changed <= 0 or absf(after_height - target_height) >= absf(before_height - target_height):
		_fail("PHASE7_SCULPT_FAILED: flatten did not converge final terrain toward fixed MSL target")
		return false

	var lattice: Array = Deltas.dir_to_lattice(center)
	var face: int = int(lattice[0])
	var ci: int = int(round(float(lattice[1])))
	var cj: int = int(round(float(lattice[2])))
	var a: Vector3i = Deltas.canonical_address(face, ci, cj)
	var b: Vector3i = Deltas.canonical_address(face, ci, cj + 1)
	var delta_a: float = Deltas.get_offset(a.x, a.y, a.z)
	var delta_b: float = Deltas.get_offset(b.x, b.y, b.z)
	if is_equal_approx(delta_a, delta_b):
		_fail("PHASE7_SCULPT_FAILED: flatten equalized deltas instead of final height")
		return false

	Deltas.clear()
	var center_address: Vector3i = Deltas.canonical_address(face, ci, cj)
	var exact_center: Vector3 = Deltas.lattice_to_dir(center_address.x, float(center_address.y), float(center_address.z))
	Deltas.add_offset(center_address.x, center_address.y, center_address.z, 20.0, -10000.0, 10000.0)
	var spike_before: float = absf(Deltas.get_offset(center_address.x, center_address.y, center_address.z))
	var smooth_changed: int = int(editor.call("_smooth_final_height_brush", exact_center, 1000000.0))
	var spike_after: float = absf(Deltas.get_offset(center_address.x, center_address.y, center_address.z))
	if smooth_changed <= 0 or spike_after >= spike_before:
		_fail("PHASE7_SCULPT_FAILED: smooth did not reduce local authored spike")
		return false
	Deltas.clear()
	editor.free()
	print("PHASE7_SCULPT_OK: flatten final-height compensation + smoothing via Phase 15 packed publication")
	return true

func _validate_phase9_falloff_profiles() -> bool:
	var editor: Control = ACTIVE_SCULPT_EDITOR.new()
	for profile: int in 4:
		editor.set("_sculpt_falloff_profile", profile)
		var core: float = float(editor.call("_sculpt_profile_weight", 0.20, 0.35))
		var edge: float = float(editor.call("_sculpt_profile_weight", 1.0, 0.35))
		if not is_equal_approx(core, 1.0) or absf(edge) > 1e-6:
			_fail("PHASE9_FALLOFF_FAILED: profile %d does not preserve core/edge invariants" % profile)
			return false
		var previous := 1.0
		for step: int in range(1, 9):
			var d: float = 0.35 + (1.0 - 0.35) * float(step) / 8.0
			var weight: float = float(editor.call("_sculpt_profile_weight", d, 0.35))
			if weight < -1e-6 or weight > 1.000001 or weight > previous + 1e-6:
				_fail("PHASE9_FALLOFF_FAILED: profile %d is unbounded or non-monotonic" % profile)
				return false
			previous = weight
	editor.free()
	print("PHASE9_FALLOFF_OK: smooth + linear + cosine + gaussian")
	return true

func _validate_phase11_thermal_erosion() -> bool:
	Planet.call("configure_for_sculpt_ci", 1000000.0)
	Deltas.clear()
	var editor: Control = ACTIVE_SCULPT_EDITOR.new()
	editor.set("_sculpt_radius_m", 3.0)
	editor.set("_sculpt_hardness", 0.98)
	editor.set("_sculpt_falloff_profile", 0)
	editor.set("_thermal_talus_deg", 75.0)
	editor.set("_thermal_strength", 0.5)

	var lattice: Array = Deltas.dir_to_lattice(Vector3.RIGHT)
	var center_address := Deltas.canonical_address(
		int(lattice[0]),
		int(round(float(lattice[1]))),
		int(round(float(lattice[2]))))
	var exact_center: Vector3 = Deltas.lattice_to_dir(
		center_address.x, float(center_address.y), float(center_address.z))
	Deltas.add_offset(center_address.x, center_address.y, center_address.z,
		20.0, -10000.0, 10000.0)

	var writes_value: Variant = editor.call("_thermal_erosion_writes", exact_center, 1000000.0)
	if not (writes_value is Array):
		_fail("PHASE11_THERMAL_FAILED: erosion target builder returned invalid data")
		return false
	var writes: Array = writes_value as Array
	if writes.is_empty():
		_fail("PHASE11_THERMAL_FAILED: isolated authored spike produced no transfers")
		return false
	var net_change := 0.0
	var center_after := 20.0
	var recipient_gain := false
	for write: Dictionary in writes:
		var address: Vector3i = write["address"]
		var before: float = Deltas.get_offset(address.x, address.y, address.z)
		var desired: float = float(write["value"])
		var change: float = desired - before
		net_change += change
		if address == center_address:
			center_after = desired
		elif change > 1e-6:
			recipient_gain = true
	if absf(net_change) > 1e-4:
		_fail("PHASE11_THERMAL_FAILED: proposed transfers do not conserve local material")
		return false
	if center_after >= 20.0 or not recipient_gain:
		_fail("PHASE11_THERMAL_FAILED: spike did not erode and deposit downhill")
		return false

	var changed: int = int(editor.call("_thermal_erosion_brush", exact_center, 1000000.0))
	var applied_center: float = Deltas.get_offset(center_address.x, center_address.y, center_address.z)
	if changed <= 0 or applied_center >= 20.0:
		_fail("PHASE11_THERMAL_FAILED: conservative targets were not applied to Deltas")
		return false
	Deltas.clear()
	editor.free()
	print("PHASE11_THERMAL_OK: downhill deposition + conservative simultaneous relaxation")
	return true

func _validate_phase13_async_shape_math() -> bool:
	Planet.call("configure_for_sculpt_ci", 1000000.0)
	Deltas.clear()
	var editor: Control = ACTIVE_SCULPT_EDITOR.new()
	editor.set("_sculpt_radius_m", 3.0)
	editor.set("_sculpt_hardness", 0.90)
	editor.set("_sculpt_falloff_profile", 0)
	editor.set("_flatten_strength", 1.0)
	editor.set("_smooth_strength", 1.0)
	editor.set("_thermal_talus_deg", 75.0)
	editor.set("_thermal_strength", 0.5)
	var planet_radius := 1000000.0
	var lattice: Array = Deltas.dir_to_lattice(Vector3.RIGHT)
	var center_address: Vector3i = Deltas.canonical_address(
		int(lattice[0]),
		int(round(float(lattice[1]))),
		int(round(float(lattice[2]))))
	var center: Vector3 = Deltas.lattice_to_dir(
		center_address.x, float(center_address.y), float(center_address.z))

	var flatten_samples_value: Variant = editor.call("_collect_sculpt_samples", center, planet_radius)
	if not (flatten_samples_value is Array):
		_fail("PHASE13_ASYNC_FAILED: flatten sample collection returned invalid data")
		return false
	var flatten_samples: Array = flatten_samples_value as Array
	var flatten_heights := PackedFloat32Array()
	flatten_heights.resize(flatten_samples.size())
	for index: int in flatten_samples.size():
		var sample: Dictionary = flatten_samples[index]
		flatten_heights[index] = float(Planet.call("pristine_height", sample["dir"]))
	var flatten_target := 26.0
	var flatten_before: float = float(Planet.call("pristine_height", center)) + Deltas.offset_at(center)
	var flatten_job := {
		"center": center,
		"planet_radius": planet_radius,
		"radius_m": 3.0,
		"target_height_m": flatten_target,
		"strength": 1.0,
		"samples": flatten_samples,
		"delta_snapshot": Deltas.snapshot_for_bounds(center, 8.0 / planet_radius),
	}
	var flatten_changed: int = int(editor.call("_apply_flatten_job_with_pristine", flatten_job, flatten_heights))
	var flatten_after: float = float(Planet.call("pristine_height", center)) + Deltas.offset_at(center)
	if flatten_changed <= 0 or absf(flatten_after - flatten_target) >= absf(flatten_before - flatten_target):
		_fail("PHASE13_ASYNC_FAILED: async flatten finalization did not converge final height")
		return false

	Deltas.clear()
	Deltas.add_offset(center_address.x, center_address.y, center_address.z,
		20.0, -10000.0, 10000.0)
	var smooth_samples: Array = editor.call("_collect_sculpt_samples", center, planet_radius) as Array
	var smooth_job := {
		"center": center,
		"planet_radius": planet_radius,
		"radius_m": 3.0,
		"strength": 1.0,
		"neighbor_step": 1,
		"samples": smooth_samples,
		"delta_snapshot": Deltas.snapshot_for_bounds(center, 8.0 / planet_radius),
	}
	var smooth_prepared: Dictionary = editor.call("_prepare_smooth_query_addresses", smooth_job) as Dictionary
	smooth_job["addresses"] = smooth_prepared["addresses"]
	smooth_job["address_index"] = smooth_prepared["address_index"]
	var smooth_heights: PackedFloat32Array = editor.call(
		"_cpu_pristine_for_addresses", smooth_prepared["addresses"]) as PackedFloat32Array
	var smooth_before: float = Deltas.get_offset(center_address.x, center_address.y, center_address.z)
	var smooth_changed: int = int(editor.call("_apply_smooth_job_with_pristine", smooth_job, smooth_heights))
	var smooth_after: float = Deltas.get_offset(center_address.x, center_address.y, center_address.z)
	if smooth_changed <= 0 or absf(smooth_after) >= absf(smooth_before):
		_fail("PHASE13_ASYNC_FAILED: async smooth finalization did not reduce center spike")
		return false

	Deltas.clear()
	Deltas.add_offset(center_address.x, center_address.y, center_address.z,
		20.0, -10000.0, 10000.0)
	var thermal_samples: Array = editor.call("_collect_sculpt_samples", center, planet_radius) as Array
	var thermal_job := {
		"center": center,
		"planet_radius": planet_radius,
		"radius_m": 3.0,
		"talus_deg": 75.0,
		"strength": 0.5,
		"spacing_m": Deltas.sample_spacing(planet_radius),
		"samples": thermal_samples,
		"delta_snapshot": Deltas.snapshot_for_bounds(center, 8.0 / planet_radius),
	}
	var thermal_prepared: Dictionary = editor.call("_prepare_thermal_query_addresses", thermal_job) as Dictionary
	thermal_job["addresses"] = thermal_prepared["addresses"]
	thermal_job["address_index"] = thermal_prepared["address_index"]
	var thermal_addresses: Array = thermal_prepared["addresses"]
	var thermal_heights: PackedFloat32Array = editor.call(
		"_cpu_pristine_for_addresses", thermal_addresses) as PackedFloat32Array
	var sum_before := 0.0
	for address_value: Variant in thermal_addresses:
		var address: Vector3i = address_value
		sum_before += Deltas.get_offset(address.x, address.y, address.z)
	var thermal_changed: int = int(editor.call("_apply_thermal_job_with_pristine", thermal_job, thermal_heights))
	var sum_after := 0.0
	for address_value: Variant in thermal_addresses:
		var address: Vector3i = address_value
		sum_after += Deltas.get_offset(address.x, address.y, address.z)
	var thermal_center_after: float = Deltas.get_offset(center_address.x, center_address.y, center_address.z)
	if thermal_changed <= 0 or thermal_center_after >= 20.0 or absf(sum_after - sum_before) > 1e-3:
		_fail("PHASE13_ASYNC_FAILED: async thermal finalization lost conservation or failed to erode")
		return false

	Deltas.clear()
	editor.free()
	print("PHASE13_ASYNC_MATH_OK: flatten + smooth + conservative thermal readback finalization")
	return true

func _validate_phase14_sampler_equivalence() -> bool:
	var planet_radius := 1000000.0
	var brush_radius := 18.0
	var hardness := 0.41
	var center := Vector3(1.0, 1.0, 0.12).normalized()
	var editor: Control = ACTIVE_SCULPT_EDITOR.new()
	editor.set("_sculpt_radius_m", brush_radius)
	editor.set("_sculpt_hardness", hardness)
	editor.set("_sculpt_falloff_profile", 2)
	var optimized_value: Variant = editor.call("_collect_sculpt_samples", center, planet_radius)
	if not (optimized_value is Array):
		_fail("PHASE14_SAMPLER_FAILED: optimized collector returned invalid data")
		return false
	var optimized: Array = optimized_value as Array
	var optimized_weights: Dictionary = {}
	for sample_value: Variant in optimized:
		var sample: Dictionary = sample_value as Dictionary
		var address: Vector3i = sample["address"]
		var key: int = _packed_address_key(address)
		if optimized_weights.has(key):
			_fail("PHASE14_SAMPLER_FAILED: optimized collector emitted duplicate seam address")
			return false
		optimized_weights[key] = float(sample["weight"])

	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var center_lattice: Array = Deltas.dir_to_lattice(center)
	var face: int = int(center_lattice[0])
	var center_i: int = int(round(float(center_lattice[1])))
	var center_j: int = int(round(float(center_lattice[2])))
	var extent: int = maxi(1, int(ceil(brush_radius / spacing_m)) + 2)
	var reference_weights: Dictionary = {}
	for source_j: int in range(center_j - extent, center_j + extent + 1):
		for source_i: int in range(center_i - extent, center_i + extent + 1):
			var address: Vector3i = Deltas.canonical_address(face, source_i, source_j)
			if address.x < 0:
				continue
			var key: int = _packed_address_key(address)
			if reference_weights.has(key):
				continue
			var sample_dir: Vector3 = Deltas.lattice_to_dir(address.x, float(address.y), float(address.z))
			var distance_m: float = acos(clampf(center.dot(sample_dir), -1.0, 1.0)) * planet_radius
			if distance_m > brush_radius:
				continue
			var normalized_distance: float = distance_m / brush_radius
			var weight: float = float(editor.call("_sculpt_profile_weight", normalized_distance, hardness))
			if weight <= 0.0001:
				continue
			reference_weights[key] = weight

	if optimized_weights.size() != reference_weights.size():
		_fail("PHASE14_SAMPLER_FAILED: optimized/reference sample counts differ (%d vs %d)" % [optimized_weights.size(), reference_weights.size()])
		return false
	var maximum_weight_error := 0.0
	for key_value: Variant in reference_weights.keys():
		var key: int = int(key_value)
		if not optimized_weights.has(key):
			_fail("PHASE14_SAMPLER_FAILED: optimized collector missed canonical seam sample")
			return false
		maximum_weight_error = maxf(maximum_weight_error,
			absf(float(optimized_weights[key]) - float(reference_weights[key])))
	if maximum_weight_error > 2e-5:
		_fail("PHASE14_SAMPLER_FAILED: polynomial arc falloff error %.8f exceeds tolerance" % maximum_weight_error)
		return false
	if not bool(editor.get("_phase14_last_fast_arc")):
		_fail("PHASE14_SAMPLER_FAILED: planetary test did not exercise fast arc path")
		return false
	editor.free()
	print("PHASE14_SAMPLER_OK: seam-canonical sample set matches exact geodesic reference; max weight error %.8f" % maximum_weight_error)
	return true

func _validate_phase15_packed_deltas() -> bool:
	Deltas.clear()
	var base := Deltas.dir_to_lattice(Vector3(1.0, 0.17, -0.11).normalized())
	var a: Vector3i = Deltas.canonical_address(
		int(base[0]), int(round(float(base[1]))), int(round(float(base[2]))))
	var b: Vector3i = Deltas.canonical_address(a.x, a.y + 3, a.z - 2)
	var packed_a: int = Deltas.pack_address(a)
	var packed_b: int = Deltas.pack_address(b)
	if packed_a < 0 or packed_b < 0 or Deltas.unpack_address(packed_a) != a or Deltas.unpack_address(packed_b) != b:
		_fail("PHASE15_PACKED_FAILED: canonical address codec did not round-trip")
		return false
	var addresses := PackedInt64Array([packed_a, packed_b])
	var values := PackedFloat32Array([3.25, -2.5])
	var changed: int = Deltas.set_packed_offsets_batch(addresses, values, -10000.0, 10000.0)
	if changed != 2:
		_fail("PHASE15_PACKED_FAILED: initial packed write changed %d samples instead of 2" % changed)
		return false
	if not is_equal_approx(Deltas.get_offset(a.x, a.y, a.z), 3.25) \
			or not is_equal_approx(Deltas.get_offset(b.x, b.y, b.z), -2.5):
		_fail("PHASE15_PACKED_FAILED: packed absolute targets were not stored exactly")
		return false
	values[0] = 0.0
	values[1] = 0.0
	changed = Deltas.set_packed_offsets_batch(addresses, values, -10000.0, 10000.0)
	if changed != 2 or not Deltas.is_empty():
		_fail("PHASE15_PACKED_FAILED: zero packed targets did not prune the sparse tile layer")
		return false
	print("PHASE15_PACKED_OK: address codec + absolute write + zero-tile pruning")
	return true

func _packed_address_key(address: Vector3i) -> int:
	return (int(address.x) << 42) | (int(address.y) << 21) | int(address.z)

func _fail(message: String) -> void:
	push_error(message)
	get_tree().quit(1)
