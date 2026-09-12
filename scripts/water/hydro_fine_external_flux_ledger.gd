extends Node
## Cumulative environmental water ledger for sparse SWE.
##
## SparseHydroStepGPU reports exact gross water addition/removal for every production
## macro advance in its normal 96-byte diagnostics block. This autoload accumulates
## those compact values across sparse runtime generations. It resets only when the
## persistent coarse store is rebuilt, which is the world-hydrology generation
## boundary used by WaterSystem as well.

signal ledger_reset(generation: int)
signal ledger_updated(added_m3: float, removed_m3: float, net_m3: float)

var _runtime: SparseHydrologyRuntime
var _solver: SparseHydroStepGPU
var _generation := 0
var _cycles_accounted := 0
var _cumulative_added_m3 := 0.0
var _cumulative_removed_m3 := 0.0
var _last_step_id := -1
var _last_cycle_added_m3 := 0.0
var _last_cycle_removed_m3 := 0.0
var _complete := true


func _ready() -> void:
	process_priority = 22
	if not WaterSystem.sparse_runtime_ready.is_connected(_on_sparse_runtime_ready):
		WaterSystem.sparse_runtime_ready.connect(_on_sparse_runtime_ready)
	if not WaterSystem.sparse_runtime_state_changed.is_connected(_on_sparse_state_changed):
		WaterSystem.sparse_runtime_state_changed.connect(_on_sparse_state_changed)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(_on_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(_on_store_rebuilt)
	call_deferred(&"_try_bind")


func cumulative_added_m3() -> float:
	return _cumulative_added_m3


func cumulative_removed_m3() -> float:
	return _cumulative_removed_m3


func cumulative_net_m3() -> float:
	return _cumulative_added_m3 - _cumulative_removed_m3


func complete() -> bool:
	return _complete


func generation() -> int:
	return _generation


func stats() -> Dictionary:
	return {
		"generation": _generation,
		"bound": _solver != null and is_instance_valid(_solver),
		"complete": _complete,
		"cycles_accounted": _cycles_accounted,
		"cumulative_added_m3": _cumulative_added_m3,
		"cumulative_removed_m3": _cumulative_removed_m3,
		"cumulative_net_m3": cumulative_net_m3(),
		"last_step_id": _last_step_id,
		"last_cycle_added_m3": _last_cycle_added_m3,
		"last_cycle_removed_m3": _last_cycle_removed_m3,
		"sink_clipping_exact": true,
	}


func reset() -> void:
	_generation += 1
	_cycles_accounted = 0
	_cumulative_added_m3 = 0.0
	_cumulative_removed_m3 = 0.0
	_last_step_id = -1
	_last_cycle_added_m3 = 0.0
	_last_cycle_removed_m3 = 0.0
	_complete = true
	ledger_reset.emit(_generation)


func _on_sparse_runtime_ready() -> void:
	_try_bind()


func _on_sparse_state_changed(state: String) -> void:
	if state == "ready":
		call_deferred(&"_try_bind")
	elif _runtime != null and WaterSystem.sparse_runtime() != _runtime:
		_unbind()


func _on_store_rebuilt() -> void:
	# WaterSystem recycles sparse ownership on this same boundary. Reset first so the
	# replacement runtime begins a fresh environmental ledger generation.
	reset()
	_unbind()
	call_deferred(&"_try_bind")


func _try_bind() -> void:
	var runtime := WaterSystem.sparse_runtime()
	if runtime == null or not runtime.initialized_ok() or runtime.solver == null \
			or not runtime.solver.initialized_ok():
		return
	if _runtime == runtime and _solver == runtime.solver:
		return
	_unbind()
	_runtime = runtime
	_solver = runtime.solver
	if not _solver.diagnostics_ready.is_connected(_on_solver_diagnostics):
		_solver.diagnostics_ready.connect(_on_solver_diagnostics)


func _on_solver_diagnostics(step_id: int, diagnostics: Dictionary) -> void:
	if _solver == null or not is_instance_valid(_solver):
		_complete = false
		return
	var added := float(diagnostics.get("external_added_m3", NAN))
	var removed := float(diagnostics.get("external_removed_m3", NAN))
	var exact := bool(diagnostics.get("external_sink_clipping_exact", false))
	if not is_finite(added) or not is_finite(removed) or added < 0.0 or removed < 0.0 \
			or not exact:
		_complete = false
		return
	# One production runtime advance publishes exactly one diagnostics packet. Guard
	# against accidental duplicate signal delivery from a rebound solver instance.
	if step_id == _last_step_id:
		return
	_last_step_id = step_id
	_last_cycle_added_m3 = added
	_last_cycle_removed_m3 = removed
	_cumulative_added_m3 += added
	_cumulative_removed_m3 += removed
	_cycles_accounted += 1
	ledger_updated.emit(_cumulative_added_m3, _cumulative_removed_m3,
		cumulative_net_m3())


func _unbind() -> void:
	if _solver != null and is_instance_valid(_solver) \
			and _solver.diagnostics_ready.is_connected(_on_solver_diagnostics):
		_solver.diagnostics_ready.disconnect(_on_solver_diagnostics)
	_runtime = null
	_solver = null
	_last_step_id = -1


func _exit_tree() -> void:
	_unbind()
	if WaterSystem.sparse_runtime_ready.is_connected(_on_sparse_runtime_ready):
		WaterSystem.sparse_runtime_ready.disconnect(_on_sparse_runtime_ready)
	if WaterSystem.sparse_runtime_state_changed.is_connected(_on_sparse_state_changed):
		WaterSystem.sparse_runtime_state_changed.disconnect(_on_sparse_state_changed)
	if PersistentHydrologySystem.store_rebuilt.is_connected(_on_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.disconnect(_on_store_rebuilt)
