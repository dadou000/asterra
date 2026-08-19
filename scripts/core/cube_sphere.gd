class_name CubeSphere
extends RefCounted
## Equi-angular cube-sphere mapping.
##
## Six square faces, each parameterised by (u, v) in [-1, 1]. The equi-angular
## warp (tan / atan) keeps cell sizes far more uniform than a naive gnomonic
## cube-sphere, and unlike the "adjusted cube" mapping it inverts in closed form,
## which the streaming quadtree and the terrain-edit system both need.
##
## Face order: 0:+X  1:-X  2:+Y  3:-Y  4:+Z  5:-Z

const FACE_PX := 0
const FACE_NX := 1
const FACE_PY := 2
const FACE_NY := 3
const FACE_PZ := 4
const FACE_NZ := 5

const AXIS := [
	Vector3(1, 0, 0), Vector3(-1, 0, 0),
	Vector3(0, 1, 0), Vector3(0, -1, 0),
	Vector3(0, 0, 1), Vector3(0, 0, -1),
]
## For every face, RIGHT x UP must equal AXIS -- the same handedness on all
## six faces, or a quadtree chunk on one face and its neighbour across a cube
## edge disagree about which way (u, v) runs along their shared boundary and
## their edge vertices land nowhere near each other. Faces 2 and 3 (+Y/-Y,
## the poles) used to be mirrored relative to the other four.
const RIGHT := [
	Vector3(0, 0, -1), Vector3(0, 0, 1),
	Vector3(-1, 0, 0), Vector3(-1, 0, 0),
	Vector3(1, 0, 0), Vector3(-1, 0, 0),
]
const UP := [
	Vector3(0, 1, 0), Vector3(0, 1, 0),
	Vector3(0, 0, 1), Vector3(0, 0, -1),
	Vector3(0, 1, 0), Vector3(0, 1, 0),
]

const Q := PI * 0.25

## (face, u, v) -> unit direction.
static func face_uv_to_dir(face: int, u: float, v: float) -> Vector3:
	var tu := tan(u * Q)
	var tv := tan(v * Q)
	var axis: Vector3 = AXIS[face]
	var right: Vector3 = RIGHT[face]
	var up: Vector3 = UP[face]
	return (axis + right * tu + up * tv).normalized()

## Double-precision (face, u, v) -> unit direction. Vector3 is float32, which
## costs ~0.1 m of positional error at a 1000 km radius -- fine for sampling a
## height field, not fine for placing a vertex on a 0.75 m edit lattice.
static func face_uv_to_dir_d(face: int, u: float, v: float) -> Vec3D:
	var tu := tan(u * Q)
	var tv := tan(v * Q)
	var a: Vector3 = AXIS[face]
	var r: Vector3 = RIGHT[face]
	var up: Vector3 = UP[face]
	var x := float(a.x) + float(r.x) * tu + float(up.x) * tv
	var y := float(a.y) + float(r.y) * tu + float(up.y) * tv
	var z := float(a.z) + float(r.z) * tu + float(up.z) * tv
	var l := sqrt(x * x + y * y + z * z)
	return Vec3D.new(x / l, y / l, z / l)

## Unit direction -> [face, u, v].
static func dir_to_face_uv(d: Vector3) -> Array:
	var ax := absf(d.x)
	var ay := absf(d.y)
	var az := absf(d.z)
	var face: int
	if ax >= ay and ax >= az:
		face = FACE_PX if d.x > 0.0 else FACE_NX
	elif ay >= az:
		face = FACE_PY if d.y > 0.0 else FACE_NY
	else:
		face = FACE_PZ if d.z > 0.0 else FACE_NZ
	var axis: Vector3 = AXIS[face]
	var right: Vector3 = RIGHT[face]
	var up: Vector3 = UP[face]
	var a: float = axis.dot(d)
	if absf(a) < 1e-12:
		a = 1e-12 if a >= 0.0 else -1e-12
	var u := atan(right.dot(d) / a) / Q
	var v := atan(up.dot(d) / a) / Q
	return [face, u, v]

## Latitude / longitude in radians (Y is the polar axis).
static func dir_to_latlon(d: Vector3) -> Vector2:
	return Vector2(asin(clampf(d.y, -1.0, 1.0)), atan2(d.z, d.x))

static func latlon_to_dir(lat: float, lon: float) -> Vector3:
	var c := cos(lat)
	return Vector3(c * cos(lon), sin(lat), c * sin(lon))

## Orthonormal tangent basis at a surface direction: [east, north].
static func tangent_basis(d: Vector3) -> Array:
	var up := Vector3(0, 1, 0)
	if absf(d.y) > 0.999:
		up = Vector3(0, 0, 1)
	var east := up.cross(d).normalized()
	var north := d.cross(east).normalized()
	return [east, north]
