extends Node
## Headless metric-addressing gate. Production sparse hydrology must never treat an
## arbitrary solver dx and a quadtree address level as the same physical footprint.

const RADIUS := 1_000_000.0
const TILE_RES := 32
const TARGET_DX := 4.0
const TOL := 1.0e-9

var _failed := false


func _ready() -> void:
	_test_exact_roundtrip()
	_test_nearest_level_contract()
	_test_level_monotonicity()
	if _failed:
		get_tree().quit(1)
		return
	print("HYDRO_METRIC_GRID: PASS")
	get_tree().quit(0)


func _test_exact_roundtrip() -> void:
	for level in [0, 1, 7, 12, 18, HydroTileKey.MAX_LEVEL]:
		var dx := HydroMetricGrid.compatible_cell_size_m(RADIUS, TILE_RES, level)
		var recovered := HydroMetricGrid.level_for_target_cell_size(RADIUS, TILE_RES, dx)
		_require(recovered == level,
			"exact compatible dx did not recover level %d; got %d" % [level, recovered])
		var width := HydroMetricGrid.tile_width_m(RADIUS, level)
		_require(absf(width - dx * float(TILE_RES)) <= maxf(width, 1.0) * TOL,
			"tile width/cell size mismatch at level %d" % level)


func _test_nearest_level_contract() -> void:
	var c := HydroMetricGrid.contract_for_target(RADIUS, TILE_RES, TARGET_DX)
	var level := int(c["level"])
	var exact_dx := float(c["compatible_cell_size_m"])
	var lower_error := INF
	var upper_error := INF
	if level > 0:
		lower_error = absf(HydroMetricGrid.compatible_cell_size_m(
			RADIUS, TILE_RES, level - 1) - TARGET_DX)
	if level < HydroTileKey.MAX_LEVEL:
		upper_error = absf(HydroMetricGrid.compatible_cell_size_m(
			RADIUS, TILE_RES, level + 1) - TARGET_DX)
	var chosen_error := absf(exact_dx - TARGET_DX)
	_require(chosen_error <= lower_error + 1.0e-9 and chosen_error <= upper_error + 1.0e-9,
		"contract did not choose nearest quadtree level: %s" % str(c))
	_require(absf(float(c["cell_size_ratio"]) - exact_dx / TARGET_DX) <= TOL,
		"cell-size ratio inconsistent")


func _test_level_monotonicity() -> void:
	var prior := HydroMetricGrid.compatible_cell_size_m(RADIUS, TILE_RES, 0)
	for level in range(1, HydroTileKey.MAX_LEVEL + 1):
		var current := HydroMetricGrid.compatible_cell_size_m(RADIUS, TILE_RES, level)
		_require(absf(prior * 0.5 - current) <= maxf(current, 1.0) * TOL,
			"level %d is not an exact 2x metric refinement" % level)
		prior = current


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("HYDRO_METRIC_GRID: " + message)
