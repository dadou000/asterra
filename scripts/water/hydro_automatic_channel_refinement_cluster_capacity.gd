class_name HydroAutomaticChannelRefinementClusterCapacity
extends HydroAutomaticChannelRefinement
## Production cluster/component-aware automatic channel refinement policy.
##
## High-priority confluences are promoted as one complete connected component when
## the component planner and member budget can satisfy the whole graph. Ordinary
## reaches retain the existing cluster path. A component is never partially admitted
## to the budget and its automatic collapse is likewise component-wide.

var max_auto_refined_member_slots := 24
var automatic_confluence_components := true
var minimum_component_upstream_reaches := 2
var maximum_component_reaches := 8

var _member_budget_suppressed := 0
var _sparse_slot_suppressed := 0
var _component_capacity_suppressed := 0
var _component_plan_suppressed := 0
var _component_promotions_started := 0
var _component_collapses_started := 0
var _last_refined_member_slots := 0
var _last_requested_member_slots := 0
var _last_free_sparse_slots := 0
var _active_component_cells := PackedInt32Array()


static func member_capacity_available(current_refined_members: int,
		requested_members: int, max_member_slots: int,
		free_sparse_slots: int) -> bool:
	if current_refined_members < 0 or requested_members <= 0 \
			or free_sparse_slots < requested_members:
		return false
	var budget := maxi(max_member_slots, 0)
	return current_refined_members + requested_members <= budget


func stats() -> Dictionary:
	var out := super.stats()
	var persistent := _persistent_facade()
	if persistent != null and bool(persistent.call("available")):
		var value: Variant = persistent.call("store")
		if value is PlanetHydrologyRiverCoupledStore:
			_last_refined_member_slots = _refined_member_slots(
				value as PlanetHydrologyRiverCoupledStore)
	var water := _water_facade()
	_last_free_sparse_slots = _free_sparse_member_slots(water)
	out["max_auto_refined_member_slots"] = max_auto_refined_member_slots
	out["refined_river_member_slots"] = _last_refined_member_slots
	out["last_requested_member_slots"] = _last_requested_member_slots
	out["free_sparse_slots"] = _last_free_sparse_slots
	out["member_budget_suppressed"] = _member_budget_suppressed
	out["sparse_slot_suppressed"] = _sparse_slot_suppressed
	out["cluster_capacity_guard"] = true
	out["counts_manual_refinements"] = true
	out["automatic_confluence_components"] = automatic_confluence_components
	out["minimum_component_upstream_reaches"] = minimum_component_upstream_reaches
	out["maximum_component_reaches"] = maximum_component_reaches
	out["component_capacity_suppressed"] = _component_capacity_suppressed
	out["component_plan_suppressed"] = _component_plan_suppressed
	out["component_promotions_started"] = _component_promotions_started
	out["component_collapses_started"] = _component_collapses_started
	out["active_component_cells"] = _active_component_cells.size()
	return out


