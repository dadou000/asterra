class_name HydroAutomaticChannelRefinement
extends Node
## Low-cadence policy for automatic persistent-1D <-> sparse-2D river refinement.
##
## This node owns policy only. Physical transfers always go through WaterSystem's
## transactional river promotion/collapse bridges and continuous coupling registry.
## It is completely camera-independent and both production switches remain OFF by
## default in WaterSystem.
##
## Promotion:
##   abnormal 1D discharge OR near-bankfull stage
##   -> priority by anomaly + stream order + precomputed confluence indegree
##   -> one transactional river promotion
##
## Collapse (only reaches automatically promoted by this policy):
##   fine corridor passes HydroRiverCollapsePolicy
##   AND measured fine downstream Q has fallen below exit hysteresis
##   AND coarse residual bank stage has fallen below exit hysteresis
##   -> one transactional river collapse

var scan_interval_s := 4.0
var candidate_limit := 64
var max_auto_refined_reaches := 8
var promotion_discharge_ratio_enter := 2.0
var promotion_discharge_ratio_exit := 1.35
var promotion_bankfull_ratio_enter := 0.85
var promotion_bankfull_ratio_exit := 0.60
var minimum_stream_order := 2
var retry_cooldown_scans := 3
var post_collapse_cooldown_scans := 5

var _timer: Timer
var _water_override: Node
var _persistent_override: Node
var _active_kind := "" # promotion | collapse
var _active_request_id := -1
var _active_cell := -1
var _auto_owned: Dictionary = {} # cell -> tile_id
var _cooldown_until_scan: Dictionary = {} # cell -> scan index
var _upstream_indegree := PackedInt32Array()
var _topology_generation := -1

var _scan_count := 0
var _promotion_started := 0
var _promotion_completed := 0
var _promotion_failed := 0
var _collapse_started := 0
var _collapse_completed := 0
var _collapse_failed := 0
var _capacity_suppressed := 0
var _cooldown_suppressed := 0
var _resident_suppressed := 0
var _last_candidate_count := 0
var _last_reason := "disabled"
var _last_failure_stage := ""


## Test seam: production resolves WaterSystem/PersistentHydrologySystem autoloads.
func bind_facades_for_test(water_facade: Node, persistent_facade: Node) -> Error:
	if is_inside_tree() or water_facade == null or persistent_facade == null:
		return ERR_INVALID_PARAMETER
	_water_override = water_facade
	_persistent_override = persistent_facade
	return OK


func _ready() -> void:
	process_priority = 22
	_timer = Timer.new()
	_timer.name = "ChannelRefinementTimer"
	_timer.one_shot = false
	_timer.wait_time = maxf(scan_interval_s, 0.1)
	_timer.timeout.connect(scan_once)
	add_child(_timer)
	_timer.start()
	_bind_signals()
	_rebuild_topology_cache()


func stats() -> Dictionary:
	return {
		"promotion_enabled": _promotion_enabled(),
		"collapse_enabled": _collapse_enabled(),
		"scan_interval_s": scan_interval_s,
		"max_auto_refined_reaches": max_auto_refined_reaches,
		"auto_owned_reaches": _auto_owned.size(),
		"active_kind": _active_kind,
		"active_request_id": _active_request_id,
		"active_cell": _active_cell,
		"scan_count": _scan_count,
		"last_candidate_count": _last_candidate_count,
		"promotions_started": _promotion_started,
		"promotions_completed": _promotion_completed,
		"promotions_failed": _promotion_failed,
		"collapses_started": _collapse_started,
		"collapses_completed": _collapse_completed,
		"collapses_failed": _collapse_failed,
		"capacity_suppressed": _capacity_suppressed,
		"cooldown_suppressed": _cooldown_suppressed,
		"resident_suppressed": _resident_suppressed,
		"last_reason": _last_reason,
		"last_failure_stage": _last_failure_stage,
		"camera_dependent": false,
	}


