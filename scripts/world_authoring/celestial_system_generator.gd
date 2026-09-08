class_name CelestialSystemGenerator
extends RefCounted
## Deterministic procedural celestial systems for the standalone game (M4 of the
## seamless-multi-planet design).
##
## `generate(system_seed)` returns a CelestialSystemDefinition: the root star
## Helion at the origin, the home planet "asterra" on a ~1 AU orbit (its day-0 sun
## direction calibrated to the historical fixed vector so nothing looks different
## on the first frame), a handful of sibling planets on a Titius-Bode-ish
## progression, and 0-2 moons each. `gen_config_for()` derives each solid body's
## PlanetBake input from a baseline GenConfig, seeded so every world is distinct
## and cache-stable. `populate_pool()` registers every non-home solid body with
## the `Bodies` autoload so the M3 approach state machine can travel to them.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SYSTEM_SCRIPT := preload("res://scripts/world_authoring/model/celestial_system_definition.gd")
const ORBIT_SCRIPT := preload("res://scripts/world_authoring/model/orbit_definition.gd")
const ORBIT_MATH := preload("res://scripts/world_authoring/model/orbit_math.gd")
const GAS_GIANT_MODEL := preload("res://scripts/gen/gas_giant_model.gd")

const HOME_BODY_ID := "asterra"
const STAR_BODY_ID := "helion"

## Sol-like star, matching WorldAuthoringSession.HELION_* (single source of truth
## is awkward across a const boundary; kept identical on purpose).
const STAR_RADIUS_M := 696_340_000.0
const STAR_MASS_KG := 1.988_416e30
const STAR_GM := 1.327_124_400_18e20
const STAR_SURFACE_GRAVITY := 274.0
const STAR_ROTATION_S := 25.05 * 86_400.0
const STAR_AXIAL_TILT_DEG := 7.25

const AU_M := 149_597_870_700.0
const HOME_ECCENTRICITY := 0.0167
const G := 6.674_30e-11
## Rocky-body bulk density band (kg/m^3) used to back out mass from radius.
const DENSITY_MIN := 3200.0
const DENSITY_MAX := 5800.0


## Peak fully-detailed bodies the pool renders at once (matches
## body_runtime_pool.MAX_HOT); the rest are far-LOD proxies regardless of count.
const MAX_HOT_BODIES := 2

## The fixed archetype spine every generated system carries, inner -> outer:
## two scorched inner rocks close to Helion, the terran home, a second temperate
## world, a frozen world, and a banded gas giant with a moon retinue. `body_scale`
## then adds RNG filler planets past the giant, and a distant pulsar companion.
const SPINE := [
	{"id": "cinder", "name": "Cinder", "arch": &"hot_rock", "au": 0.31, "r": 355_000.0, "tilt": 2.0, "moons": 0},
	{"id": "ember", "name": "Ember", "arch": &"hot_rock", "au": 0.54, "r": 470_000.0, "tilt": 6.0, "moons": 1},
	{"id": "asterra", "name": "Asterra", "arch": &"terran", "au": 1.00, "r": 1_000_000.0, "tilt": -1.0, "moons": 1, "home": true},
	{"id": "meridian", "name": "Meridian", "arch": &"terran", "au": 1.58, "r": 830_000.0, "tilt": 18.0, "moons": 2},
	{"id": "rime", "name": "Rime", "arch": &"ice", "au": 3.15, "r": 720_000.0, "tilt": 26.0, "moons": 1},
	{"id": "colossus", "name": "Colossus", "arch": &"gas_giant", "au": 5.7, "r": 68_000_000.0, "tilt": 3.2, "moons": 4},
]

