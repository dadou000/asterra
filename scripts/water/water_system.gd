extends "res://scripts/water/water_system_base.gd"
## Production facade extending the stable 0.1.0 water coordinator with
## transactional planet-coarse <-> sparse-SWE ownership bridges and diagnostics.
##
## Automatic surface/flood promotion and isolated quiet-surface demotion policies
## are installed but deliberately remain OFF until their conservation gates run on
## the project Godot build/GPU. Explicit transactional handoffs remain available.

signal planet_promotion_bridge_state_changed(state: String)
signal planet_promotion_bridge_ready
signal planet_promotion_completed(report: Dictionary)
signal planet_promotion_failed(error: Error, stage: String)
signal planet_demotion_bridge_state_changed(state: String)
signal planet_demotion_bridge_ready
signal planet_demotion_completed(report: Dictionary)
signal planet_demotion_failed(error: Error, stage: String)
signal frontier_coarse_preseed_ready
signal frontier_coarse_preseed_failed(error: Error)
signal active_sparse_volume_ready(request_id: int, volume_m3: float)
signal active_sparse_volume_failed(request_id: int, error: Error)
signal representation_audit_ready(audit_id: int, report: Dictionary)
signal representation_audit_failed(audit_id: int, error: Error, stage: String)

## Policy switches consumed by the automatic surface promotion/demotion autoloads.
## Both remain false in production until their renderer conservation gates run.
var automatic_coarse_promotion_enabled := false
var automatic_fine_demotion_enabled := false

var _planet_promotion_bridge: PlanetHydroPromotionBridge
var _planet_promotion_state := "offline"
var _planet_demotion_bridge: PlanetHydroDemotionBridge
var _planet_demotion_state := "offline"
var _frontier_coarse_preseed: HydroFrontierCoarsePreseed
var _sparse_volume_diagnostic: SparseHydroVolumeDiagnosticsGPU
var _representation_audit: HydroRepresentationAudit
var _failed_runtime_teardown_queued := false


