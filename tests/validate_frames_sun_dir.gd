extends SceneTree
## Verifies Frames.sun_dir_from_system() in isolation, one rotation at a time,
## BEFORE any renderer consumes it (see the plan's "highest risk" note).
## Run: godot --headless --path . --script res://tests/validate_frames_sun_dir.gd

const FRAMES_SCRIPT := preload("res://scripts/core/frames.gd")

var _failed := false


func _init() -> void:
	_test_obliquity_isolated()
	_test_diurnal_isolated()
	_test_matches_phase20_time_of_day()
	_test_year_sweep_subsolar_latitude()
	if _failed:
		quit(1)
		return
	print("FRAMES_SUN_DIR_OK")
	quit(0)


func _frames(tilt_deg: float, day_s: float) -> Node:
	var f: Node = FRAMES_SCRIPT.new()
	f.axial_tilt_deg = tilt_deg
	f.day_seconds = day_s
	f.system_time_s = 0.0
	f._rotation_phase0_deg = 0.0
	return f


## spin == 0: sub-solar latitude must trace asin(sin(tilt)*sin(theta)) as the
## planet->star vector sweeps the ecliptic (XZ) circle: 0 at the system-X
## crossings, +/- tilt at the system-Z crossings.
func _test_obliquity_isolated() -> void:
	var tilt_deg := 23.4
	var f := _frames(tilt_deg, 86400.0)  # system_time_s 0 -> spin 0
	var max_lat := -999.0
	var min_lat := 999.0
	for i: int in 360:
		var theta := TAU * float(i) / 360.0
		var s := Vector3(cos(theta), 0.0, sin(theta))
		var d: Vector3 = f.sun_dir_from_system(s)
		var lat := rad_to_deg(asin(clampf(d.y, -1.0, 1.0)))
		max_lat = maxf(max_lat, lat)
		min_lat = minf(min_lat, lat)
		# zero-crossings on the system-X axis
		if is_equal_approx(theta, 0.0) or absf(theta - PI) < 1e-6:
			_assert(absf(lat) < 1e-4, "equinox: sub-solar latitude must be 0 at theta=%s, got %s" % [theta, lat])
	_assert(absf(max_lat - tilt_deg) < 1e-3, "solstice max sub-solar latitude must be +axial_tilt (got %s)" % max_lat)
	_assert(absf(min_lat + tilt_deg) < 1e-3, "solstice min sub-solar latitude must be -axial_tilt (got %s)" % min_lat)
	f.free()


## tilt == 0, planet fixed: sweeping system_time_s over one day must keep the sun
## on the equator (y ~ 0) and advance sub-solar longitude by exactly one turn,
## monotonically increasing.
func _test_diurnal_isolated() -> void:
	var day_s := 90000.0
	var f := _frames(0.0, day_s)
	var s := Vector3(1.0, 0.0, 0.0)
	var prev_lon := -1000.0
	var unwrapped := 0.0
	var samples := 720
	for i: int in samples + 1:
		f.system_time_s = day_s * float(i) / float(samples)
		var d: Vector3 = f.sun_dir_from_system(s)
		_assert(absf(d.y) < 1e-5, "diurnal: tilt 0 must keep the sun on the equator, y=%s" % d.y)
		_assert(absf(d.length() - 1.0) < 1e-5, "helion_dir must stay unit length, len=%s" % d.length())
		var lon := atan2(d.z, d.x)
		if prev_lon > -100.0:
			var step := lon - prev_lon
			if step < -PI:
				step += TAU
			_assert(step > -1e-6, "sub-solar longitude must be non-decreasing (step=%s)" % step)
			unwrapped += step
		prev_lon = lon
	_assert(absf(unwrapped - TAU) < 1e-3, "one day must advance sub-solar longitude by exactly 2*pi (got %s)" % unwrapped)
	f.free()


## sun_dir_from_system must reproduce the shipped phase20 time-of-day slider
## formula: dir = (cos hour, sin decl, sin hour) with hour = TAU*t/day, decl = 0.
func _test_matches_phase20_time_of_day() -> void:
	var day_s := 90000.0
	var f := _frames(0.0, day_s)
	var s := Vector3(1.0, 0.0, 0.0)
	for i: int in 48:
		var t := day_s * float(i) / 48.0
		f.system_time_s = t
		var got: Vector3 = f.sun_dir_from_system(s)
		var hour := TAU * (t / day_s)
		var want := Vector3(cos(hour), 0.0, sin(hour))
		_assert(got.distance_to(want) < 1e-5,
			"phase20 parity failed at t=%s: got %s want %s" % [t, got, want])
	f.free()


## Full combined sweep: a circular orbit + real tilt over exactly one year. The
## sub-solar latitude must complete one +tilt/-tilt oscillation (both extremes
## reached, and the endpoints line up).
func _test_year_sweep_subsolar_latitude() -> void:
	var tilt_deg := 23.4
	var day_s := 86400.0
	var year_days := 365.25
	var f := _frames(tilt_deg, day_s)
	f.set_orbit_period_s(year_days * day_s)
	var year_s: float = f.year_seconds()
	_assert(absf(year_s - year_days * day_s) < 1.0, "year_seconds must follow the supplied orbital period")

	var max_lat := -999.0
	var min_lat := 999.0
	var first_lat := 0.0
	var last_lat := 0.0
	var steps := 1460  # 4 samples/day
	for i: int in steps + 1:
		var frac := float(i) / float(steps)
		f.system_time_s = year_s * frac
		# circular orbit: planet->star sweeps the ecliptic circle once per year
		var phi := TAU * frac
		var s := Vector3(-cos(phi), 0.0, -sin(phi))
		var d: Vector3 = f.sun_dir_from_system(s)
		var lat := rad_to_deg(f.sub_solar_latitude_rad()) if false else rad_to_deg(asin(clampf(d.y, -1.0, 1.0)))
		max_lat = maxf(max_lat, lat)
		min_lat = minf(min_lat, lat)
		if i == 0:
			first_lat = lat
		if i == steps:
			last_lat = lat
	_assert(max_lat > tilt_deg - 0.5, "year sweep must reach +axial_tilt sub-solar latitude (got %s)" % max_lat)
	_assert(min_lat < -(tilt_deg - 0.5), "year sweep must reach -axial_tilt sub-solar latitude (got %s)" % min_lat)
	_assert(absf(first_lat - last_lat) < 0.5, "sub-solar latitude must return to its start after one year (%s vs %s)" % [first_lat, last_lat])
	f.free()


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("FRAMES_SUN_DIR_FAILED: %s" % message)
