class_name CelestialBodyDefinition
extends Resource
## Stable persistent definition for a star, planet, moon or other celestial body.

const ORBIT_SCRIPT := preload("res://scripts/world_authoring/model/orbit_definition.gd")
const RING_SCRIPT := preload("res://scripts/world_authoring/model/ring_system_definition.gd")
const PLANET_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/planet_authoring_profile.gd")
const STAR_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/star_authoring_profile.gd")

enum BodyType {
	STAR,
	PLANET,
	MOON,
	DWARF,
	OTHER,
}

@export var body_id: String = ""
@export var display_name: String = "Untitled Body"
@export_enum("Star", "Planet", "Moon", "Dwarf", "Other") var body_type: int = BodyType.PLANET
## Coarse classification that drives procedural terrain climate + the far-LOD tint
## (CelestialSystemGenerator). Free-form; known values: terran, hot_rock, ice,
## gas_giant, pulsar, moon_rock, moon_ice. Default keeps hand-authored bodies as
## Asterra-like.
@export var archetype: StringName = &"terran"
@export var parent_body_id: String = ""

@export var radius_m: float = 1000000.0
@export var mass_kg: float = 0.0
@export var gravitational_parameter_m3_s2: float = 0.0
@export var surface_gravity_m_s2: float = 9.81

## Gas-giant vertical structure (GasGiantModel, filled by CelestialSystemGenerator
## for `archetype == "gas_giant"`; 0 for every other body). `radius_m` stays the
## visible cloud-top / 1-bar level -- what orbits, far-LOD and camera framing use.
## `core_radius_m` is the rho ~= 1000 kg/m^3 solid surface the terrain bake,
## collision and altitude datum key off; the shell between the two is volumetric.
@export var core_radius_m: float = 0.0
@export var one_bar_radius_m: float = 0.0
@export var deadly_radius_m: float = 0.0

@export var sidereal_rotation_period_s: float = 86400.0
@export var axial_tilt_deg: float = 0.0
@export var rotation_phase_at_epoch_deg: float = 0.0
@export var rotation_epoch_s: float = 0.0

@export var orbit: Resource
@export var rings: Resource
@export var planet_profile: Resource
@export var star_profile: Resource

func ensure_children() -> void:
	if body_id.is_empty():
		body_id = make_body_id(display_name)
	if orbit == null:
		orbit = ORBIT_SCRIPT.new()
	if rings == null:
		rings = RING_SCRIPT.new()
	if body_type == BodyType.STAR:
		if star_profile == null:
			star_profile = STAR_PROFILE_SCRIPT.new()
		star_profile.call("ensure_valid")
	else:
		if planet_profile == null:
			planet_profile = PLANET_PROFILE_SCRIPT.new()
		planet_profile.call("ensure_children")

func is_gas_giant() -> bool:
	return archetype == &"gas_giant"

## The radius the terrain stack bakes / collides / measures altitude against. For a
## gas giant that is its solid rocky core, NOT the cloud tops; everything else
## bakes at its own `radius_m`.
func surface_reference_radius_m() -> float:
	if is_gas_giant() and core_radius_m > 0.0:
		return core_radius_m
	return radius_m

func hours_per_day() -> float:
	return sidereal_rotation_period_s / 3600.0

func set_hours_per_day(hours: float) -> void:
	sidereal_rotation_period_s = maxf(0.001, absf(hours) * 3600.0)

func surface_gravity_from_mass() -> float:
	if mass_kg <= 0.0 or radius_m <= 0.0:
		return surface_gravity_m_s2
	return 6.67430e-11 * mass_kg / (radius_m * radius_m)

static func make_body_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "body"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]
