class_name CelestialBodyDefinition
extends Resource
## Stable persistent definition for a star, planet or moon.

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
@export var parent_body_id: String = ""

@export var radius_m: float = 1000000.0
@export var mass_kg: float = 0.0
@export var gravitational_parameter_m3_s2: float = 0.0
@export var surface_gravity_m_s2: float = 9.81

@export var sidereal_rotation_period_s: float = 86400.0
@export var axial_tilt_deg: float = 0.0
@export var rotation_phase_at_epoch_deg: float = 0.0
@export var rotation_epoch_s: float = 0.0

@export var orbit: AuthoringOrbitDefinition
@export var rings: AuthoringRingSystemDefinition
@export var planet_profile: PlanetAuthoringProfile

func ensure_children() -> void:
	if body_id.is_empty():
		body_id = make_body_id(display_name)
	if orbit == null:
		orbit = AuthoringOrbitDefinition.new()
	if rings == null:
		rings = AuthoringRingSystemDefinition.new()
	if body_type != BodyType.STAR:
		if planet_profile == null:
			planet_profile = PlanetAuthoringProfile.new()
		planet_profile.ensure_children()

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
