class_name Vec3D
extends RefCounted
## Double-precision 3D vector.
##
## Godot's built-in Vector3 stores 32-bit floats in standard engine builds, which
## breaks down at planetary scale (a 600 km planet already loses centimetre
## precision at the surface). GDScript's `float` is a 64-bit double, so canonical
## world state is stored with this type and only *rendered* through the local
## float32 frame produced by Frames.to_render().

var x: float
var y: float
var z: float

func _init(px: float = 0.0, py: float = 0.0, pz: float = 0.0) -> void:
	x = px
	y = py
	z = pz

static func from_v3(v: Vector3) -> Vec3D:
	return Vec3D.new(v.x, v.y, v.z)

func to_v3() -> Vector3:
	return Vector3(x, y, z)

func dup() -> Vec3D:
	return Vec3D.new(x, y, z)

func add(o: Vec3D) -> Vec3D:
	return Vec3D.new(x + o.x, y + o.y, z + o.z)

func sub(o: Vec3D) -> Vec3D:
	return Vec3D.new(x - o.x, y - o.y, z - o.z)

func mul(s: float) -> Vec3D:
	return Vec3D.new(x * s, y * s, z * s)

func add_scaled(o: Vec3D, s: float) -> Vec3D:
	return Vec3D.new(x + o.x * s, y + o.y * s, z + o.z * s)

func dot(o: Vec3D) -> float:
	return x * o.x + y * o.y + z * o.z

func cross(o: Vec3D) -> Vec3D:
	return Vec3D.new(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x)

func length_sq() -> float:
	return x * x + y * y + z * z

func length() -> float:
	return sqrt(x * x + y * y + z * z)

func distance_to(o: Vec3D) -> float:
	var dx := x - o.x
	var dy := y - o.y
	var dz := z - o.z
	return sqrt(dx * dx + dy * dy + dz * dz)

func normalized() -> Vec3D:
	var l := length()
	if l <= 0.0:
		return Vec3D.new(0.0, 1.0, 0.0)
	return Vec3D.new(x / l, y / l, z / l)

func lerp_to(o: Vec3D, t: float) -> Vec3D:
	return Vec3D.new(x + (o.x - x) * t, y + (o.y - y) * t, z + (o.z - z) * t)

func _to_string() -> String:
	return "(%.3f, %.3f, %.3f)" % [x, y, z]