func _try_start_promotion(water: Node, persistent: Node,
		store: PlanetHydrologyRiverCoupledStore) -> bool:
	if not bool(water.call("river_reach_promotion_bridge_available")) \
			or not bool(water.call("river_reach_coupling_available")):
		_last_reason = "river_promotion_stack_unavailable"
		return false
	if store.refined_reach_count() >= maxi(max_auto_refined_reaches, 0):
		_capacity_suppressed += 1
		_last_reason = "refined_reach_capacity_reached"
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
		candidate["upstream_indegree"] = indegree
		ranked.append(candidate)
	ranked.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("policy_priority", -INF)) \
			> float(b.get("policy_priority", -INF)))

	var refined_members := _refined_member_slots(store)
	_last_refined_member_slots = refined_members
	var blocked_reason := ""
	for candidate in ranked:
		var cell := int(candidate.get("cell", -1))

		# Prefer an atomic physical confluence when the candidate is a multi-donor
		# node. No individual tributary transaction is attempted if the component can
		# be planned but is temporarily over capacity; this avoids topology thrash.
		if automatic_confluence_components and store is PlanetHydrologyRiverClusterStore \
				and int(candidate.get("upstream_indegree", 0)) \
				>= maxi(minimum_component_upstream_reaches, 2):
			var component_cells := _confluence_component_cells(
				store as PlanetHydrologyRiverClusterStore, cell)
			if component_cells.size() >= 3:
				var component_result := _try_component_promotion(water,
					store as PlanetHydrologyRiverClusterStore, component_cells,
					refined_members)
				if int(component_result.get("request_id", -1)) >= 0:
					_active_kind = "promotion"
					_active_request_id = int(component_result["request_id"])
					_active_cell = cell
					_active_component_cells = component_cells.duplicate()
					_promotion_started += 1
					_component_promotions_started += 1
					_last_reason = "component_promotion_started"
					return true
				var component_reason := String(component_result.get("reason", ""))
				if not component_reason.is_empty():
					blocked_reason = component_reason
				# A structurally valid component that is only budget/capacity blocked should
				# wait as a component rather than refine the same confluence piecemeal.
				if bool(component_result.get("structurally_valid", false)):
					continue

		var requested_members := _planned_member_slots(water, cell)
		var free_slots := _free_sparse_member_slots(water)
		_last_requested_member_slots = requested_members
		_last_free_sparse_slots = free_slots
		if requested_members <= 0:
			blocked_reason = "cluster_plan_unavailable"
			continue
		if refined_members + requested_members > maxi(max_auto_refined_member_slots, 0):
			_member_budget_suppressed += 1
			blocked_reason = "refined_member_budget_reached"
			continue
		if free_slots < requested_members:
			_sparse_slot_suppressed += 1
			blocked_reason = "sparse_member_capacity_insufficient"
			continue
		if not member_capacity_available(refined_members, requested_members,
				max_auto_refined_member_slots, free_slots):
			_member_budget_suppressed += 1
			blocked_reason = "refined_member_capacity_rejected"
			continue

		var suggested := float(water.call("suggested_river_reach_promotion_volume_m3", cell))
		if not is_finite(suggested) or suggested <= 0.0:
			continue
		var request_id := int(water.call("promote_coarse_river_reach", cell, suggested))
		if request_id < 0:
			_set_cooldown(cell, retry_cooldown_scans)
			continue
		_active_component_cells = PackedInt32Array()
		_active_kind = "promotion"
		_active_request_id = request_id
		_active_cell = cell
		_promotion_started += 1
		_last_reason = "promotion_started"
		return true

	_last_reason = blocked_reason if not blocked_reason.is_empty() \
		else "no_eligible_channel_candidate"
	return false


func _try_component_promotion(water: Node, store: PlanetHydrologyRiverClusterStore,
		cells: PackedInt32Array, refined_members: int) -> Dictionary:
	if not water.has_method("river_component_promotion_available") \
			or not bool(water.call("river_component_promotion_available")) \
			or not water.has_method("river_component_requested_member_count") \
			or not water.has_method("promote_coarse_river_component"):
		return {"request_id": -1, "reason": "component_promotion_stack_unavailable"}
	if store.refined_reach_count() + cells.size() > maxi(max_auto_refined_reaches, 0):
		_component_capacity_suppressed += 1
		return {
			"request_id": -1,
			"reason": "component_refined_reach_capacity_reached",
			"structurally_valid": true,
		}
	var requested_members := maxi(int(water.call(
		"river_component_requested_member_count", cells)), 0)
	var free_slots := _free_sparse_member_slots(water)
	_last_requested_member_slots = requested_members
	_last_free_sparse_slots = free_slots
	if requested_members <= 0:
		_component_plan_suppressed += 1
		return {"request_id": -1, "reason": "component_plan_unavailable"}
	if refined_members + requested_members > maxi(max_auto_refined_member_slots, 0):
		_component_capacity_suppressed += 1
		_member_budget_suppressed += 1
		return {
			"request_id": -1,
			"reason": "component_member_budget_reached",
			"structurally_valid": true,
		}
	if free_slots < requested_members:
		_component_capacity_suppressed += 1
		_sparse_slot_suppressed += 1
		return {
			"request_id": -1,
			"reason": "component_sparse_capacity_insufficient",
			"structurally_valid": true,
		}
	if not member_capacity_available(refined_members, requested_members,
			max_auto_refined_member_slots, free_slots):
		_component_capacity_suppressed += 1
		return {
			"request_id": -1,
			"reason": "component_member_capacity_rejected",
			"structurally_valid": true,
		}
	var request_id := int(water.call("promote_coarse_river_component", cells))
	if request_id < 0:
		for raw_cell in cells:
			_set_cooldown(int(raw_cell), retry_cooldown_scans)
		return {
			"request_id": -1,
			"reason": "component_promotion_submit_failed",
			"structurally_valid": true,
		}
	return {
		"request_id": request_id,
		"reason": "component_promotion_started",
		"structurally_valid": true,
	}