## Pure hysteresis helper for the headless policy gate.
static func promotion_anomaly_active(q_ratio: float, bank_ratio: float,
		was_latched: bool, q_enter: float, q_exit: float,
		bank_enter: float, bank_exit: float) -> bool:
	if not is_finite(q_ratio) or not is_finite(bank_ratio):
		return false
	var qe := maxf(q_enter, 1.0)
	var qx := clampf(q_exit, 0.0, qe)
	var be := maxf(bank_enter, 1.0e-6)
	var bx := clampf(bank_exit, 0.0, be)
	if was_latched:
		return q_ratio >= qx or bank_ratio >= bx
	return q_ratio >= qe or bank_ratio >= be


static func priority_score(q_ratio: float, bank_ratio: float, stream_order: int,
		upstream_indegree: int, q_enter: float, bank_enter: float) -> float:
	if not is_finite(q_ratio) or not is_finite(bank_ratio):
		return -INF
	var anomaly := maxf(q_ratio / maxf(q_enter, 1.0e-6),
		bank_ratio / maxf(bank_enter, 1.0e-6))
	var order_bonus := 0.04 * float(maxi(stream_order, 0))
	var confluence_bonus := 0.10 * float(clampi(upstream_indegree - 1, 0, 4))
	return anomaly + order_bonus + confluence_bonus


static func collapse_hysteresis_clear(measured_q_ratio: float,
		residual_bank_ratio: float, q_exit: float, bank_exit: float) -> bool:
	return is_finite(measured_q_ratio) and is_finite(residual_bank_ratio) \
		and measured_q_ratio < maxf(q_exit, 0.0) \
		and residual_bank_ratio < maxf(bank_exit, 0.0)


func scan_once() -> void:
	if _timer != null:
		_timer.wait_time = maxf(scan_interval_s, 0.1)
	_scan_count += 1
	_last_candidate_count = 0
	var water := _water_facade()
	var persistent := _persistent_facade()
	if water == null or persistent == null:
		_last_reason = "facade_unavailable"
		return
	if not _promotion_enabled() and not _collapse_enabled():
		_last_reason = "disabled"
		return
	if not _active_kind.is_empty():
		_last_reason = "policy_transaction_active"
		return
	if not bool(persistent.call("available")):
		_last_reason = "persistent_store_unavailable"
		return
	var store_value: Variant = persistent.call("store")
	if not (store_value is PlanetHydrologyRiverCoupledStore):
		_last_reason = "coupled_store_unavailable"
		return
	var store := store_value as PlanetHydrologyRiverCoupledStore
	_ensure_topology_cache(store)
	_prune_auto_owned(store)

	# Reverse policy first. Once a policy-owned fine corridor is quiet and the
	# anomaly has exited the hysteresis band, freeing it reduces GPU pressure before
	# considering another refinement.
	if _collapse_enabled() and _try_start_collapse(water, store):
		return
	if not _promotion_enabled():
		_last_reason = "promotion_disabled_no_collapse"
		return
	_try_start_promotion(water, persistent, store)