## Test spine (GenConfig.minimal_system): exactly one body per archetype style, no
## RNG filler. Home keeps a rock moon; the ice world keeps an ice moon; the pulsar
## is still added. So every style -- hot_rock, terran, ice, gas_giant, moon_rock,
## moon_ice, pulsar -- is one hop away and quick to check.
const MINIMAL_SPINE := [
	{"id": "cinder", "name": "Cinder", "arch": &"hot_rock", "au": 0.5, "r": 420_000.0, "tilt": 4.0, "moons": 0},
	{"id": "asterra", "name": "Asterra", "arch": &"terran", "au": 1.00, "r": 1_000_000.0, "tilt": -1.0, "moons": 1, "home": true, "moon_styles": [&"moon_rock"]},
	{"id": "rime", "name": "Rime", "arch": &"ice", "au": 3.15, "r": 720_000.0, "tilt": 26.0, "moons": 1, "moon_styles": [&"moon_ice"]},
	{"id": "colossus", "name": "Colossus", "arch": &"gas_giant", "au": 5.7, "r": 68_000_000.0, "tilt": 3.2, "moons": 0},
]

## Build a full system from `system_seed`. Deterministic: two calls with the same
## seed produce identical bodies. The archetype spine is fixed; `system_seed` +
## `body_scale` vary radii jitter, orbit phases, moon counts and the number of
## outer filler planets. The runtime pool budget bounds what is ever resident, so
## a dense system costs the same as a sparse one.
static func generate(system_seed: int, home_axial_tilt_deg: float = 21.4,
		body_scale: float = 1.0, minimal: bool = false) -> CelestialSystemDefinition:
	var system: CelestialSystemDefinition = SYSTEM_SCRIPT.new()
	system.system_id = "gen-%d" % system_seed
	system.display_name = "System %d%s" % [system_seed & 0xffff, " (minimal)" if minimal else ""]

	system.add_body(_make_star())

	var rng := RandomNumberGenerator.new()
	rng.seed = system_seed

	# --- Archetype spine -------------------------------------------------
	var spine: Array = MINIMAL_SPINE if minimal else SPINE
	for spec_v: Variant in spine:
		var spec: Dictionary = spec_v
		var is_home: bool = bool(spec.get("home", false))
		var tilt: float = home_axial_tilt_deg if is_home else maxf(float(spec["tilt"]), 0.0)
		var planet := _make_planet(rng, String(spec["id"]), String(spec["name"]),
			spec["arch"], float(spec["au"]), float(spec["r"]), tilt, is_home)
		system.add_body(planet)
		_add_moons(system, rng, planet, int(spec["moons"]), body_scale,
			spec.get("moon_styles", []) as Array)

	# --- Outer filler planets past the gas giant (skipped in minimal) --
	if not minimal:
		var filler: int = clampi(int(round((2 + (absi(system_seed) % 5)) * body_scale)), 0, 40)
		var a: float = 8.5
		for i in filler:
			a *= rng.randf_range(1.35, 1.9)
			var arch: StringName = _weighted_filler_archetype(rng, a)
			var r: float = _radius_for_archetype(rng, arch)
			var planet := _make_planet(rng, "outer-%d" % (i + 1), "Outer %d" % (i + 1),
				arch, a, r, rng.randf_range(0.0, 34.0), false)
			system.add_body(planet)
			_add_moons(system, rng, planet, (1 if arch == &"gas_giant" else 0), body_scale)

	# --- Distant pulsar companion ------------------------------------
	system.add_body(_make_pulsar(rng, 120.0 + rng.randf_range(0.0, 90.0)))

	system.active_body_id = HOME_BODY_ID
	system.ensure_valid()
	return system


