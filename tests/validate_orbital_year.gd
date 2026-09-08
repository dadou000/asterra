extends Node
## End-to-end check that OrbitalMotionRuntime + the bootstrap Asterra->Helion
## orbit produce a real seasonal cycle. Launched via a .tscn so the Frames
## autoload resolves as it does at runtime.

const SESSION_SCRIPT := preload("res://scripts/world_authoring/world_authoring_session.gd")
const GEN_PROFILE := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const ORBITAL_MOTION := preload("res://scripts/world_authoring/orbital_motion_runtime.gd")
const GRAPHICS_QUALITY := preload("res://scripts/rendering/graphics_quality.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")

var _failed := false


func _ready() -> void:
	var session: RefCounted = SESSION_SCRIPT.new()
	session.bootstrap_from_generation_profile(GEN_PROFILE.new())
	var system: Resource = session.staged_system

	var asterra: Resource = system.call("find_body", "asterra") as Resource
	_assert(asterra != null and String(asterra.get(&"parent_body_id")) == "helion",
		"bootstrap must make Asterra a Helion child")
	var tilt_deg: float = float(asterra.get(&"axial_tilt_deg"))
	_assert(tilt_deg > 1.0, "Asterra needs a non-trivial axial tilt for a seasonal test (got %s)" % tilt_deg)

	var motion: Node = ORBITAL_MOTION.new()
	add_child(motion)
	motion.call("bind", session, null)
	motion.call("set_anchor", "asterra")

	Frames.playing = false
	Frames.system_time_s = 0.0
	motion.call("_process", 0.0)  # populate Frames.set_orbit_period_s / helion_*
	var year_s: float = float(Frames.year_seconds())
	_assert(year_s > 1.0e6, "orbital period did not reach Frames (got %s s)" % year_s)

	# Day 0 must reproduce the historical fixed sun direction so nothing looks
	# different until the clock is scrubbed.
	var want_dir: Vector3 = Vector3(1.0, 0.15, 0.3).normalized()
	var got_dir: Vector3 = Frames.helion_dir.normalized()
	_assert(got_dir.angle_to(want_dir) < deg_to_rad(1.0),
		"system_time_s == 0 must match the legacy sun direction within 1 deg (got %s want %s, %s deg off)" % [
			got_dir, want_dir, rad_to_deg(got_dir.angle_to(want_dir))])

	var au: float = 1.495978707e11
	var max_lat := -999.0
	var min_lat := 999.0
	var first_lat := 0.0
	var last_lat := 0.0
	var min_dist := 1.0e30
	var max_dist := 0.0
	var steps := 512
	for i: int in steps + 1:
		Frames.system_time_s = year_s * float(i) / float(steps)
		motion.call("_process", 0.0)
		var lat: float = rad_to_deg(float(Frames.sub_solar_latitude_rad()))
		max_lat = maxf(max_lat, lat)
		min_lat = minf(min_lat, lat)
		min_dist = minf(min_dist, float(Frames.helion_distance_m))
		max_dist = maxf(max_dist, float(Frames.helion_distance_m))
		if i == 0:
			first_lat = lat
		if i == steps:
			last_lat = lat

	_assert(max_lat > tilt_deg - 0.5, "sub-solar latitude must reach +axial_tilt over a year (got %s, tilt %s)" % [max_lat, tilt_deg])
	_assert(min_lat < -(tilt_deg - 0.5), "sub-solar latitude must reach -axial_tilt over a year (got %s)" % min_lat)
	_assert(absf(first_lat - last_lat) < 0.5, "sub-solar latitude must close the loop after one year (%s vs %s)" % [first_lat, last_lat])
	_assert(min_dist > 0.9 * au and max_dist < 1.1 * au, "Helion distance must stay near 1 AU (min %s max %s)" % [min_dist, max_dist])
	_assert(max_dist - min_dist > 1.0e9, "eccentricity 0.0167 must give a measurable perihelion/aphelion swing (got %s m)" % (max_dist - min_dist))

	# Brightness invariant: the surface (sun.light_energy) and the scattering
	# (solar_irradiance) scale by the exact same inverse-square factor, and it
	# actually moves with the orbit.
	var seen_below := false
	var seen_above := false
	for i: int in 96:
		Frames.system_time_s = year_s * float(i) / 96.0
		motion.call("_process", 0.0)
		var scale: float = float(Frames.solar_distance_scale())
		var irr: float = float(GRAPHICS_QUALITY.solar_irradiance())
		var want: float = float(GRAPHICS_QUALITY.SUN_LIGHT_ENERGY) * PI * scale
		_assert(absf(irr - want) < 1e-4 * maxf(want, 1.0),
			"solar_irradiance must equal SUN_LIGHT_ENERGY*PI*solar_distance_scale (%s vs %s)" % [irr, want])
		var light_energy: float = float(GRAPHICS_QUALITY.SUN_LIGHT_ENERGY) * scale
		_assert(absf(irr / maxf(light_energy, 1e-6) - PI) < 1e-4,
			"solar_irradiance / light_energy must stay exactly PI (got %s)" % (irr / maxf(light_energy, 1e-6)))
		if scale > 1.0001:
			seen_above = true
		if scale < 0.9999:
			seen_below = true
	_assert(seen_above and seen_below,
		"solar_distance_scale must swing above and below 1.0 over the eccentric year")

	motion.free()

	_test_nested_moon(session)

	if _failed:
		get_tree().quit(1)
		return
	print("ORBITAL_YEAR_OK")
	get_tree().quit(0)