func _try_start_promotion(water: Node, persistent: Node,
		store: PlanetHydrologyRiverCoupledStore) -> bool:
	if not bool(water.call("river_reach_promotion_bridge_available")) \
			or not bool(water.call("river_reach_coupling_available")):
		_last_reason = "river_promotion_stack_unavailable"
		return false
	if store.refined_reach_count() >= maxi(max_auto_refined_reaches, 0):
		_capacity_suppressed += 1
		_last_reason = "refined_capacity_reached"
		return false

	var value: Variant = water.call("channel_reach_candidates",
		maxi(candidate_limit, 1), promotion_discharge_ratio_exit,
		promotion_bankfull_ratio_exit)
	if not (value is Array):
		_last_reason = "candidate_query_invalid"
		return false
	var candidates: Array = value
	_last_candidate_count = candidates.size()
	var ranked: Array[Dictionary] = []
	for item: Variant in candidates:
		if not (item is Dictionary):
			continue
		var candidate := (item as Dictionary).duplicate(true)
		var cell := int(candidate.get("cell", -1))
		if cell < 0 or store.is_refined_reach(cell):
			continue
		if _scan_count < int(_cooldown_until_scan.get(cell, 0)):
			_cooldown_suppressed += 1
			continue
		var order := int(candidate.get("stream_order", 0))
		if order < maxi(minimum_stream_order, 0):
			continue
		var q_ratio := maxf(float(candidate.get("discharge_ratio", 0.0)), 0.0)
		var bank_ratio := maxf(float(candidate.get("bankfull_ratio", 0.0)), 0.0)
		if not promotion_anomaly_active(q_ratio, bank_ratio, false,
				promotion_discharge_ratio_enter, promotion_discharge_ratio_exit,
				promotion_bankfull_ratio_enter, promotion_bankfull_ratio_exit):
			continue
		var indegree := _upstream_indegree[cell] \
			if cell >= 0 and cell < _upstream_indegree.size() else 0
		candidate["policy_priority"] = priority_score(q_ratio, bank_ratio, order,
			indegree, promotion_discharge_ratio_enter,
			promotion_bankfull_ratio_enter)
		ranked.append(candidate)
	ranked.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("policy_priority", -INF)) \
			> float(b.get("policy_priority", -INF)))

	for candidate in ranked:
		var cell := int(candidate.get("cell", -1))
		var suggested := float(water.call("suggested_river_reach_promotion_volume_m3", cell))
		if not is_finite(suggested) or suggested <= 0.0:
			continue
		var request_id := int(water.call("promote_coarse_river_reach", cell, suggested))
		if request_id < 0:
			_set_cooldown(cell, retry_cooldown_scans)
			continue
		_active_kind = "promotion"
		_active_request_id = request_id
		_active_cell = cell
		_promotion_started += 1
		_last_reason = "promotion_started"
		return true
	_last_reason = "no_eligible_channel_candidate"
	return false


func _try_start_collapse(water: Node, store: PlanetHydrologyRiverCoupledStore) -> bool:
	if not bool(water.call("river_reach_collapse_bridge_available")):
		return false
	var coupling_value: Variant = water.call("river_reach_coupling")
	if not (coupling_value is HydroRiverReachCoupling):
		return false
	var coupling := coupling_value as HydroRiverReachCoupling
	for cell_value: Variant in _auto_owned.keys():
		var cell := int(cell_value)
		if _scan_count < int(_cooldown_until_scan.get(cell, 0)):
			continue
		if not store.is_refined_reach(cell):
			continue
		if not bool(water.call("river_reach_collapse_eligible", cell)):
			continue
		var coupling_record := coupling.registered_reach(cell)
		if coupling_record.is_empty():
			continue
		var baseline_q := maxf(float(store.fields.discharge[cell]), 0.05)
		var measured_q_ratio := maxf(float(coupling_record.get(
			"last_downstream_q_m3s", 0.0)), 0.0) / baseline_q
		var residual := store.river_reaches.reach_state(cell,
			store.channel_storage_m3[cell])
		var bank_ratio := maxf(float(residual.get("bankfull_depth_ratio", 0.0)), 0.0)
		if not collapse_hysteresis_clear(measured_q_ratio, bank_ratio,
				promotion_discharge_ratio_exit, promotion_bankfull_ratio_exit):
			continue
		var request_id := int(water.call("collapse_fine_river_reach", cell, false))
		if request_id < 0:
			_set_cooldown(cell, retry_cooldown_scans)
			continue
		_active_kind = "collapse"
		_active_request_id = request_id
		_active_cell = cell
		_collapse_started += 1
		_last_reason = "collapse_started"
		return true
	return false


func _bind_signals() -> void:
	var water := _water_facade()
	var persistent := _persistent_facade()
	if water != null:
		_connect_if_present(water, "river_reach_promotion_completed", _on_promotion_completed)
		_connect_if_present(water, "river_reach_promotion_failed", _on_promotion_failed)
		_connect_if_present(water, "river_reach_collapse_completed", _on_collapse_completed)
		_connect_if_present(water, "river_reach_collapse_failed", _on_collapse_failed)
	if persistent != null:
		_connect_if_present(persistent, "store_rebuilt", _on_store_rebuilt)