static func _make_planet(rng: RandomNumberGenerator, id: String, name: String,
		archetype: StringName, au: float, radius_m: float, tilt_deg: float,
		is_home: bool) -> CelestialBodyDefinition:
	var body: CelestialBodyDefinition = BODY_SCRIPT.new()
	body.body_id = id
	body.display_name = name
	body.body_type = BODY_SCRIPT.BodyType.PLANET
	body.archetype = archetype
	body.parent_body_id = STAR_BODY_ID
	body.ensure_children()

	body.radius_m = radius_m * (1.0 if is_home else rng.randf_range(0.9, 1.12))
	var density: float = _density_for_archetype(rng, archetype)
	body.mass_kg = density * (4.0 / 3.0) * PI * pow(body.radius_m, 3.0)
	body.gravitational_parameter_m3_s2 = G * body.mass_kg
	body.surface_gravity_m_s2 = G * body.mass_kg / (body.radius_m * body.radius_m)
	body.axial_tilt_deg = tilt_deg
	body.sidereal_rotation_period_s = rng.randf_range(28_000.0, 160_000.0)

	# Gas giants get a vertical envelope: radius_m stays the cloud tops, but the
	# terrain bake / collision / altitude datum key off the rho ~= 1000 kg/m^3
	# solid core (see GasGiantModel, GG0 of the gas-giant milestone).
	if archetype == &"gas_giant":
		var model: GasGiantModel = GAS_GIANT_MODEL.from_body(
			body.radius_m, body.gravitational_parameter_m3_s2, _hash_id(id) ^ 0x6a5f)
		body.core_radius_m = model.core_radius_m
		body.one_bar_radius_m = model.one_bar_radius_m
		body.deadly_radius_m = model.deadly_radius_m
		# Gravity at the solid core surface. Inside a roughly uniform-density body
		# only the enclosed mass pulls, so g falls ~linearly toward the centre:
		# g_core ~= g_cloud_top * (core_radius / cloud_top_radius). Keeps a big
		# gas giant's core a walkable ~1 g rather than a crushing GM/core^2.
		var g_cloud_top: float = body.gravitational_parameter_m3_s2 \
			/ (body.radius_m * body.radius_m)
		body.surface_gravity_m_s2 = g_cloud_top * (body.core_radius_m / body.radius_m)

	var orbit: AuthoringOrbitDefinition = body.orbit
	orbit.semi_major_axis_m = au * AU_M
	orbit.eccentricity = HOME_ECCENTRICITY if is_home else rng.randf_range(0.004, 0.11)
	orbit.inclination_deg = 0.0 if is_home else rng.randf_range(0.0, 4.0)
	orbit.longitude_ascending_node_deg = 0.0 if is_home else rng.randf_range(0.0, 360.0)
	orbit.mean_anomaly_at_epoch_deg = 0.0 if is_home else rng.randf_range(0.0, 360.0)
	if is_home:
		_calibrate_home_day0(body)
	return body


static func _add_moons(system: CelestialSystemDefinition, rng: RandomNumberGenerator,
		parent: CelestialBodyDefinition, base_count: int, body_scale: float,
		moon_styles: Array = []) -> void:
	var count: int = clampi(base_count + int(round(float(rng.randi() % 3) * (body_scale - 1.0))), 0, 6)
	var icy_parent: bool = parent.archetype == &"ice"
	for m in count:
		var moon: CelestialBodyDefinition = BODY_SCRIPT.new()
		moon.body_id = "%s-moon-%d" % [parent.body_id, m + 1]
		moon.display_name = "%s %s" % [parent.display_name, char(0x41 + m)]
		moon.body_type = BODY_SCRIPT.BodyType.MOON
		if m < moon_styles.size():
			moon.archetype = moon_styles[m]
		else:
			moon.archetype = &"moon_ice" if (icy_parent or rng.randf() < 0.4) else &"moon_rock"
		moon.parent_body_id = parent.body_id
		moon.ensure_children()
		var moon_r: float = rng.randf_range(80_000.0, 430_000.0)
		moon.radius_m = moon_r
		var moon_density: float = rng.randf_range(1800.0, 3400.0)
		moon.mass_kg = moon_density * (4.0 / 3.0) * PI * pow(moon_r, 3.0)
		moon.gravitational_parameter_m3_s2 = G * moon.mass_kg
		moon.surface_gravity_m_s2 = G * moon.mass_kg / (moon_r * moon_r)
		moon.axial_tilt_deg = rng.randf_range(0.0, 12.0)
		moon.sidereal_rotation_period_s = rng.randf_range(60_000.0, 500_000.0)
		var moon_orbit: AuthoringOrbitDefinition = moon.orbit
		moon_orbit.semi_major_axis_m = parent.radius_m * rng.randf_range(3.5, 12.0)
		moon_orbit.eccentricity = rng.randf_range(0.0, 0.05)
		moon_orbit.inclination_deg = rng.randf_range(0.0, 9.0)
		moon_orbit.mean_anomaly_at_epoch_deg = rng.randf_range(0.0, 360.0)
		system.add_body(moon)


