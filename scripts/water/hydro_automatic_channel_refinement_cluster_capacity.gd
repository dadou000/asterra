class_name HydroAutomaticChannelRefinementClusterCapacity
extends HydroAutomaticChannelRefinement
## Production cluster-aware capacity policy.
##
## The base policy limits refined *coarse reaches*. Multi-tile refinement also needs
## a fine-member budget because one reach can occupy several sparse atlas slots.
## This layer counts actual members across every refined river record (manual or
## automatic) and checks the scheduler's real free-slot count before requesting the
## transactional bridge. WaterSystem independently repeats the exact free-slot guard
## immediately before promotion, so no ownership transaction can begin on stale
## capacity information.

var max_auto_refined_member_slots := 24

var _member_budget_suppressed := 0
var _sparse_slot_suppressed := 0
var _last_refined_member_slots := 0
var _last_requested_member_slots := 0
var _last_free_sparse_slots := 0


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
		ranked.append(candidate)
	ranked.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("policy_priority", -INF)) \
			> float(b.get("policy_priority", -INF)))

	var refined_members := _refined_member_slots(store)
	_last_refined_member_slots = refined_members
	var blocked_reason := ""
	for candidate in ranked:
		var cell := int(candidate.get("cell", -1))
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
		# WaterSystem re-plans the same cell and re-checks scheduler free slots here.
		# This closes the race between this policy scan and ownership reservation.
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

	_last_reason = blocked_reason if not blocked_reason.is_empty() \
		else "no_eligible_channel_candidate"
	return false


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
	# Legacy/test facades without a sparse-capacity query behave as one-tile systems;
	# the configured river-member budget remains the limiting guard there.
	return maxi(max_auto_refined_member_slots, 0)