func _connect_if_present(node: Node, signal_name: StringName, callable: Callable) -> void:
	if node.has_signal(signal_name) and not node.is_connected(signal_name, callable):
		node.connect(signal_name, callable)


func _on_promotion_completed(report: Dictionary) -> void:
	if _active_kind != "promotion":
		return
	var request_id := int(report.get("request_id", -1))
	if request_id != _active_request_id:
		return
	var cell := _active_cell
	var tile_id := int(report.get("tile_id", -1))
	if cell >= 0 and tile_id >= 0:
		_auto_owned[cell] = tile_id
	_promotion_completed += 1
	_clear_active()
	_last_reason = "promotion_completed"


func _on_promotion_failed(_error: Error, stage: String) -> void:
	if _active_kind != "promotion":
		return
	_set_cooldown(_active_cell, retry_cooldown_scans)
	_promotion_failed += 1
	_last_failure_stage = stage
	_clear_active()
	_last_reason = "promotion_failed"


func _on_collapse_completed(report: Dictionary) -> void:
	if _active_kind != "collapse":
		return
	var request_id := int(report.get("request_id", -1))
	if request_id != _active_request_id:
		return
	var cell := _active_cell
	_auto_owned.erase(cell)
	_set_cooldown(cell, post_collapse_cooldown_scans)
	_collapse_completed += 1
	_clear_active()
	_last_reason = "collapse_completed"


func _on_collapse_failed(_error: Error, stage: String) -> void:
	if _active_kind != "collapse":
		return
	_set_cooldown(_active_cell, retry_cooldown_scans)
	_collapse_failed += 1
	_last_failure_stage = stage
	_clear_active()
	_last_reason = "collapse_failed"


func _on_store_rebuilt() -> void:
	_auto_owned.clear()
	_cooldown_until_scan.clear()
	_upstream_indegree = PackedInt32Array()
	_topology_generation = -1
	_clear_active()
	_last_reason = "store_rebuilt"
	call_deferred(&"_rebuild_topology_cache")


func _prune_auto_owned(store: PlanetHydrologyRiverCoupledStore) -> void:
	for value: Variant in _auto_owned.keys():
		var cell := int(value)
		if not store.is_refined_reach(cell):
			_auto_owned.erase(cell)


func _ensure_topology_cache(store: PlanetHydrologyRiverCoupledStore) -> void:
	if _upstream_indegree.size() == store.cell_count():
		return
	_build_topology_cache(store)


func _rebuild_topology_cache() -> void:
	var persistent := _persistent_facade()
	if persistent == null or not bool(persistent.call("available")):
		return
	var value: Variant = persistent.call("store")
	if value is PlanetHydrologyRiverCoupledStore:
		_build_topology_cache(value as PlanetHydrologyRiverCoupledStore)


func _build_topology_cache(store: PlanetHydrologyRiverCoupledStore) -> void:
	var n := store.cell_count()
	_upstream_indegree = PackedInt32Array()
	_upstream_indegree.resize(n)
	_upstream_indegree.fill(0)
	for c in n:
		var r := int(store.receiver[c])
		if r >= 0 and r < n and r != c:
			_upstream_indegree[r] += 1
	_topology_generation += 1


func _set_cooldown(cell: int, scans: int) -> void:
	if cell >= 0:
		_cooldown_until_scan[cell] = _scan_count + maxi(scans, 0)


func _clear_active() -> void:
	_active_kind = ""
	_active_request_id = -1
	_active_cell = -1


func _promotion_enabled() -> bool:
	var water := _water_facade()
	return water != null and bool(water.get("automatic_channel_promotion_enabled"))


func _collapse_enabled() -> bool:
	var water := _water_facade()
	return water != null and bool(water.get("automatic_channel_demotion_enabled"))


func _water_facade() -> Node:
	return _water_override if _water_override != null else WaterSystem


func _persistent_facade() -> Node:
	return _persistent_override if _persistent_override != null else PersistentHydrologySystem