## A fast-spinning blue-white neutron-star companion far out from Helion. Rendered
## far-LOD only (it is a STAR, so populate_pool never registers it as a target).
static func _make_pulsar(rng: RandomNumberGenerator, au: float) -> CelestialBodyDefinition:
	var p: CelestialBodyDefinition = BODY_SCRIPT.new()
	p.body_id = "lighthouse"
	p.display_name = "Lighthouse"
	p.body_type = BODY_SCRIPT.BodyType.STAR
	p.archetype = &"pulsar"
	p.parent_body_id = STAR_BODY_ID
	p.radius_m = 16_000.0
	p.mass_kg = 2.8e30
	p.gravitational_parameter_m3_s2 = G * p.mass_kg
	p.surface_gravity_m_s2 = G * p.mass_kg / (p.radius_m * p.radius_m)
	p.sidereal_rotation_period_s = 1.4        # milliseconds in reality; cosmetic here
	p.axial_tilt_deg = rng.randf_range(0.0, 20.0)
	p.ensure_children()
	var star := p.star_profile
	if star != null:
		star.set(&"photosphere_color", Color(0.74, 0.82, 1.0))
		star.set(&"photosphere_intensity", 6.0)
		star.set(&"corona_color", Color(0.62, 0.78, 1.0))
		star.set(&"corona_intensity", 3.5)
		star.set(&"corona_extent_radii", 4.0)
		star.set(&"light_color", Color(0.82, 0.88, 1.0))
	var orbit: AuthoringOrbitDefinition = p.orbit
	orbit.semi_major_axis_m = au * AU_M
	orbit.eccentricity = rng.randf_range(0.1, 0.35)
	orbit.inclination_deg = rng.randf_range(4.0, 22.0)
	orbit.mean_anomaly_at_epoch_deg = rng.randf_range(0.0, 360.0)
	return p


static func _weighted_filler_archetype(rng: RandomNumberGenerator, au: float) -> StringName:
	# Colder further out; a gas giant needs room.
	var roll := rng.randf()
	if au > 20.0 and roll < 0.28:
		return &"gas_giant"
	if roll < 0.55:
		return &"ice"
	if roll < 0.75:
		return &"terran"
	if roll < 0.9:
		return &"hot_rock"
	return &"terran"


static func _radius_for_archetype(rng: RandomNumberGenerator, arch: StringName) -> float:
	match arch:
		&"gas_giant":
			return rng.randf_range(38_000_000.0, 92_000_000.0)
		&"ice":
			return rng.randf_range(380_000.0, 1_150_000.0)
		&"hot_rock":
			return rng.randf_range(280_000.0, 620_000.0)
		_:
			return rng.randf_range(520_000.0, 1_320_000.0)


static func _density_for_archetype(rng: RandomNumberGenerator, arch: StringName) -> float:
	match arch:
		&"gas_giant":
			return rng.randf_range(700.0, 1_600.0)
		&"ice":
			return rng.randf_range(1_600.0, 2_800.0)
		&"hot_rock":
			return rng.randf_range(4_200.0, 6_000.0)
		_:
			return rng.randf_range(DENSITY_MIN, DENSITY_MAX)


static func home_body_id(_system: CelestialSystemDefinition) -> String:
	return HOME_BODY_ID


