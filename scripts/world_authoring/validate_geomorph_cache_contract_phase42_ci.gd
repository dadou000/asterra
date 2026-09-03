extends Node
## Phase 42A regression for the shared production geomorph/cache contract.
##
## This test is intentionally independent of a live RenderingDevice. It proves the
## serialized CPU snapshot, cache invalidation policy and compute-shader ABI agree;
## the workflow separately asks Godot to compile the RDShaderFile to SPIR-V.

const CONTRACT := preload(
	"res://scripts/world_authoring/model/terrain_geomorph_gpu_contract.gd")
const CACHE := preload(
	"res://scripts/terrain/gpu_terrain_clipmap_cache_phase42_final.gd")
const RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase42.gd")
const COMPUTE_PATH := "res://shaders/terrain_clipmap_cache_phase42.glsl"


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var error: String = _validate_contract()
	if error.is_empty():
		error = await _validate_cache_invalidation()
	if error.is_empty():
		error = _validate_runtime_snapshot()
	if error.is_empty():
		error = _validate_compute_shader_contract()
	if not error.is_empty():
		push_error("GEOMORPH_CACHE_PHASE42_FAILED: " + error)
		get_tree().quit(1)
		return
	print("GEOMORPH_CACHE_PHASE42_OK: one normalized production snapshot drives cache ABI, invalidation, seed override and active runtime state")
	get_tree().quit(0)


func _fixture() -> Dictionary:
	return {
		"detail_strength": 0.63,
		"warp_strength": 1.75,
		"broad_strength": 0.82,
		"mountain_strength": 0.37,
		"mid_strength": 1.22,
		"channel_strength": 0.54,
		"deposit_strength": 1.18,
		"fine_strength": 0.71,
		"dune_strength": 0.44,
		"glacial_strength": 0.91,
		"broad_wavelength_m": 12345.0,
		"broad_low_amplitude_m": 31.0,
		"broad_mountain_amplitude_m": 143.0,
		"broad_warp": 1.2,
		"mountain_wavelength_m": 7345.0,
		"mountain_amplitude_m": 333.0,
		"mountain_warp": 1.45,
		"mountain_ridge_scale": 1.82,
		"mountain_cell_mix": 0.41,
		"mid_wavelength_m": 1710.0,
		"mid_ridge_amplitude_m": 85.0,
		"mid_noise_amplitude_m": 19.0,
		"mid_warp": 0.61,
		"mid_ridge_scale": 1.41,
		"mid_detail_scale": 2.33,
		"channel_wavelength_m": 515.0,
		"channel_depth_min_m": 3.0,
		"channel_depth_max_m": 47.0,
		"channel_warp": 0.73,
		"channel_power": 5.2,
		"flow_along_scale": 0.35,
		"flow_across_scale": 1.66,
		"deposit_amplitude_min_m": 2.0,
		"deposit_amplitude_max_m": 15.0,
		"deposit_scale": 0.57,
		"deposit_power": 2.7,
		"fine_wavelength_m": 96.0,
		"fine_amplitude_m": 6.2,
		"dune_wavelength_m": 205.0,
		"dune_amplitude_m": 11.0,
		"dune_warp": 0.52,
		"micro_wavelength_m": 19.0,
		"micro_amplitude_m": 1.3,
		"glacial_wavelength_m": 3100.0,
		"glacial_amplitude_m": 67.0,
		"glacial_base_scale": 0.58,
		"glacial_mix": 0.81,
		"override_seed": true,
		"detail_seed": 4242,
	}


func _validate_contract() -> String:
	if CONTRACT.PACKED_KEYS.size() != 48 or CONTRACT.VEC4_COUNT != 12 \
			or CONTRACT.BYTE_SIZE != 192:
		return "std430 production-control ABI is not exactly 12 vec4 / 192 bytes"
	var fixture: Dictionary = _fixture()
	var normalized: Dictionary = CONTRACT.normalized_controls(fixture)
	var packed: PackedByteArray = CONTRACT.pack_controls(fixture)
	var values: PackedFloat32Array = CONTRACT.packed_values(fixture)
	if packed.size() != CONTRACT.BYTE_SIZE or values.size() != CONTRACT.PACKED_KEYS.size():
		return "packed production-control sizes disagree"
	for index: int in CONTRACT.PACKED_KEYS.size():
		var decoded: float = packed.decode_float(index * 4)
		if not is_equal_approx(decoded, float(values[index])):
			return "packed control %s changed value" % CONTRACT.PACKED_KEYS[index]
	if not is_equal_approx(float(normalized.get("mountain_amplitude_m", 0.0)), 333.0) \
			or not is_equal_approx(float(normalized.get("warp_strength", 0.0)), 1.75):
		return "fixture values were not preserved by normalization"
	if CONTRACT.effective_seed(fixture, 17) != 4242:
		return "explicit production detail seed did not override the planet seed"

	# Contract clamps must remain identical to the render-material binder's safety
	# policy: gains/amplitudes nonnegative, wavelengths/scales strictly positive and
	# cell mix constrained to [0,1].
	var unsafe := {
		"mountain_strength": -5.0,
		"mountain_wavelength_m": -1.0,
		"mountain_amplitude_m": -9.0,
		"mountain_cell_mix": 4.0,
		"channel_power": 0.0,
	}
	var safe: Dictionary = CONTRACT.normalized_controls(unsafe)
	if not is_zero_approx(float(safe.get("mountain_strength", 1.0))) \
			or not is_equal_approx(float(safe.get("mountain_wavelength_m", 0.0)), 0.001) \
			or not is_zero_approx(float(safe.get("mountain_amplitude_m", 1.0))) \
			or not is_equal_approx(float(safe.get("mountain_cell_mix", 0.0)), 1.0) \
			or not is_equal_approx(float(safe.get("channel_power", 0.0)), 0.01):
		return "production-control normalization no longer matches renderer safety clamps"
	return ""


