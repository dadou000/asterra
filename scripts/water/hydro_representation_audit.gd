class_name HydroRepresentationAudit
extends Node
## Coordinated coarse + fine hydrology accounting snapshot.
##
## A coherent audit requires the CPU coarse store and GPU sparse atlas to refer to
## the same instant. request_audit() therefore accepts work only while sparse SWE is
## idle and no ownership transaction is pending, temporarily disables both runtime
## owners, snapshots the coarse/environmental ledgers, requests the four-byte
## occupied sparse volume reduction, then restores the exact previous enabled
## states.
##
## SparseHydroStepGPU now exposes exact gross fine additions and sink-clipped
## removals in its normal diagnostics. HydroFineExternalFluxLedger accumulates those
## values across sparse runtime generations, allowing strict environmental closure
## during normal production forcing rather than only no-source tests.

signal audit_started(audit_id: int, sparse_volume_request_id: int)
signal audit_ready(audit_id: int, report: Dictionary)
signal audit_failed(audit_id: int, error: Error, stage: String)
signal released

var store: PlanetHydrologyOwnershipStore
var runtime: SparseHydrologyRuntime
var diagnostic: SparseHydroVolumeDiagnosticsGPU
var coarse_owner: Node
var fine_flux_ledger: Node

var _initialized := false
var _pending := false
var _next_audit_id := 1
var _audit_id := -1
var _volume_request_id := -1
var _coarse_snapshot: Dictionary = {}
var _expect_closed_external_balance := false
var _abs_tolerance_m3 := 0.01
var _relative_tolerance := 1.0e-6
var _runtime_was_enabled := false
var _coarse_was_enabled := false


func initialize(p_store: PlanetHydrologyOwnershipStore,
		p_runtime: SparseHydrologyRuntime,
		p_diagnostic: SparseHydroVolumeDiagnosticsGPU,
		p_coarse_owner: Node,
		p_fine_flux_ledger: Node = null) -> Error:
	if _initialized or _pending:
		return ERR_BUSY
	if p_store == null or not p_store.initialized \
			or p_runtime == null or not p_runtime.initialized_ok() \
			or p_diagnostic == null or not p_diagnostic.initialized_ok() \
			or p_coarse_owner == null:
		return ERR_UNCONFIGURED
	if not _has_enabled_property(p_coarse_owner):
		return ERR_INVALID_PARAMETER
	store = p_store
	runtime = p_runtime
	diagnostic = p_diagnostic
	coarse_owner = p_coarse_owner
	fine_flux_ledger = p_fine_flux_ledger
	if fine_flux_ledger == null:
		fine_flux_ledger = get_node_or_null("/root/HydroFineExternalFluxLedger")
	diagnostic.volume_ready.connect(_on_sparse_volume_ready)
	diagnostic.readback_failed.connect(_on_sparse_volume_failed)
	_initialized = true
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _pending


## `expect_no_untracked_fine_flux` remains for controlled fixtures that intentionally
## have no fine forcing and do not instantiate the production ledger. In production,
## a complete HydroFineExternalFluxLedger makes strict balance testable automatically.
func request_audit(expect_no_untracked_fine_flux: bool = false,
		abs_tolerance_m3: float = 0.01,
		relative_tolerance: float = 1.0e-6) -> int:
	if not _initialized or _pending or store == null or runtime == null \
			or diagnostic == null or coarse_owner == null:
		return -1
	if runtime.busy() or diagnostic.pending() \
			or store.pending_ownership_transaction_count() > 0:
		return -1
	if not is_finite(abs_tolerance_m3) or abs_tolerance_m3 < 0.0 \
			or not is_finite(relative_tolerance) or relative_tolerance < 0.0:
		return -1

	_pending = true
	_audit_id = _next_audit_id
	_next_audit_id += 1
	_expect_closed_external_balance = expect_no_untracked_fine_flux
	_abs_tolerance_m3 = abs_tolerance_m3
	_relative_tolerance = relative_tolerance

	_runtime_was_enabled = runtime.enabled
	_coarse_was_enabled = bool(coarse_owner.get("enabled"))
	runtime.enabled = false
	coarse_owner.set("enabled", false)
	_coarse_snapshot = _capture_coarse_snapshot()
	if _coarse_snapshot.is_empty():
		_fail(ERR_INVALID_DATA, "coarse_snapshot")
		return -1

	_volume_request_id = diagnostic.request_volume()
	if _volume_request_id < 0:
		_fail(ERR_BUSY, "sparse_volume_submit")
		return -1
	audit_started.emit(_audit_id, _volume_request_id)
	return _audit_id


func _capture_coarse_snapshot() -> Dictionary:
	if store == null or not store.initialized \
			or store.pending_ownership_transaction_count() > 0:
		return {}
	var fine_added := 0.0
	var fine_removed := 0.0
	var fine_complete := false
	var fine_generation := -1
	if fine_flux_ledger != null and is_instance_valid(fine_flux_ledger):
		if fine_flux_ledger.has_method("complete"):
			fine_complete = bool(fine_flux_ledger.call("complete"))
		if fine_flux_ledger.has_method("cumulative_added_m3"):
			fine_added = float(fine_flux_ledger.call("cumulative_added_m3"))
		if fine_flux_ledger.has_method("cumulative_removed_m3"):
			fine_removed = float(fine_flux_ledger.call("cumulative_removed_m3"))
		if fine_flux_ledger.has_method("generation"):
			fine_generation = int(fine_flux_ledger.call("generation"))
	if not is_finite(fine_added) or not is_finite(fine_removed) \
			or fine_added < 0.0 or fine_removed < 0.0:
		fine_complete = false
		fine_added = 0.0
		fine_removed = 0.0
	return {
		"coarse_storage_m3": store.total_storage_m3(),
		"initial_storage_m3": store.initial_storage_m3,
		"precipitation_input_m3": store.cumulative_precipitation_m3,
		"climatology_input_m3": store.cumulative_climatology_input_m3,
		"outlet_export_m3": store.cumulative_outlet_m3,
		"promoted_to_fine_m3": store.cumulative_promoted_to_fine_m3,
		"demoted_from_fine_m3": store.cumulative_demoted_from_fine_m3,
		"fine_external_added_m3": fine_added,
		"fine_external_removed_m3": fine_removed,
		"fine_external_net_m3": fine_added - fine_removed,
		"fine_external_ledger_complete": fine_complete,
		"fine_external_ledger_generation": fine_generation,
		"coarse_mass_error_m3": store.mass_error_m3(),
		"coarse_mass_relative_error": store.mass_relative_error(),
		"coarse_step_count": store.step_count,
		"coarse_simulated_seconds": store.simulated_seconds,
	}


