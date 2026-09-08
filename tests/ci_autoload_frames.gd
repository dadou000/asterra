extends Node
## Parser/runtime stub for isolated CI. The production project uses frames.gd.

signal origin_shifted(delta_render: Vector3)

var origin: Vector3 = Vector3.ZERO
var planet_radius: float = 1000000.0
var axial_tilt_deg: float = 0.0
var day_seconds: float = 86400.0
var year_days: float = 402.0

const REFERENCE_HELION_DISTANCE_M := 149597870700.0

var helion_dir: Vector3 = Vector3(1, 0.15, 0.3).normalized()
var helion_radius_m: float = 696340000.0
var helion_distance_m: float = 149597870700.0

var system_time_s: float = 0.0
var time_scale: float = 1.0
var playing: bool = false
var anchor_body_id: String = "asterra"
var _rotation_phase0_deg: float = 0.0
var _orbit_period_s: float = 0.0


func to_render(p: Vec3D) -> Vector3:
	return Vector3(float(p.x - origin.x), float(p.y - origin.y), float(p.z - origin.z))


func to_world(p: Vector3) -> Vec3D:
	return Vec3D.new(origin.x + p.x, origin.y + p.y, origin.z + p.z)


func rebase(new_origin: Vec3D) -> void:
	var next_origin := Vector3(float(new_origin.x), float(new_origin.y), float(new_origin.z))
	var delta := origin - next_origin
	origin = next_origin
	origin_shifted.emit(delta)


func set_planet_radius(value: float) -> void:
	planet_radius = value


func helion_angular_radius_rad() -> float:
	return asin(clampf(helion_radius_m / maxf(helion_distance_m, 1.0), 0.0, 0.999999))


func helion_angular_diameter_deg() -> float:
	return rad_to_deg(helion_angular_radius_rad() * 2.0)


func sun_dir_at(_d: Vector3) -> Vector3:
	return helion_dir


func sun_dir_from_system(sunward_system: Vector3) -> Vector3:
	var s := sunward_system.normalized()
	if not s.is_normalized():
		return helion_dir
	var tilt := deg_to_rad(axial_tilt_deg)
	var spin := TAU * (system_time_s / maxf(day_seconds, 1e-6)) + deg_to_rad(_rotation_phase0_deg)
	var seasonal: Vector3 = Basis(Vector3(1.0, 0.0, 0.0), -tilt) * s
	return (Basis(Vector3(0.0, 1.0, 0.0), -spin) * seasonal).normalized()


func solar_distance_scale() -> float:
	var ratio := REFERENCE_HELION_DISTANCE_M / maxf(helion_distance_m, 1.0)
	return clampf(ratio * ratio, 0.1, 10.0)


func sub_solar_latitude_rad() -> float:
	return asin(clampf(helion_dir.y, -1.0, 1.0))


func set_orbit_period_s(value: float) -> void:
	_orbit_period_s = maxf(value, 0.0)
	if _orbit_period_s > 0.0 and day_seconds > 1e-6:
		year_days = _orbit_period_s / day_seconds


func year_seconds() -> float:
	if _orbit_period_s > 0.0:
		return _orbit_period_s
	return maxf(year_days * day_seconds, 1.0)


func day_of_year() -> float:
	return fposmod(system_time_s / maxf(day_seconds, 1e-6), maxf(year_days, 1e-6))