func _validate_cache_invalidation() -> String:
	var cache: Node = CACHE.new() as Node
	if cache == null:
		return "Phase 42 cache could not instantiate"
	add_child(cache)
	await get_tree().process_frame
	var fixture: Dictionary = _fixture()
	var generation_before: int = int(cache.call("anchor_generation"))
	if not bool(cache.call("set_production_controls", fixture)):
		cache.queue_free()
		return "first production snapshot was not accepted by cache"
	var generation_after: int = int(cache.call("anchor_generation"))
	if generation_after <= generation_before:
		cache.queue_free()
		return "new production snapshot did not invalidate all old cache keys"
	var snapshot: Dictionary = cache.call("production_controls_snapshot") as Dictionary
	var expected: Dictionary = CONTRACT.normalized_controls(fixture)
	for key: String in ["detail_strength", "warp_strength", "mountain_strength",
			"mountain_wavelength_m", "mountain_amplitude_m", "glacial_mix"]:
		if not is_equal_approx(float(snapshot.get(key, -999.0)), float(expected.get(key, 999.0))):
			cache.queue_free()
			return "cache snapshot diverged at %s" % key
	var fingerprint: String = String(cache.call("production_controls_fingerprint"))
	if fingerprint.is_empty():
		cache.queue_free()
		return "cache did not retain a production-control fingerprint"

	var same_generation: int = int(cache.call("anchor_generation"))
	if bool(cache.call("set_production_controls", fixture)) \
			or int(cache.call("anchor_generation")) != same_generation:
		cache.queue_free()
		return "identical production snapshot caused unnecessary cache invalidation"

	var edited: Dictionary = fixture.duplicate(true)
	edited["mountain_amplitude_m"] = 777.0
	if not bool(cache.call("set_production_controls", edited)):
		cache.queue_free()
		return "changed Mountain Height was ignored by cache fingerprint"
	if int(cache.call("anchor_generation")) <= same_generation:
		cache.queue_free()
		return "changed Mountain Height did not retire old warm-cache texels"
	if String(cache.call("production_controls_fingerprint")) == fingerprint:
		cache.queue_free()
		return "changed Mountain Height retained the old cache fingerprint"
	var stats: Dictionary = cache.call("stats") as Dictionary
	if int(stats.get("geomorph_contract_version", -1)) != CONTRACT.CONTRACT_VERSION \
			or int(stats.get("geomorph_control_bytes", -1)) != CONTRACT.BYTE_SIZE \
			or int(stats.get("geomorph_effective_seed", -1)) != 4242:
		cache.queue_free()
		return "cache diagnostics do not expose the active shared contract"
	cache.queue_free()
	await get_tree().process_frame
	return ""


func _validate_runtime_snapshot() -> String:
	var runtime: Node = RUNTIME.new() as Node
	if runtime == null:
		return "Phase 42 displacement runtime could not instantiate"
	var fixture: Dictionary = _fixture()
	runtime.set("_production_controls", fixture)
	var active: Dictionary = runtime.call("active_production_controls") as Dictionary
	var expected: Dictionary = CONTRACT.normalized_controls(fixture)
	for key: String in CONTRACT.PACKED_KEYS:
		if not is_equal_approx(float(active.get(key, 0.0)), float(expected.get(key, 0.0))):
			runtime.free()
			return "runtime active snapshot diverged from shared contract at %s" % key
	if String(runtime.call("active_production_controls_fingerprint", 17)) \
			!= CONTRACT.fingerprint(fixture, 17):
		runtime.free()
		return "runtime and cache contract use different fingerprint semantics"
	var revision_before: int = int(runtime.call("active_production_controls_revision"))
	runtime.call("_record_active_production_controls_revision")
	var revision_after: int = int(runtime.call("active_production_controls_revision"))
	if revision_after <= revision_before:
		runtime.free()
		return "changed active controls did not advance the runtime revision"
	runtime.call("_record_active_production_controls_revision")
	if int(runtime.call("active_production_controls_revision")) != revision_after:
		runtime.free()
		return "unchanged active controls advanced the runtime revision"
	runtime.free()
	return ""


func _validate_compute_shader_contract() -> String:
	var resource: Resource = load(COMPUTE_PATH)
	if resource == null or not (resource is RDShaderFile):
		return "Phase 42 compute shader did not import as RDShaderFile"
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() \
			or spirv.bytecode_compute.is_empty():
		return "Phase 42 compute shader did not compile to compute SPIR-V"
	var source: String = FileAccess.get_file_as_string(COMPUTE_PATH)
	for token: String in [
		"binding = 8",
		"GC_DETAIL_STRENGTH",
		"GC_WARP_STRENGTH",
		"GC_MOUNTAIN_STRENGTH",
		"GC_MOUNTAIN_WAVELENGTH_M",
		"GC_MOUNTAIN_AMPLITUDE_M",
		"GC_GLACIAL_MIX",
		"geomorph(dir,spacing,macro_h)*coast_guard*GC_DETAIL_STRENGTH",
	]:
		if source.find(token) < 0:
			return "compute shader is missing shared-contract token: %s" % token
	return ""