## A moon orbiting Asterra orbiting Helion: OrbitalMotionRuntime must resolve the
## root star through two hops, keep Helion ~1 AU away (not the moon's ~4e8 m
## orbit), and give the moon its OWN axial tilt's seasonal sweep.
func _test_nested_moon(session: RefCounted) -> void:
	var moon: Resource = session.create_body("CI Moon", BODY_SCRIPT.BodyType.MOON, "asterra")
	_assert(moon != null, "moon creation failed")
	var moon_id: String = String(moon.get(&"body_id"))
	var moon_orbit: Resource = moon.get(&"orbit") as Resource
	_assert(moon_orbit != null and float(moon_orbit.get(&"semi_major_axis_m")) > 0.0,
		"moon must have a seeded orbit around Asterra")
	moon.set(&"axial_tilt_deg", 5.0)  # distinct from Asterra's tilt
	moon.set(&"sidereal_rotation_period_s", 86400.0)

	var motion: Node = ORBITAL_MOTION.new()
	add_child(motion)
	motion.call("bind", session, null)
	motion.call("set_anchor", moon_id)

	Frames.system_time_s = 0.0
	motion.call("_process", 0.0)
	var year_s: float = maxf(float(Frames.year_seconds()), 1.0)
	var au: float = 1.495978707e11

	var max_lat := -999.0
	var min_lat := 999.0
	var min_dist := 1.0e30
	var max_dist := 0.0
	for i: int in 400:
		Frames.system_time_s = year_s * float(i) / 400.0
		motion.call("_process", 0.0)
		var lat: float = rad_to_deg(float(Frames.sub_solar_latitude_rad()))
		max_lat = maxf(max_lat, lat)
		min_lat = minf(min_lat, lat)
		min_dist = minf(min_dist, float(Frames.helion_distance_m))
		max_dist = maxf(max_dist, float(Frames.helion_distance_m))

	_assert(min_dist > 0.85 * au and max_dist < 1.15 * au,
		"root star resolved through both hops must stay ~1 AU from the moon (min %s max %s)" % [min_dist, max_dist])
	_assert(max_lat > 4.0 and min_lat < -4.0,
		"moon sub-solar latitude must sweep to its OWN +/-5 deg tilt (got %s .. %s)" % [min_lat, max_lat])
	_assert(max_lat < 15.0 and min_lat > -15.0,
		"moon must NOT inherit Asterra's 21 deg tilt (got %s .. %s)" % [min_lat, max_lat])
	motion.free()


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("ORBITAL_YEAR_FAILED: %s" % message)