func _on_sparse_volume_ready(request_id: int, fine_volume_m3: float) -> void:
	if not _pending or request_id != _volume_request_id:
		return
	if not is_finite(fine_volume_m3) or fine_volume_m3 < 0.0:
		_fail(ERR_INVALID_DATA, "sparse_volume_value")
		return

	var coarse_storage := float(_coarse_snapshot.get("coarse_storage_m3", 0.0))
	var initial := float(_coarse_snapshot.get("initial_storage_m3", 0.0))
	var precip := float(_coarse_snapshot.get("precipitation_input_m3", 0.0))
	var climatology := float(_coarse_snapshot.get("climatology_input_m3", 0.0))
	var outlet := float(_coarse_snapshot.get("outlet_export_m3", 0.0))
	var promoted := float(_coarse_snapshot.get("promoted_to_fine_m3", 0.0))
	var demoted := float(_coarse_snapshot.get("demoted_from_fine_m3", 0.0))
	var fine_added := float(_coarse_snapshot.get("fine_external_added_m3", 0.0))
	var fine_removed := float(_coarse_snapshot.get("fine_external_removed_m3", 0.0))
	var fine_ledger_complete := bool(
		_coarse_snapshot.get("fine_external_ledger_complete", false))
	var combined_storage := coarse_storage + fine_volume_m3
	var external_budget_before_outlet := initial + precip + climatology \
		+ fine_added - fine_removed
	var expected_owned_after_outlet := external_budget_before_outlet - outlet
	var environmental_residual := combined_storage - expected_owned_after_outlet
	var net_promoted := promoted - demoted
	var fine_minus_net_promoted := fine_volume_m3 - net_promoted
	var scale := maxf(maxf(absf(external_budget_before_outlet), absf(combined_storage)), 1.0)
	var tolerance := maxf(_abs_tolerance_m3, scale * _relative_tolerance)
	var strict_testable := fine_ledger_complete or _expect_closed_external_balance
	var strict_pass := absf(environmental_residual) <= tolerance

	var report := _coarse_snapshot.duplicate(true)
	report["audit_id"] = _audit_id
	report["sparse_volume_request_id"] = _volume_request_id
	report["active_sparse_volume_m3"] = fine_volume_m3
	report["combined_owned_storage_m3"] = combined_storage
	report["external_budget_before_outlet_m3"] = external_budget_before_outlet
	report["expected_owned_after_outlet_m3"] = expected_owned_after_outlet
	report["environmental_balance_residual_m3"] = environmental_residual
	report["net_promoted_to_fine_m3"] = net_promoted
	report["fine_minus_net_promoted_m3"] = fine_minus_net_promoted
	report["strict_environmental_balance_requested"] = _expect_closed_external_balance
	report["strict_environmental_balance_testable"] = strict_testable
	report["strict_environmental_balance_pass"] = strict_pass if strict_testable else false
	report["untracked_fine_external_flux_possible"] = not strict_testable
	report["tolerance_m3"] = tolerance
	report["ownership_transaction_pending"] = false

	var completed_id := _audit_id
	_restore_execution()
	_clear_request()
	audit_ready.emit(completed_id, report)


func _on_sparse_volume_failed(request_id: int, error: Error) -> void:
	if _pending and request_id == _volume_request_id:
		_fail(error, "sparse_volume_readback")


func _fail(error: Error, stage: String) -> void:
	var failed_id := _audit_id
	_restore_execution()
	_clear_request()
	audit_failed.emit(failed_id, error, stage)


func _restore_execution() -> void:
	if runtime != null and is_instance_valid(runtime):
		runtime.enabled = _runtime_was_enabled
	if coarse_owner != null and is_instance_valid(coarse_owner) \
			and _has_enabled_property(coarse_owner):
		coarse_owner.set("enabled", _coarse_was_enabled)


func _clear_request() -> void:
	_pending = false
	_audit_id = -1
	_volume_request_id = -1
	_coarse_snapshot.clear()
	_expect_closed_external_balance = false
	_runtime_was_enabled = false
	_coarse_was_enabled = false


func _has_enabled_property(node: Node) -> bool:
	for property in node.get_property_list():
		if StringName(property.get("name", &"")) == &"enabled":
			return true
	return false


func release() -> void:
	if _pending:
		_restore_execution()
	_clear_request()
	if diagnostic != null:
		if diagnostic.volume_ready.is_connected(_on_sparse_volume_ready):
			diagnostic.volume_ready.disconnect(_on_sparse_volume_ready)
		if diagnostic.readback_failed.is_connected(_on_sparse_volume_failed):
			diagnostic.readback_failed.disconnect(_on_sparse_volume_failed)
	store = null
	runtime = null
	diagnostic = null
	coarse_owner = null
	fine_flux_ledger = null
	_initialized = false
	released.emit()


func _exit_tree() -> void:
	release()