## Rebuild the GasGiantModel for `body` with the same seed formula _make_planet
## used, so renderers / physics can query its envelope without stashing the model.
## Returns null for a non-gas body.
static func gas_giant_model_for(body: Resource) -> GasGiantModel:
	if body == null or StringName(body.get(&"archetype")) != &"gas_giant":
		return null
	return GAS_GIANT_MODEL.from_body(
		float(body.get(&"radius_m")),
		float(body.get(&"gravitational_parameter_m3_s2")),
		_hash_id(String(body.get(&"body_id"))) ^ 0x6a5f)


const GENERATION_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")

## Stamp every non-star body's `planet_profile.terrain.generation_profile` (and
## atmosphere height) with its archetype-tuned config, so Planet Studio's per-body
## Apply / bake path renders that world's real terrain. `baseline` is the game's
## world config (a GenConfig or a GenerationAuthoringProfile -- any object with
## the matching script vars). The home body keeps the baseline unchanged.
static func apply_archetype_profiles(system: CelestialSystemDefinition,
		baseline: Resource) -> void:
	if system == null:
		return
	for body_value: Variant in system.bodies:
		var body: Resource = body_value as Resource
		if body == null or int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
			continue
		body.call("ensure_children")
		var profile: Resource = body.get(&"planet_profile") as Resource
		if profile == null:
			continue
		profile.call("ensure_children")
		var terrain: Resource = profile.get(&"terrain") as Resource
		if terrain == null:
			continue
		var body_id: String = String(body.get(&"body_id"))
		var cfg: Resource = gen_config_for(system, body_id, baseline)
		# gen_config_for returns a duplicate of `baseline`; if that was already a
		# GenerationAuthoringProfile it can be used as-is, otherwise import.
		var gap: Resource = cfg
		if cfg.get_script() != GENERATION_PROFILE_SCRIPT:
			gap = GENERATION_PROFILE_SCRIPT.new()
			gap.call("import_from_resource", cfg)
		gap.set(&"system_seed", 0)
		# Mirror the gas-giant envelope radii so a bake rebuilt from this profile
		# alone (Planet Studio persistence) stays at parity with the game.
		gap.set(&"core_radius_m", float(body.get(&"core_radius_m")))
		gap.set(&"one_bar_radius_m", float(body.get(&"one_bar_radius_m")))
		gap.set(&"deadly_radius_m", float(body.get(&"deadly_radius_m")))
		gap.set(&"cloud_top_radius_m",
			float(body.get(&"radius_m")) if float(body.get(&"core_radius_m")) > 0.0 else 0.0)
		terrain.set(&"generation_profile", gap)
		var atmosphere: Resource = profile.get(&"atmosphere") as Resource
		if atmosphere != null:
			atmosphere.set(&"atmosphere_height_m", float(gap.get(&"atmosphere_height")))
			atmosphere.set(&"enabled", float(gap.get(&"atmosphere_height")) > 1.0)


## Per-body PlanetBake input, derived from `baseline` (the game's world.tres cfg).
## The home body returns the baseline unchanged (so its existing bake is reused);
## every other body gets a distinct `world_seed` and perturbed climate/terrain so
## it bakes a different-looking world, cache-stable across runs.
static func gen_config_for(system: CelestialSystemDefinition, body_id: String,
		baseline: Resource) -> Resource:
	var cfg: Resource = baseline.duplicate(true)
	cfg.set(&"system_seed", 0)   # a per-body cfg must not re-trigger generation

	if body_id == HOME_BODY_ID:
		return cfg

	var body: Resource = system.find_body(body_id)
	if body == null:
		return cfg

	var seed: int = int(baseline.get(&"system_seed")) ^ _hash_id(body_id)
	var rng := RandomNumberGenerator.new()
	rng.seed = seed

	cfg.set(&"world_seed", seed)
	# A gas giant bakes its solid core (rho ~= 1000 kg/m^3), not the cloud tops;
	# every other body bakes at its own radius.
	var bake_radius: float = float(body.get(&"radius_m"))
	if StringName(body.get(&"archetype")) == &"gas_giant" and float(body.get(&"core_radius_m")) > 0.0:
		bake_radius = float(body.get(&"core_radius_m"))
	cfg.set(&"planet_radius", maxf(bake_radius, 50_000.0))
	cfg.set(&"axial_tilt_deg", float(body.get(&"axial_tilt_deg")))
	_apply_archetype_climate(cfg, StringName(body.get(&"archetype")), rng)

	# Smaller bodies get a coarser macro grid so their bakes stay cheap; the gas
	# giant is huge and never walked closely, so it gets the coarsest.
	var radius: float = float(body.get(&"radius_m"))
	var base_face: int = int(baseline.get(&"face_res"))
	if StringName(body.get(&"archetype")) == &"gas_giant":
		cfg.set(&"face_res", 64)
	elif radius < 500_000.0:
		cfg.set(&"face_res", mini(96, base_face) if base_face < 96 else 96)
	else:
		cfg.set(&"face_res", base_face)
	return cfg


