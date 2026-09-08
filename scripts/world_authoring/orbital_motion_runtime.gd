extends Node
## Per-session Planet Studio node. Each frame it places Helion in the anchor
## body's rotating frame from the nested Keplerian orbit and the Frames sim clock.
##
## The anchor body's centre stays the origin of Frames world space -- terrain,
## ocean and scatter are all relative to it, and its motion through the system
## never enters Frames.origin (CelestialBodyPreviewRuntime renders the rest of the
## system anchor-relative). Only helion_dir / helion_distance_m / the Asterra year
## length become time-varying, and every existing consumer already polls those.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const ORBIT_MATH := preload("res://scripts/world_authoring/model/orbit_math.gd")

var _session: RefCounted
var _main: Node
var _anchor_body_id: String = ""


func bind(session: RefCounted, main: Node) -> void:
	_session = session
	_main = main
	set_process(true)


## The body Frames is centred on -- the detailed-runtime body when there is one,
## otherwise the selected body. Repointed from the runtime host on every
## _flush_active_body_preview.
func set_anchor(body_id: String) -> void:
	_anchor_body_id = body_id


func _process(_dt: float) -> void:
	if _session == null or _anchor_body_id.is_empty():
		return
	var system: Resource = _session.get("staged_system") as Resource
	if system == null:
		return
	var anchor: Resource = system.call("find_body", _anchor_body_id) as Resource
	if anchor == null or int(anchor.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		return
	var star: Resource = _root_star(system, anchor)
	if star == null:
		return

	var t: float = float(Frames.system_time_s)
	var anchor_pos: Vec3D = ORBIT_MATH.system_position(system, _anchor_body_id, t)
	var star_pos: Vec3D = ORBIT_MATH.system_position(system, String(star.get(&"body_id")), t)
	var to_star: Vec3D = star_pos.sub(anchor_pos)
	var distance_m: float = maxf(to_star.length(), 1.0)

	var parent: Resource = system.call("find_body", String(anchor.get(&"parent_body_id"))) as Resource
	var anchor_orbit: Resource = anchor.get(&"orbit") as Resource
	var semi_major: float = float(anchor_orbit.get(&"semi_major_axis_m")) if anchor_orbit != null else 0.0

	Frames.anchor_body_id = _anchor_body_id
	Frames.axial_tilt_deg = float(anchor.get(&"axial_tilt_deg"))
	Frames.day_seconds = maxf(0.001, absf(float(anchor.get(&"sidereal_rotation_period_s"))))
	Frames._rotation_phase0_deg = float(anchor.get(&"rotation_phase_at_epoch_deg"))
	Frames.set_orbit_period_s(ORBIT_MATH.period_s(semi_major, ORBIT_MATH.body_mu(parent)))
	Frames.helion_distance_m = distance_m
	Frames.helion_dir = Frames.sun_dir_from_system(to_star.to_v3().normalized())


## Walk parents to the first parentless STAR; fall back to the system's root STAR
## (Helion) so an anchor whose chain is momentarily broken still gets sunlight.
func _root_star(system: Resource, from_body: Resource) -> Resource:
	var cursor: Resource = from_body
	var guard: int = int((system.get(&"bodies") as Array).size()) + 2
	while cursor != null and guard > 0:
		guard -= 1
		var parent_id: String = String(cursor.get(&"parent_body_id"))
		if parent_id.is_empty():
			break
		var parent: Resource = system.call("find_body", parent_id) as Resource
		if parent == null:
			break
		if int(parent.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR \
				and String(parent.get(&"parent_body_id")).is_empty():
			return parent
		cursor = parent
	for body_value: Variant in system.get(&"bodies"):
		var body: Resource = body_value as Resource
		if body != null and int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR \
				and String(body.get(&"parent_body_id")).is_empty():
			return body
	return null
