extends "res://scripts/world_authoring/validate_geomorph_cache_contract_phase42_ci.gd"
## Coroutine-safe entry point for the Phase 42A contract regression.


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
