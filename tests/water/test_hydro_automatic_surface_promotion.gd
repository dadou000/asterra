extends Node

const Policy := preload("res://scripts/water/hydro_automatic_surface_promotion.gd")

var _failures: Array[String] = []


func _ready() -> void:
	_test_hysteresis()
	_test_surface_only_parcel_cap()
	if _failures.is_empty():
		print("HydroAutomaticSurfacePromotionTests: PASS")
		get_tree().quit(0)
		return
	for failure in _failures:
		push_error("HydroAutomaticSurfacePromotionTests: " + failure)
	get_tree().quit(1)


func _test_hysteresis() -> void:
	var enter := 0.05
	var exit := 0.025
	_expect(not Policy.surface_depth_eligible(0.049, false, enter, exit),
		"unlatched cell entered below the high threshold")
	_expect(Policy.surface_depth_eligible(0.05, false, enter, exit),
		"unlatched cell did not enter at the high threshold")
	_expect(Policy.surface_depth_eligible(0.03, true, enter, exit),
		"latched cell dropped out inside hysteresis band")
	_expect(Policy.surface_depth_eligible(0.025, true, enter, exit),
		"latched cell dropped out at the low threshold")
	_expect(not Policy.surface_depth_eligible(0.0249, true, enter, exit),
		"latched cell remained active below the low threshold")
	# Misordered configuration clamps exit to enter instead of creating an inverted
	# hysteresis range.
	_expect(not Policy.surface_depth_eligible(0.049, true, 0.05, 0.08),
		"misordered thresholds created an invalid hysteresis band")


func _test_surface_only_parcel_cap() -> void:
	_expect_close(Policy.surface_only_parcel_m3(20.0, 12.0), 12.0, 1.0e-9,
		"parcel exceeded available surface storage")
	_expect_close(Policy.surface_only_parcel_m3(8.0, 12.0), 8.0, 1.0e-9,
		"parcel changed a valid surface-only request")
	_expect_close(Policy.surface_only_parcel_m3(20.0, 0.0), 0.0, 1.0e-9,
		"zero surface storage borrowed non-surface water")
	_expect_close(Policy.surface_only_parcel_m3(0.00005, 1.0, 0.0001), 0.0, 1.0e-9,
		"sub-minimum parcel was not rejected")
	_expect_close(Policy.surface_only_parcel_m3(-5.0, 12.0), 0.0, 1.0e-9,
		"negative suggestion produced a parcel")


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _expect_close(actual: float, expected: float, tolerance: float,
		message: String) -> void:
	if absf(actual - expected) > tolerance:
		_failures.append("%s (actual=%g expected=%g)" % [message, actual, expected])