## Push archetype-specific climate + relief knobs onto a per-body GenConfig so a
## scorched inner rock, a frozen world and a gas giant each bake a distinctly
## different planet from the same pipeline.
static func _apply_archetype_climate(cfg: Resource, arch: StringName,
		rng: RandomNumberGenerator) -> void:
	match arch:
		&"hot_rock", &"moon_rock":
			cfg.set(&"ocean_fraction", clampf(rng.randf_range(0.0, 0.04), 0.0, 0.1))
			cfg.set(&"greenhouse_offset", rng.randf_range(18.0, 46.0))
			cfg.set(&"atmosphere_height", rng.randf_range(0.0, 14_000.0))
			cfg.set(&"continent_scale", rng.randf_range(0.6, 1.1))
			cfg.set(&"max_uplift", rng.randf_range(4_200.0, 8_600.0))
			cfg.set(&"volcanism_strength", rng.randf_range(1.4, 2.6))
			cfg.set(&"base_precip", rng.randf_range(20.0, 180.0))
			cfg.set(&"albedo_land", rng.randf_range(0.10, 0.20))
		&"ice", &"moon_ice":
			cfg.set(&"ocean_fraction", clampf(rng.randf_range(0.68, 0.9), 0.0, 0.95))
			cfg.set(&"greenhouse_offset", rng.randf_range(-34.0, -14.0))
			cfg.set(&"atmosphere_height", rng.randf_range(8_000.0, 42_000.0))
			cfg.set(&"continent_scale", rng.randf_range(1.1, 2.0))
			cfg.set(&"max_uplift", rng.randf_range(2_000.0, 4_200.0))
			cfg.set(&"ice_onset_temp", rng.randf_range(1.0, 4.0))
			cfg.set(&"ice_full_temp", rng.randf_range(-10.0, -4.0))
			cfg.set(&"albedo_ice", rng.randf_range(0.62, 0.74))
			cfg.set(&"base_precip", rng.randf_range(120.0, 420.0))
		&"gas_giant":
			# The BAKE is the solid rocky core under the envelope: no sea, dark
			# rock, real relief, hot and volcanically active. The gas shell itself
			# is the volumetric layer (GG2/GG3); atmosphere_height still drives it.
			cfg.set(&"ocean_fraction", clampf(rng.randf_range(0.0, 0.05), 0.0, 0.1))
			cfg.set(&"greenhouse_offset", rng.randf_range(60.0, 150.0))
			cfg.set(&"atmosphere_height", rng.randf_range(140_000.0, 280_000.0))
			cfg.set(&"continent_scale", rng.randf_range(1.2, 2.6))
			cfg.set(&"max_uplift", rng.randf_range(3_000.0, 7_800.0))
			cfg.set(&"volcanism_strength", rng.randf_range(1.8, 3.4))
			cfg.set(&"base_precip", rng.randf_range(0.0, 40.0))
			cfg.set(&"albedo_land", rng.randf_range(0.05, 0.12))
		_:  # terran / default
			cfg.set(&"ocean_fraction", clampf(rng.randf_range(0.35, 0.78), 0.0, 0.95))
			cfg.set(&"greenhouse_offset", rng.randf_range(-10.0, 14.0))
			cfg.set(&"atmosphere_height", rng.randf_range(45_000.0, 82_000.0))
			cfg.set(&"continent_scale", rng.randf_range(1.0, 1.9))
			cfg.set(&"max_uplift", rng.randf_range(3_600.0, 7_000.0))


