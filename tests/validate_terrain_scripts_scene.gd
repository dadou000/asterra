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
const ACTIVE_SCULPT_EDITOR := preload("res://scripts/world_authoring/world_authoring_editor_live_phase13.gd")

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

	# Verify flatten compensated a generated slope rather than writing one constant
	# delta everywhere. Two nearby lattice points have slightly different pristine
	# heights and must therefore end with different offsets at the same final target.
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

	# Smooth must reduce a sharp authored spike. Use an exact lattice direction so
	# interpolation does not hide whether the center sample actually changed.
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
	print("PHASE7_SCULPT_OK: flatten final-height compensation + smoothing via Phase 13 batch-compatible writes")
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

	# Flatten finalization: feed deterministic pristine values into the same function
	# used by asynchronous GPU readback and require movement toward one fixed MSL target.
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

	# Smooth finalization: build the exact unique neighborhood address set used by
	# the GPU path, then verify an authored center spike is reduced.
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

	# Thermal finalization: conservative offset transfer must preserve the sum of
	# authored delta heights over the complete source+recipient address set.
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

func _fail(message: String) -> void:
	push_error(message)
	get_tree().quit(1)