func _confluence_component_cells(store: PlanetHydrologyRiverClusterStore,
		stem: int) -> PackedInt32Array:
	var upstream: Array[int] = []
	for cell in store.cell_count():
		if int(store.receiver[cell]) != stem or store.is_refined_reach(cell) \
				or store.river_reaches == null or not store.river_reaches.is_reach_cell(cell):
			continue
		if int(store.fields.stream_order[cell]) < maxi(minimum_stream_order, 0):
			continue
		if store.available_channel_promotion_volume_m3(cell) <= 0.0:
			continue
		if _scan_count < int(_cooldown_until_scan.get(cell, 0)):
			continue
		upstream.append(cell)
	upstream.sort_custom(func(a: int, b: int) -> bool:
		var qa := maxf(float(store.fields.discharge[a]), 0.0)
		var qb := maxf(float(store.fields.discharge[b]), 0.0)
		return qa > qb)
	var required_upstream := maxi(minimum_component_upstream_reaches, 2)
	if upstream.size() < required_upstream or store.is_refined_reach(stem) \
			or store.available_channel_promotion_volume_m3(stem) <= 0.0:
		return PackedInt32Array()
	var max_reaches := maxi(maximum_component_reaches, required_upstream + 1)
	var result := PackedInt32Array()
	for i in mini(upstream.size(), max_reaches - 1):
		result.append(upstream[i])
	result.append(stem)
	return result


func _try_start_collapse(water: Node, store: PlanetHydrologyRiverCoupledStore) -> bool:
	if not bool(water.call("river_reach_collapse_bridge_available")):
		return false
	var coupling_value: Variant = water.call("river_reach_coupling")
	if not (coupling_value is HydroRiverReachCoupling):
		return false
	var coupling := coupling_value as HydroRiverReachCoupling
	var seen_components: Dictionary = {}
	for cell_value: Variant in _auto_owned.keys():
		var cell := int(cell_value)
		if _scan_count < int(_cooldown_until_scan.get(cell, 0)):
			continue
		if not store.is_refined_reach(cell):
			continue

		var component_id := -1
		var evaluation_cell := cell
		var component_cells := PackedInt32Array()
		if store is PlanetHydrologyRiverClusterStore:
			var cluster_store := store as PlanetHydrologyRiverClusterStore
			component_id = cluster_store.refined_component_id_for_cell(cell)
			if component_id >= 0:
				if seen_components.has(component_id):
					continue
				seen_components[component_id] = true
				var component := cluster_store.refined_component(component_id)
				component_cells = component.get("cells", PackedInt32Array()) as PackedInt32Array
				evaluation_cell = int(component.get("downstream_outlet_cell", cell))
				# Automatic ownership is all-or-nothing for a component. A manually
				# extended/adopted component is not collapsed by this policy.
				var all_auto_owned := not component_cells.is_empty()
				for raw_component_cell in component_cells:
					if not _auto_owned.has(int(raw_component_cell)):
						all_auto_owned = false
						break
				if not all_auto_owned:
					continue

		if not bool(water.call("river_reach_collapse_eligible", evaluation_cell)):
			continue
		var coupling_record := coupling.registered_reach(evaluation_cell)
		if coupling_record.is_empty():
			continue
		var baseline_q := maxf(float(store.fields.discharge[evaluation_cell]), 0.05)
		var measured_q_ratio := maxf(float(coupling_record.get(
			"last_downstream_q_m3s", 0.0)), 0.0) / baseline_q
		var residual := store.river_reaches.reach_state(evaluation_cell,
			store.channel_storage_m3[evaluation_cell])
		var bank_ratio := maxf(float(residual.get("bankfull_depth_ratio", 0.0)), 0.0)
		if not collapse_hysteresis_clear(measured_q_ratio, bank_ratio,
				promotion_discharge_ratio_exit, promotion_bankfull_ratio_exit):
			continue
		var request_id := int(water.call("collapse_fine_river_reach",
			evaluation_cell, false))
		if request_id < 0:
			_set_cooldown(evaluation_cell, retry_cooldown_scans)
			continue
		_active_kind = "collapse"
		_active_request_id = request_id
		_active_cell = evaluation_cell
		_active_component_cells = component_cells.duplicate()
		_collapse_started += 1
		if component_id >= 0:
			_component_collapses_started += 1
			_last_reason = "component_collapse_started"
		else:
			_last_reason = "collapse_started"
		return true
	return false