## Register every non-home solid body (planet + moon) with the `Bodies` autoload
## so the M3 approach state machine can swap to it, and record each body's system
## position in `Frames`. `bodies_autoload` / `frames_autoload` are passed in so
## this stays callable from a headless test without the game's `main`.
static func populate_pool(system: CelestialSystemDefinition, baseline: Resource,
		bodies_autoload: Node, frames_autoload: Node, sim_time_s: float = 0.0) -> int:
	if system == null or bodies_autoload == null:
		return 0
	var registered: int = 0
	for body_value: Variant in system.bodies:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		var body_id: String = String(body.get(&"body_id"))
		var body_type: int = int(body.get(&"body_type"))
		if body_type == BODY_SCRIPT.BodyType.STAR or body_id == HOME_BODY_ID:
			continue
		var center: Vec3D = ORBIT_MATH.system_position(system, body_id, sim_time_s)
		var cfg: Resource = gen_config_for(system, body_id, baseline)
		# radius_m is the reference (cloud tops for a gas giant); surface radius is
		# what the resident terrain bake / altitude datum use (the core for a gas
		# giant, == radius_m otherwise).
		var surface_r: float = float(body.get(&"radius_m"))
		if StringName(body.get(&"archetype")) == &"gas_giant" and float(body.get(&"core_radius_m")) > 0.0:
			surface_r = float(body.get(&"core_radius_m"))
		bodies_autoload.call(&"register_body", StringName(body_id), cfg, center,
			float(body.get(&"radius_m")), float(body.get(&"surface_gravity_m_s2")), surface_r)
		registered += 1
	# The home body's own system centre (it orbits the star ~1 AU out).
	if frames_autoload != null and frames_autoload.has_method(&"set_body_frame"):
		var home_center: Vec3D = ORBIT_MATH.system_position(system, HOME_BODY_ID, sim_time_s)
		frames_autoload.call(&"set_body_frame", StringName(HOME_BODY_ID), home_center,
			float(baseline.get(&"planet_radius")))
	return registered


static func _make_star() -> CelestialBodyDefinition:
	var star: CelestialBodyDefinition = BODY_SCRIPT.new()
	star.body_id = STAR_BODY_ID
	star.display_name = "Helion"
	star.body_type = BODY_SCRIPT.BodyType.STAR
	star.parent_body_id = ""
	star.radius_m = STAR_RADIUS_M
	star.mass_kg = STAR_MASS_KG
	star.gravitational_parameter_m3_s2 = STAR_GM
	star.surface_gravity_m_s2 = STAR_SURFACE_GRAVITY
	star.sidereal_rotation_period_s = STAR_ROTATION_S
	star.axial_tilt_deg = STAR_AXIAL_TILT_DEG
	star.ensure_children()
	return star


## Match WorldAuthoringSession._seed_primary_orbit: pick the orbit phase +
## rotation phase so `Frames.sun_dir_from_system` at system_time_s == 0 reproduces
## the legacy fixed sun direction. Keeps the game's day-0 look unchanged.
static func _calibrate_home_day0(planet: CelestialBodyDefinition) -> void:
	var align: Dictionary = ORBIT_MATH.epoch_alignment_for(
		ORBIT_MATH.LEGACY_SUNWARD, planet.axial_tilt_deg)
	planet.orbit.argument_periapsis_deg = float(align["argp_deg"])
	planet.rotation_phase_at_epoch_deg = float(align["rotation_phase0_deg"])


static func _hash_id(text: String) -> int:
	var h: int = 1469598103934665603
	for i in text.length():
		h = (h ^ text.unicode_at(i)) * 1099511628211
		h &= 0x7fff_ffff_ffff_ffff
	return h
