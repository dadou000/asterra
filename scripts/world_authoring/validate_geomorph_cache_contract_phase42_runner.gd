extends "res://scripts/world_authoring/validate_geomorph_cache_contract_phase42_ci.gd"
## Deterministic headless entry point for the Phase 42A contract regression.
##
## The contract test does not need a live GPU cache node in the SceneTree. Calling
## its queue initialization directly avoids render-thread lifetime work while still
## exercising the exact set_production_controls()/generation/fingerprint code.


func _run() -> void:
	var error: String = _validate_contract()
	if error.is_empty():
		error = _validate_cache_invalidation()
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


func _validate_cache_invalidation() -> String:
	var cache: Node = CACHE.new() as Node
	if cache == null:
		return "Phase 42 cache could not instantiate"
	# _ready() only initializes CPU queues and discovers renderer support. The
	# isolated CI project uses GL compatibility, so it cannot enqueue GPU creation.
	cache.call("_ready")
	var fixture: Dictionary = _fixture()
	var generation_before: int = int(cache.call("anchor_generation"))
	if not bool(cache.call("set_production_controls", fixture)):
		cache.free()
		return "first production snapshot was not accepted by cache"
	var generation_after: int = int(cache.call("anchor_generation"))
	if generation_after <= generation_before:
		cache.free()
		return "new production snapshot did not invalidate all old cache keys"
	var snapshot: Dictionary = cache.call("production_controls_snapshot") as Dictionary
	var expected: Dictionary = CONTRACT.normalized_controls(fixture)
	for key: String in ["detail_strength", "warp_strength", "mountain_strength",
			"mountain_wavelength_m", "mountain_amplitude_m", "glacial_mix"]:
		if not is_equal_approx(float(snapshot.get(key, -999.0)), float(expected.get(key, 999.0))):
			cache.free()
			return "cache snapshot diverged at %s" % key
	var fingerprint: String = String(cache.call("production_controls_fingerprint"))
	if fingerprint.is_empty():
		cache.free()
		return "cache did not retain a production-control fingerprint"

	var same_generation: int = int(cache.call("anchor_generation"))
	if bool(cache.call("set_production_controls", fixture)) \
			or int(cache.call("anchor_generation")) != same_generation:
		cache.free()
		return "identical production snapshot caused unnecessary cache invalidation"

	var edited: Dictionary = fixture.duplicate(true)
	edited["mountain_amplitude_m"] = 777.0
	if not bool(cache.call("set_production_controls", edited)):
		cache.free()
		return "changed Mountain Height was ignored by cache fingerprint"
	if int(cache.call("anchor_generation")) <= same_generation:
		cache.free()
		return "changed Mountain Height did not retire old warm-cache texels"
	if String(cache.call("production_controls_fingerprint")) == fingerprint:
		cache.free()
		return "changed Mountain Height retained the old cache fingerprint"
	var stats: Dictionary = cache.call("stats") as Dictionary
	if int(stats.get("geomorph_contract_version", -1)) != CONTRACT.CONTRACT_VERSION \
			or int(stats.get("geomorph_control_bytes", -1)) != CONTRACT.BYTE_SIZE \
			or int(stats.get("geomorph_effective_seed", -1)) != 4242:
		cache.free()
		return "cache diagnostics do not expose the active shared contract"
	cache.free()
	return ""
