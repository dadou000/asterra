extends Node
## Autoload: hierarchical reference frames + floating origin.
##
##   Helion frame  ->  Asterra frame  ->  regional frame  ->  local physics frame
##                                                          ->  assembly frame
##
## Canonical state is double precision and planet-centred (Asterra frame). The
## Godot scene graph only ever sees the *local physics frame*: a float32 space
## whose origin is periodically re-based to follow the observer, so contact
## physics and rendering always happen within a few kilometres of (0,0,0).
##
## This is the single reason the player can fly from orbit to the ground without
## vertex jitter, and it must exist before any Phase 1 terrain code.

signal origin_shifted(delta_render: Vector3)

const REBASE_THRESHOLD := 4096.0

## --- Helion system (Asterra's star) -------------------------------------------
var helion_dir: Vector3 = Vector3(1, 0.15, 0.3).normalized()  ## sunward, Asterra frame
var axial_tilt_deg: float = 21.4
var day_seconds: float = 90000.0
var year_days: float = 402.0

## --- Asterra ------------------------------------------------------------------
var planet_radius: float = 1000000.0

## --- Local physics frame ------------------------------------------------------
var origin: Vec3D = Vec3D.new(0, 0, 0)

var _rebase_count: int = 0

func _ready() -> void:
	process_priority = -100

func set_planet_radius(r: float) -> void:
	planet_radius = r

## Asterra frame (double) -> local render frame (float32).
func to_render(p: Vec3D) -> Vector3:
	return Vector3(float(p.x - origin.x), float(p.y - origin.y), float(p.z - origin.z))

## Local render frame -> Asterra frame.
func to_world(p: Vector3) -> Vec3D:
	return Vec3D.new(origin.x + p.x, origin.y + p.y, origin.z + p.z)

## Surface point helpers -------------------------------------------------------
func dir_altitude_to_world(d: Vector3, altitude: float) -> Vec3D:
	var r := planet_radius + altitude
	return Vec3D.new(d.x * r, d.y * r, d.z * r)

func world_to_dir(p: Vec3D) -> Vector3:
	return p.normalized().to_v3()

func world_altitude(p: Vec3D) -> float:
	return p.length() - planet_radius

## Re-base the local frame onto a new Asterra-frame origin, and report the shift
## so live nodes can translate themselves.
func rebase(new_origin: Vec3D) -> void:
	var delta := Vector3(
		float(origin.x - new_origin.x),
		float(origin.y - new_origin.y),
		float(origin.z - new_origin.z))
	origin = new_origin.dup()
	_rebase_count += 1
	origin_shifted.emit(delta)

## Call each frame with the observer's render-space position.
func maintain_origin(observer_render_pos: Vector3) -> bool:
	if observer_render_pos.length() > REBASE_THRESHOLD:
		rebase(to_world(observer_render_pos))
		return true
	return false

func rebase_count() -> int:
	return _rebase_count

## Direction of the sun at a given surface point, in the local render frame.
func sun_dir_at(_d: Vector3) -> Vector3:
	return -helion_dir
