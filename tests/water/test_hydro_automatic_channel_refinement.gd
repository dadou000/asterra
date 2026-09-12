extends Node
## CPU/headless policy gate for automatic channel refinement thresholds, ranking,
## and multi-tile sparse capacity reservation.


func _ready() -> void:
	_test_hysteresis()
	_test_priority()
	_test_collapse_exit()
	_test_member_capacity()
	print("HYDRO_AUTOMATIC_CHANNEL_REFINEMENT: PASS")
	get_tree().quit(0)


func _test_hysteresis() -> void:
	_require(not HydroAutomaticChannelRefinement.promotion_anomaly_active(
		1.50, 0.70, false, 2.0, 1.35, 0.85, 0.60),
		"sub-enter anomaly started promotion")
	_require(HydroAutomaticChannelRefinement.promotion_anomaly_active(
		2.05, 0.40, false, 2.0, 1.35, 0.85, 0.60),
		"discharge enter threshold did not trigger")
	_require(HydroAutomaticChannelRefinement.promotion_anomaly_active(
		1.10, 0.90, false, 2.0, 1.35, 0.85, 0.60),
		"bankfull enter threshold did not trigger")
	_require(HydroAutomaticChannelRefinement.promotion_anomaly_active(
		1.40, 0.50, true, 2.0, 1.35, 0.85, 0.60),
		"latched discharge exited too early")
	_require(HydroAutomaticChannelRefinement.promotion_anomaly_active(
		1.00, 0.65, true, 2.0, 1.35, 0.85, 0.60),
		"latched bank stage exited too early")
	_require(not HydroAutomaticChannelRefinement.promotion_anomaly_active(
		1.20, 0.50, true, 2.0, 1.35, 0.85, 0.60),
		"anomaly did not clear below both exit thresholds")


func _test_priority() -> void:
	var plain := HydroAutomaticChannelRefinement.priority_score(
		2.2, 0.7, 2, 1, 2.0, 0.85)
	var higher_order := HydroAutomaticChannelRefinement.priority_score(
		2.2, 0.7, 5, 1, 2.0, 0.85)
	var confluence := HydroAutomaticChannelRefinement.priority_score(
		2.2, 0.7, 2, 4, 2.0, 0.85)
	_require(higher_order > plain, "stream-order priority bonus missing")
	_require(confluence > plain, "confluence indegree priority bonus missing")
	var stronger := HydroAutomaticChannelRefinement.priority_score(
		3.0, 0.7, 2, 1, 2.0, 0.85)
	_require(stronger > plain, "stronger hydraulic anomaly did not rank higher")


func _test_collapse_exit() -> void:
	_require(HydroAutomaticChannelRefinement.collapse_hysteresis_clear(
		1.10, 0.45, 1.35, 0.60),
		"quiet low-flow reach did not clear collapse hysteresis")
	_require(not HydroAutomaticChannelRefinement.collapse_hysteresis_clear(
		1.50, 0.45, 1.35, 0.60),
		"high measured fine discharge allowed collapse")
	_require(not HydroAutomaticChannelRefinement.collapse_hysteresis_clear(
		1.10, 0.70, 1.35, 0.60),
		"high residual bank stage allowed collapse")


func _test_member_capacity() -> void:
	# A three-tile cluster fits exactly into the available scheduler slots.
	_require(HydroAutomaticChannelRefinementClusterCapacity.member_capacity_available(
		6, 3, 24, 3),
		"exact-fit three-member cluster was rejected")
	# The coarse river budget may have room, but two physical sparse slots cannot
	# satisfy an atomic three-member request.
	_require(not HydroAutomaticChannelRefinementClusterCapacity.member_capacity_available(
		6, 3, 24, 2),
		"cluster was allowed with insufficient sparse free slots")
	# Manual and automatic refined members share the same river-member budget.
	_require(not HydroAutomaticChannelRefinementClusterCapacity.member_capacity_available(
		22, 3, 24, 16),
		"cluster exceeded configured refined-member budget")
	_require(HydroAutomaticChannelRefinementClusterCapacity.member_capacity_available(
		21, 3, 24, 16),
		"cluster exactly fitting refined-member budget was rejected")
	_require(not HydroAutomaticChannelRefinementClusterCapacity.member_capacity_available(
		0, 0, 24, 24),
		"zero-member request was accepted")


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	push_error("HYDRO_AUTOMATIC_CHANNEL_REFINEMENT: " + message)
	get_tree().quit(1)
