extends Node
## Phase 2 numerical reference tests. Exit code 0 = pass, 1 = failure.

var _failures: Array[String] = []


func _ready() -> void:
	_test_lake_at_rest()
	_test_dam_break_mass_and_positivity()
	_test_uniform_rain_volume()
	if _failures.is_empty():
		print("HYDRO_REFERENCE: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_REFERENCE: " + failure)
		get_tree().quit(1)


func _test_lake_at_rest() -> void:
	var solver := HydroReferenceSolver.new(28, 24, 5.0)
	solver.manning_n = 0.0
	var eta := 8.0
	for y in solver.height:
		for x in solver.width:
			var px := float(x) - float(solver.width - 1) * 0.5
			var py := float(y) - float(solver.height - 1) * 0.5
			var z := 1.1 + 0.008 * px * px + 0.011 * py * py \
				+ 0.12 * sin(float(x) * 0.41) * cos(float(y) * 0.33)
			solver.set_bed(x, y, z)
			solver.set_surface_elevation(x, y, eta)

	var initial_volume := solver.total_volume_m3()
	for _step in 160:
		solver.advance(0.08)

	var max_eta_error := 0.0
	for y in solver.height:
		for x in solver.width:
			max_eta_error = maxf(max_eta_error,
				absf(solver.surface_elevation(x, y) - eta))
	var volume_error := _relative_error(solver.total_volume_m3(), initial_volume)
	if solver.max_speed_mps() > 2.0e-4:
		_failures.append("lake-at-rest generated velocity: %.9f m/s" % solver.max_speed_mps())
	if max_eta_error > 2.0e-4:
		_failures.append("lake-at-rest surface drift: %.9f m" % max_eta_error)
	if volume_error > 2.0e-6:
		_failures.append("lake-at-rest volume error: %.9g" % volume_error)


func _test_dam_break_mass_and_positivity() -> void:
	var solver := HydroReferenceSolver.new(56, 12, 2.0)
	solver.manning_n = 0.0
	var initial_wet_end := 18
	for y in solver.height:
		for x in solver.width:
			solver.set_bed(x, y, 0.0)
			solver.set_state(x, y, 2.0 if x < initial_wet_end else 0.0)

	var initial_volume := solver.total_volume_m3()
	solver.advance(4.0)
	var final_volume := solver.total_volume_m3()
	var min_depth := INF
	var furthest_wet := -1
	for y in solver.height:
		for x in solver.width:
			var depth := solver.h[solver.index(x, y)]
			min_depth = minf(min_depth, depth)
			if depth > 1.0e-4:
				furthest_wet = maxi(furthest_wet, x)

	if min_depth < -1.0e-7:
		_failures.append("dam break produced negative depth: %.9g m" % min_depth)
	if furthest_wet <= initial_wet_end + 2:
		_failures.append("dam-break wet front did not advance")
	var volume_error := _relative_error(final_volume, initial_volume)
	if volume_error > 3.0e-4:
		_failures.append("dam-break volume error: %.9g" % volume_error)


func _test_uniform_rain_volume() -> void:
	var solver := HydroReferenceSolver.new(20, 16, 10.0)
	solver.manning_n = 0.0
	var rain := 12.0e-3 / 3600.0 # 12 mm/hour -> m/s
	for y in solver.height:
		for x in solver.width:
			solver.set_bed(x, y, 0.0)
			solver.set_rain_rate(x, y, rain)

	var duration := 600.0
	solver.advance(duration)
	var area := float(solver.width * solver.height) \
		* solver.cell_size_m * solver.cell_size_m
	var expected := rain * duration * area
	var volume_error := _relative_error(solver.total_volume_m3(), expected)
	if volume_error > 2.0e-5:
		_failures.append("uniform-rain volume error: %.9g" % volume_error)


func _relative_error(value: float, reference: float) -> float:
	return absf(value - reference) / maxf(absf(reference), 1.0e-12)
