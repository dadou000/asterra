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

## Canonical double position -> local float position.
func to_render(world: Vec3D) -> Vector3:
	return Vector3(
		float(world.x - origin.x),
		float(world.y - origin.y),
		float(world.z - origin.z)
	)

## Local float position -> canonical double position.
func to_world(render: Vector3) -> Vec3D:
	return Vec3D.new(
		origin.x + float(render.x),
		origin.y + float(render.y),
		origin.z + float(render.z)
	)

## Shift the render origin. Systems with persistent scene nodes listen to the
## signal and move them by -delta; canonical coordinates never change.
func rebase(new_origin: Vec3D) -> void:
	var delta := Vector3(
		float(new_origin.x - origin.x),
		float(new_origin.y - origin.y),
		float(new_origin.z - origin.z)
	)
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

## Direction from an Asterra point toward Helion, in the Asterra/render axes.
## helion_dir is explicitly stored as the sunward vector; returning its negative
## here made this helper disagree with every shader uniform using helion_dir.
func sun_dir_at(_d: Vector3) -> Vector3:
	return helion_dir