func _on_promotion_completed(report: Dictionary) -> void:
	if _active_kind != "promotion":
		return
	var request_id := int(report.get("request_id", -1))
	if request_id != _active_request_id:
		return
	if not _active_component_cells.is_empty():
		var component_id := int(report.get("component_id", -1))
		for raw_cell in _active_component_cells:
			_auto_owned[int(raw_cell)] = component_id
	else:
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
	if not _active_component_cells.is_empty():
		for raw_cell in _active_component_cells:
			_set_cooldown(int(raw_cell), retry_cooldown_scans)
	else:
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
	if not _active_component_cells.is_empty():
		for raw_cell in _active_component_cells:
			var cell := int(raw_cell)
			_auto_owned.erase(cell)
			_set_cooldown(cell, post_collapse_cooldown_scans)
	else:
		var cell := _active_cell
		_auto_owned.erase(cell)
		_set_cooldown(cell, post_collapse_cooldown_scans)
	_collapse_completed += 1
	_clear_active()
	_last_reason = "collapse_completed"


func _on_collapse_failed(_error: Error, stage: String) -> void:
	if _active_kind != "collapse":
		return
	if not _active_component_cells.is_empty():
		for raw_cell in _active_component_cells:
			_set_cooldown(int(raw_cell), retry_cooldown_scans)
	else:
		_set_cooldown(_active_cell, retry_cooldown_scans)
	_collapse_failed += 1
	_last_failure_stage = stage
	_clear_active()
	_last_reason = "collapse_failed"


func _on_store_rebuilt() -> void:
	_active_component_cells = PackedInt32Array()
	super._on_store_rebuilt()


func _clear_active() -> void:
	_active_component_cells = PackedInt32Array()
	super._clear_active()


func _refined_member_slots(store: PlanetHydrologyRiverCoupledStore) -> int:
	if store is PlanetHydrologyRiverClusterStore:
		return (store as PlanetHydrologyRiverClusterStore).refined_sparse_member_count()
	return maxi(store.refined_reach_count(), 0)


func _planned_member_slots(water: Node, cell: int) -> int:
	if water == null:
		return 0
	if water.has_method("river_cluster_requested_member_count"):
		return maxi(int(water.call("river_cluster_requested_member_count", cell)), 0)
	if water.has_method("river_cluster_default_member_count"):
		return maxi(int(water.call("river_cluster_default_member_count")), 1)
	return 1


func _free_sparse_member_slots(water: Node) -> int:
	if water != null and water.has_method("river_cluster_free_member_slots"):
		return maxi(int(water.call("river_cluster_free_member_slots")), 0)
	return maxi(max_auto_refined_member_slots, 0)
