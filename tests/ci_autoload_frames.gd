extends Node
## Parser/runtime stub for isolated CI. The production project uses frames.gd.

signal origin_shifted(delta_render: Vector3)

var origin: Vector3 = Vector3.ZERO
var planet_radius: float = 1000000.0
var axial_tilt_deg: float = 0.0
var day_seconds: float = 86400.0


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