func _ready() -> void:
	super._ready()
	if not sparse_runtime_ready.is_connected(_on_sparse_runtime_ready_for_promotion):
		sparse_runtime_ready.connect(_on_sparse_runtime_ready_for_promotion)
	if not sparse_runtime_state_changed.is_connected(_on_sparse_runtime_state_for_promotion):
		sparse_runtime_state_changed.connect(_on_sparse_runtime_state_for_promotion)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(
			_on_persistent_hydrology_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(
			_on_persistent_hydrology_store_rebuilt)
	call_deferred(&"_try_bind_sparse_volume_diagnostic")
	call_deferred(&"_try_bind_frontier_coarse_preseed")
	call_deferred(&"_try_bind_planet_promotion_bridge")
	call_deferred(&"_try_bind_planet_demotion_bridge")


func planet_promotion_bridge_available() -> bool:
	return _planet_promotion_state == "ready" \
		and _planet_promotion_bridge != null \
		and _planet_promotion_bridge.initialized_ok()


func planet_promotion_bridge_state() -> String:
	return _planet_promotion_state


func planet_promotion_bridge() -> PlanetHydroPromotionBridge:
	return _planet_promotion_bridge


func planet_demotion_bridge_available() -> bool:
	return _planet_demotion_state == "ready" \
		and _planet_demotion_bridge != null \
		and _planet_demotion_bridge.initialized_ok()


func planet_demotion_bridge_state() -> String:
	return _planet_demotion_state


func planet_demotion_bridge() -> PlanetHydroDemotionBridge:
	return _planet_demotion_bridge


func frontier_coarse_preseed_available() -> bool:
	return _frontier_coarse_preseed != null \
		and _frontier_coarse_preseed.initialized_ok()


func active_sparse_volume_diagnostic_available() -> bool:
	return _sparse_volume_diagnostic != null \
		and _sparse_volume_diagnostic.initialized_ok()


func request_active_sparse_volume() -> int:
	if not active_sparse_volume_diagnostic_available() \
			or _sparse_volume_diagnostic.pending():
		return -1
	var runtime := sparse_runtime()
	if runtime == null or runtime.busy():
		return -1
	return _sparse_volume_diagnostic.request_volume()


func representation_audit_available() -> bool:
	return _representation_audit != null \
		and _representation_audit.initialized_ok()


## Coordinated coarse+fine snapshot. The production fine external-flux ledger is
## auto-detected by HydroRepresentationAudit. Set expect_no_untracked_fine_flux=true
## only for controlled fixtures that intentionally run without that autoload.
func request_representation_audit(expect_no_untracked_fine_flux: bool = false,
		abs_tolerance_m3: float = 0.01,
		relative_tolerance: float = 1.0e-6) -> int:
	if not representation_audit_available() or _representation_audit.pending():
		return -1
	return _representation_audit.request_audit(expect_no_untracked_fine_flux,
		abs_tolerance_m3, relative_tolerance)


func coarse_promotion_candidates(max_count: int = 64,
		surface_depth_threshold_m: float = 0.025,
		discharge_ratio_threshold: float = 2.0) -> Array[Dictionary]:
	if not PersistentHydrologySystem.available():
		return []
	return PersistentHydrologySystem.promotion_candidates(max_count,
		surface_depth_threshold_m, discharge_ratio_threshold)


func suggested_surface_promotion_volume_m3(cell: int) -> float:
	if not planet_promotion_bridge_available():
		return 0.0
	return _planet_promotion_bridge.suggested_surface_volume_m3(cell)


## Explicit coarse -> fine transfer. requested_volume_m3 < 0 selects the
## flood-oriented default parcel (coarse surface depth over one fine tile area).
## Returns the asynchronous bridge request ID, or -1 when unavailable/rejected.
func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if not planet_promotion_bridge_available() or _planet_promotion_bridge.busy():
		return -1
	if _planet_demotion_bridge != null and _planet_demotion_bridge.busy():
		return -1
	var volume := requested_volume_m3
	if volume < 0.0:
		volume = _planet_promotion_bridge.suggested_surface_volume_m3(cell)
	if not is_finite(volume) or volume <= 0.0:
		return -1
	return _planet_promotion_bridge.promote_cell(cell, volume, local_velocity)


## Explicit fine -> coarse surface collapse. This initial reverse path is only for a
## tile that maps back to the supplied coarse cell; all reduced fine water returns
## to coarse surface storage.
func demote_fine_surface_cell(cell: int) -> int:
	if not planet_demotion_bridge_available() or _planet_demotion_bridge.busy():
		return -1
	if _planet_promotion_bridge != null and _planet_promotion_bridge.busy():
		return -1
	return _planet_demotion_bridge.demote_cell(cell)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["planet_promotion"] = {
		"state": _planet_promotion_state,
		"available": planet_promotion_bridge_available(),
		"busy": _planet_promotion_bridge != null and _planet_promotion_bridge.busy(),
		"automatic_enabled": automatic_coarse_promotion_enabled,
		"coarse_store_available": PersistentHydrologySystem.available(),
	}
	out["planet_demotion"] = {
		"state": _planet_demotion_state,
		"available": planet_demotion_bridge_available(),
		"busy": _planet_demotion_bridge != null and _planet_demotion_bridge.busy(),
		"automatic_enabled": automatic_fine_demotion_enabled,
		"surface_only": true,
	}
	out["frontier_coarse_preseed"] = {
		"available": frontier_coarse_preseed_available(),
		"pending": 0 if _frontier_coarse_preseed == null \
			else _frontier_coarse_preseed.pending_count(),
		"provisional_commits": 0 if _frontier_coarse_preseed == null \
			else _frontier_coarse_preseed.provisional_commit_count(),
		"deferred_restores": 0 if _frontier_coarse_preseed == null \
			else _frontier_coarse_preseed.deferred_restore_count(),
	}
	out["active_sparse_volume_diagnostic"] = {} if _sparse_volume_diagnostic == null \
		else _sparse_volume_diagnostic.stats()
	var fine_ledger := get_node_or_null("/root/HydroFineExternalFluxLedger")
	var fine_ledger_stats: Dictionary = {}
	if fine_ledger != null and fine_ledger.has_method("stats"):
		var ledger_value: Variant = fine_ledger.call("stats")
		if ledger_value is Dictionary:
			fine_ledger_stats = ledger_value
	out["fine_external_flux_ledger"] = fine_ledger_stats
	out["representation_audit"] = {
		"available": representation_audit_available(),
		"pending": _representation_audit != null and _representation_audit.pending(),
		"fine_external_flux_ledger_complete": bool(fine_ledger_stats.get("complete", false)),
	}
	var runtime := sparse_runtime()
	out["failed_generation_preserved"] = sparse_runtime_state() == "failed" \
		and runtime != null and runtime.scheduler != null \
		and runtime.scheduler.pool != null and runtime.scheduler.pool.allocated_count() > 0
	return out


func _on_sparse_runtime_ready_for_promotion() -> void:
	_failed_runtime_teardown_queued = false
	_try_bind_sparse_volume_diagnostic()
	_try_bind_frontier_coarse_preseed()
	_try_bind_planet_promotion_bridge()
	_try_bind_planet_demotion_bridge()


func _on_sparse_runtime_state_for_promotion(state: String) -> void:
	if state == "ready":
		_failed_runtime_teardown_queued = false
		_try_bind_sparse_volume_diagnostic()
		_try_bind_frontier_coarse_preseed()
		_try_bind_planet_promotion_bridge()
		_try_bind_planet_demotion_bridge()
		return

	if state == "failed":
		_release_representation_audit()
		_release_sparse_volume_diagnostic()
		if _planet_demotion_bridge != null and not _planet_demotion_bridge.busy():
			_release_planet_demotion_bridge()
		if _planet_promotion_bridge != null:
			_release_planet_promotion_bridge()
		_set_planet_promotion_state("failed")
		_set_planet_demotion_state("failed")
		if not _failed_runtime_teardown_queued:
			_failed_runtime_teardown_queued = true
			call_deferred(&"_teardown_failed_sparse_runtime")
		return

	_release_representation_audit()
	_release_sparse_volume_diagnostic()
	_release_frontier_coarse_preseed()
	_release_planet_demotion_bridge()
	if _planet_promotion_bridge != null:
		_release_planet_promotion_bridge()
	if state == "disabled":
		_set_planet_promotion_state("disabled")
		_set_planet_demotion_state("disabled")
	elif state == "unavailable_no_rendering_device":
		_set_planet_promotion_state("unavailable")
		_set_planet_demotion_state("unavailable")
	else:
		_set_planet_promotion_state("waiting_for_sparse_runtime")
		_set_planet_demotion_state("waiting_for_sparse_runtime")


func _teardown_failed_sparse_runtime() -> void:
	if sparse_runtime_state() != "failed":
		_failed_runtime_teardown_queued = false
		return
	if _planet_demotion_bridge != null and _planet_demotion_bridge.busy():
		return
	var runtime := sparse_runtime()
	if runtime != null and runtime.scheduler != null and runtime.scheduler.pool != null \
			and runtime.scheduler.pool.allocated_count() > 0:
		_failed_runtime_teardown_queued = false
		return
	_failed_runtime_teardown_queued = false
	_release_sparse_runtime()


func _resume_failed_teardown_after_demotion() -> void:
	if _failed_runtime_teardown_queued and sparse_runtime_state() == "failed":
		call_deferred(&"_teardown_failed_sparse_runtime")


func _on_persistent_hydrology_store_rebuilt() -> void:
	_failed_runtime_teardown_queued = false
	_release_sparse_runtime()
	_set_sparse_state("recycling_coarse_store")
	call_deferred(&"_bootstrap_sparse_runtime")


func _try_bind_sparse_volume_diagnostic() -> void:
	if _sparse_volume_diagnostic != null or not sparse_runtime_available():
		return
	var runtime := sparse_runtime()
	if runtime == null or runtime.atlas == null or runtime.solver == null \
			or not runtime.solver.initialized_ok():
		return
	var diagnostic := SparseHydroVolumeDiagnosticsGPU.new()
	diagnostic.name = "SparseHydroVolumeDiagnosticsGPU"
	_sparse_volume_diagnostic = diagnostic
	add_child(diagnostic)
	diagnostic.initialized.connect(
		func():
			if diagnostic == _sparse_volume_diagnostic:
				_try_bind_representation_audit())
	diagnostic.initialization_failed.connect(
		func(error: Error):
			if diagnostic == _sparse_volume_diagnostic:
				active_sparse_volume_failed.emit(-1, error)
				_release_representation_audit()
				_release_sparse_volume_diagnostic())
	diagnostic.volume_ready.connect(
		func(request_id: int, volume_m3: float):
			if diagnostic == _sparse_volume_diagnostic:
				active_sparse_volume_ready.emit(request_id, volume_m3))
	diagnostic.readback_failed.connect(
		func(request_id: int, error: Error):
			if diagnostic == _sparse_volume_diagnostic:
				active_sparse_volume_failed.emit(request_id, error))
	var err := diagnostic.initialize(runtime.solver.canonical_state_rid(),
		runtime.atlas.occupancy_rid(), runtime.atlas.capacity,
		runtime.atlas.tile_resolution, runtime.atlas.cell_size_m)
	if err != OK:
		active_sparse_volume_failed.emit(-1, err)
		_release_sparse_volume_diagnostic()


func _try_bind_representation_audit() -> void:
	if _representation_audit != null \
			or not active_sparse_volume_diagnostic_available() \
			or not PersistentHydrologySystem.available() \
			or not sparse_runtime_available():
		return
	var runtime := sparse_runtime()
	if runtime == null:
		return
	var audit := HydroRepresentationAudit.new()
	audit.name = "HydroRepresentationAudit"
	_representation_audit = audit
	add_child(audit)
	audit.audit_ready.connect(
		func(audit_id: int, report: Dictionary):
			if audit == _representation_audit:
				representation_audit_ready.emit(audit_id, report.duplicate(true)))
	audit.audit_failed.connect(
		func(audit_id: int, error: Error, stage: String):
			if audit == _representation_audit:
				representation_audit_failed.emit(audit_id, error, stage))
	var err := audit.initialize(PersistentHydrologySystem.store(), runtime,
		_sparse_volume_diagnostic, PersistentHydrologySystem)
	if err != OK:
		representation_audit_failed.emit(-1, err, "audit_initialize")
		_release_representation_audit()


func _try_bind_frontier_coarse_preseed() -> void:
	if _frontier_coarse_preseed != null or not sparse_runtime_available() \
			or not PersistentHydrologySystem.available():
		return
	var runtime := sparse_runtime()
	if runtime == null or runtime.atlas == null or runtime.activation == null \
			or not runtime.activation.initialized_ok():
		return
	var coordinator := HydroFrontierCoarsePreseed.new()
	coordinator.name = "HydroFrontierCoarsePreseed"
	_frontier_coarse_preseed = coordinator
	add_child(coordinator)
	coordinator.initialized.connect(
		func():
			if coordinator != _frontier_coarse_preseed:
				return
			var current := sparse_runtime()
			if current == null or current != runtime or current.activation == null:
				_release_frontier_coarse_preseed()
				return
			var attach_error := current.activation.set_coarse_preseed(coordinator)
			if attach_error != OK:
				frontier_coarse_preseed_failed.emit(attach_error)
				_release_frontier_coarse_preseed()
				return
			frontier_coarse_preseed_ready.emit())
	coordinator.initialization_failed.connect(
		func(error: Error):
			if coordinator == _frontier_coarse_preseed:
				frontier_coarse_preseed_failed.emit(error)
				_release_frontier_coarse_preseed())
	var err := coordinator.initialize(PersistentHydrologySystem.store(), runtime.atlas)
	if err != OK:
		frontier_coarse_preseed_failed.emit(err)
		_release_frontier_coarse_preseed()


func _try_bind_planet_promotion_bridge() -> void:
	if _planet_promotion_bridge != null:
		return
	if not sparse_runtime_available():
		_set_planet_promotion_state("waiting_for_sparse_runtime")
		return
	if not PersistentHydrologySystem.available():
		_set_planet_promotion_state("waiting_for_coarse_store")
		return
	var runtime := sparse_runtime()
	if runtime == null or runtime.terrain_bed == null \
			or not runtime.terrain_bed.initialized_ok():
		_set_planet_promotion_state("waiting_for_sparse_terrain")
		return
	var bridge := PlanetHydroPromotionBridge.new()
	bridge.name = "PlanetHydroPromotionBridge"
	_planet_promotion_bridge = bridge
	add_child(bridge)
	bridge.initialized.connect(func(): _on_planet_promotion_bridge_initialized(bridge))
	bridge.initialization_failed.connect(func(error: Error):
		_on_planet_promotion_bridge_initialization_failed(bridge, error))
	bridge.promotion_completed.connect(func(_request_id: int, report: Dictionary):
		if bridge == _planet_promotion_bridge:
			planet_promotion_completed.emit(report.duplicate(true)))
	bridge.promotion_failed.connect(func(_request_id: int, error: Error, stage: String):
		if bridge == _planet_promotion_bridge:
			planet_promotion_failed.emit(error, stage))
	_set_planet_promotion_state("initializing")
	var err := bridge.initialize(PersistentHydrologySystem.store(),
		runtime.scheduler, runtime.atlas, runtime.connectivity,
		runtime.identity_bridge, runtime.terrain_bed, runtime)
	if err != OK:
		_on_planet_promotion_bridge_initialization_failed(bridge, err)


func _try_bind_planet_demotion_bridge() -> void:
	if _planet_demotion_bridge != null:
		return
	if not sparse_runtime_available():
		_set_planet_demotion_state("waiting_for_sparse_runtime")
		return
	if not PersistentHydrologySystem.available():
		_set_planet_demotion_state("waiting_for_coarse_store")
		return
	var runtime := sparse_runtime()
	if runtime == null or runtime.scheduler == null or runtime.atlas == null \
			or runtime.connectivity == null or runtime.identity_bridge == null:
		_set_planet_demotion_state("waiting_for_sparse_runtime")
		return
	var bridge := PlanetHydroDemotionBridge.new()
	bridge.name = "PlanetHydroDemotionBridge"
	_planet_demotion_bridge = bridge
	add_child(bridge)
	bridge.initialized.connect(func(): _on_planet_demotion_bridge_initialized(bridge))
	bridge.initialization_failed.connect(func(error: Error):
		_on_planet_demotion_bridge_initialization_failed(bridge, error))
	bridge.demotion_completed.connect(func(_request_id: int, report: Dictionary):
		if bridge == _planet_demotion_bridge:
			planet_demotion_completed.emit(report.duplicate(true))
			_resume_failed_teardown_after_demotion())
	bridge.demotion_failed.connect(func(_request_id: int, error: Error, stage: String):
		if bridge == _planet_demotion_bridge:
			planet_demotion_failed.emit(error, stage)
			_resume_failed_teardown_after_demotion())
	_set_planet_demotion_state("initializing")
	var err := bridge.initialize(PersistentHydrologySystem.store(),
		runtime.scheduler, runtime.atlas, runtime.connectivity,
		runtime.identity_bridge, runtime)
	if err != OK:
		_on_planet_demotion_bridge_initialization_failed(bridge, err)


func _on_planet_promotion_bridge_initialized(bridge: PlanetHydroPromotionBridge) -> void:
	if bridge != _planet_promotion_bridge:
		return
	_set_planet_promotion_state("ready")
	planet_promotion_bridge_ready.emit()


func _on_planet_promotion_bridge_initialization_failed(
		bridge: PlanetHydroPromotionBridge, error: Error) -> void:
	if bridge != _planet_promotion_bridge:
		return
	_set_planet_promotion_state("failed")
	planet_promotion_failed.emit(error, "bridge_initialize")
	bridge.release()
	bridge.queue_free()
	_planet_promotion_bridge = null


func _on_planet_demotion_bridge_initialized(bridge: PlanetHydroDemotionBridge) -> void:
	if bridge != _planet_demotion_bridge:
		return
	_set_planet_demotion_state("ready")
	planet_demotion_bridge_ready.emit()


func _on_planet_demotion_bridge_initialization_failed(
		bridge: PlanetHydroDemotionBridge, error: Error) -> void:
	if bridge != _planet_demotion_bridge:
		return
	_set_planet_demotion_state("failed")
	planet_demotion_failed.emit(error, "bridge_initialize")
	bridge.release()
	bridge.queue_free()
	_planet_demotion_bridge = null


func _set_planet_promotion_state(state: String) -> void:
	if _planet_promotion_state == state:
		return
	_planet_promotion_state = state
	planet_promotion_bridge_state_changed.emit(state)


func _set_planet_demotion_state(state: String) -> void:
	if _planet_demotion_state == state:
		return
	_planet_demotion_state = state
	planet_demotion_bridge_state_changed.emit(state)


func _release_representation_audit() -> void:
	var audit := _representation_audit
	_representation_audit = null
	if audit != null and is_instance_valid(audit):
		audit.release()
		audit.queue_free()


func _release_sparse_volume_diagnostic() -> void:
	var diagnostic := _sparse_volume_diagnostic
	_sparse_volume_diagnostic = null
	if diagnostic != null and is_instance_valid(diagnostic):
		diagnostic.release()
		diagnostic.queue_free()


func _release_frontier_coarse_preseed() -> void:
	var coordinator := _frontier_coarse_preseed
	_frontier_coarse_preseed = null
	var runtime := sparse_runtime()
	if runtime != null and runtime.activation != null \
			and runtime.activation.coarse_preseed == coordinator \
			and not runtime.activation.busy():
		runtime.activation.set_coarse_preseed(null)
	if coordinator != null and is_instance_valid(coordinator):
		coordinator.release()
		coordinator.queue_free()


func _release_planet_promotion_bridge() -> void:
	var bridge := _planet_promotion_bridge
	_planet_promotion_bridge = null
	if bridge != null and is_instance_valid(bridge):
		bridge.release()
		bridge.queue_free()
	_set_planet_promotion_state("offline")


func _release_planet_demotion_bridge() -> void:
	var bridge := _planet_demotion_bridge
	_planet_demotion_bridge = null
	if bridge != null and is_instance_valid(bridge):
		bridge.release()
		bridge.queue_free()
	_set_planet_demotion_state("offline")


func _release_sparse_runtime() -> void:
	_release_representation_audit()
	_release_sparse_volume_diagnostic()
	_release_frontier_coarse_preseed()
	_release_planet_demotion_bridge()
	_release_planet_promotion_bridge()
	super._release_sparse_runtime()
