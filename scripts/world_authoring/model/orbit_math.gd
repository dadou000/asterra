class_name OrbitMath
extends RefCounted
## Shared Keplerian orbit evaluation for Planet Studio.
##
## Extracted verbatim from CelestialBodyPreviewRuntime._orbit_offset (which now
## delegates here) plus a time term: mean anomaly advances as
##   M(t) = M_epoch + n * (t - epoch_s),   n = sqrt(mu / a^3)
## so `orbit_offset(..., mu, t)` with `mu <= 0` OR `t == epoch_s` reproduces the
## previous frozen-at-epoch position exactly.
##
## Frame convention (matches _orbit_offset's rotation block and the
## world_authoring_runtime_host "staged Keplerian reference plane is XZ" comment):
## the returned Vec3D is parent-centred, +Y = orbit-plane normal / ecliptic north,
## the reference plane is XZ.

const G := 6.67430e-11

## Fallback orbit shaping when a child has no authored semi-major axis / anomaly.
## Kept identical to CelestialBodyPreviewRuntime so delegated calls do not move.
const FALLBACK_ORBIT_PARENT_RADII := 4.0
const FALLBACK_ORBIT_CHILD_RADII := 3.0
const FALLBACK_ANOMALY_DEG := 35.0

const NEWTON_ITERATIONS := 8

## The fixed sunward vector the world was authored/tuned against before Asterra
## orbited Helion (frames.gd's pre-orbit default). epoch_alignment_for() picks the
## orbit phase that reproduces it at system_time_s == 0, so scrubbing the clock is
## the only thing that changes the lighting -- day 0 looks exactly as it did.
const LEGACY_SUNWARD := Vector3(1.0, 0.15, 0.3)


## For a coplanar (inclination 0) orbit evaluated at mean anomaly 0 (perihelion),
## returns { argp_deg, rotation_phase0_deg } such that
##   Frames.sun_dir_from_system((-orbit_pos).normalized())  at system_time_s == 0
## equals `target` (exactly in azimuth; in elevation up to what axial tilt allows).
static func epoch_alignment_for(target: Vector3, tilt_deg: float) -> Dictionary:
	var t: Vector3 = target.normalized()
	var tilt: float = deg_to_rad(tilt_deg)
	# helion_dir.y is spin-invariant and equals -sin(tilt) * sin(argp) with Omega = 0.
	var sin_argp: float = clampf(-t.y / maxf(sin(tilt), 1e-4), -1.0, 1.0)
	var argp: float = asin(sin_argp)
	var s: Vector3 = Vector3(-cos(argp), 0.0, -sin(argp))  # -(orbit_pos dir at perihelion)
	var seasonal: Vector3 = Basis(Vector3(1.0, 0.0, 0.0), -tilt) * s
	# sun_dir_from_system applies Basis(Y, -spin); that adds +spin to the azimuth.
	var phase0: float = atan2(t.z, t.x) - atan2(seasonal.z, seasonal.x)
	return {
		"argp_deg": rad_to_deg(argp),
		"rotation_phase0_deg": rad_to_deg(phase0),
	}


## Standard gravitational parameter of a body: its authored GM, else G * mass,
## else 0 (which makes any orbit around it time-frozen).
static func body_mu(body: Resource) -> float:
	if body == null:
		return 0.0
	var gm: float = float(body.get(&"gravitational_parameter_m3_s2"))
	if gm > 0.0:
		return gm
	var mass: float = float(body.get(&"mass_kg"))
	return G * mass if mass > 0.0 else 0.0


## Mean motion (rad/s). 0 when the orbit or the parent mass is undefined.
static func mean_motion(semi_major_axis_m: float, mu: float) -> float:
	if semi_major_axis_m <= 0.0 or mu <= 0.0:
		return 0.0
	return sqrt(mu / (semi_major_axis_m * semi_major_axis_m * semi_major_axis_m))


## Orbital period (s). 0 when mean motion is undefined.
static func period_s(semi_major_axis_m: float, mu: float) -> float:
	var n: float = mean_motion(semi_major_axis_m, mu)
	return TAU / n if n > 0.0 else 0.0


## Newton iteration for the eccentric anomaly. Fixed unconditional iteration count
## (matches the legacy solver so delegated preview positions are byte-identical).
static func solve_eccentric_anomaly(mean_anomaly: float, eccentricity: float) -> float:
	var eccentric_anomaly: float = mean_anomaly
	for _iteration: int in NEWTON_ITERATIONS:
		var f: float = eccentric_anomaly - eccentricity * sin(eccentric_anomaly) - mean_anomaly
		var fp: float = maxf(1.0 - eccentricity * cos(eccentric_anomaly), 1e-8)
		eccentric_anomaly -= f / fp
	return eccentric_anomaly


## Parent-centred position of `orbit` at sim time `t` (seconds). `mu` is the
## parent's standard gravitational parameter; pass 0 (or `t == orbit.epoch_s`) for
## the frozen epoch position.
static func orbit_offset(orbit: Resource, parent_radius_m: float,
		child_radius_m: float, mu: float, t: float) -> Vec3D:
	var semi_major_axis: float = float(orbit.get(&"semi_major_axis_m")) if orbit != null else 0.0
	var fallback_orbit: bool = semi_major_axis <= 0.0
	if fallback_orbit:
		var parent_radius: float = maxf(parent_radius_m, 1.0)
		var child_radius: float = maxf(child_radius_m, 1.0)
		semi_major_axis = maxf(
			parent_radius * FALLBACK_ORBIT_PARENT_RADII,
			parent_radius + child_radius * FALLBACK_ORBIT_CHILD_RADII)

	var eccentricity: float = clampf(
		float(orbit.get(&"eccentricity")) if orbit != null else 0.0, 0.0, 0.999999)
	var mean_anomaly_deg: float = float(orbit.get(&"mean_anomaly_at_epoch_deg")) if orbit != null else 0.0
	if fallback_orbit and absf(mean_anomaly_deg) <= 1e-6:
		mean_anomaly_deg = FALLBACK_ANOMALY_DEG

	var epoch_s: float = float(orbit.get(&"epoch_s")) if orbit != null else 0.0
	var mean_anomaly: float = deg_to_rad(mean_anomaly_deg) \
		+ mean_motion(semi_major_axis, mu) * (t - epoch_s)

	var eccentric_anomaly: float = solve_eccentric_anomaly(mean_anomaly, eccentricity)

	var orbital_x: float = semi_major_axis * (cos(eccentric_anomaly) - eccentricity)
	var orbital_z: float = semi_major_axis \
		* sqrt(maxf(1.0 - eccentricity * eccentricity, 0.0)) * sin(eccentric_anomaly)

	var inclination: float = deg_to_rad(float(orbit.get(&"inclination_deg")) if orbit != null else 0.0)
	var ascending_node: float = deg_to_rad(
		float(orbit.get(&"longitude_ascending_node_deg")) if orbit != null else 0.0)
	var periapsis: float = deg_to_rad(
		float(orbit.get(&"argument_periapsis_deg")) if orbit != null else 0.0)

	var cos_o: float = cos(ascending_node)
	var sin_o: float = sin(ascending_node)
	var cos_w: float = cos(periapsis)
	var sin_w: float = sin(periapsis)
	var cos_i: float = cos(inclination)
	var sin_i: float = sin(inclination)
	var x: float = (cos_o * cos_w - sin_o * sin_w * cos_i) * orbital_x \
		+ (-cos_o * sin_w - sin_o * cos_w * cos_i) * orbital_z
	var y: float = (sin_w * sin_i) * orbital_x + (cos_w * sin_i) * orbital_z
	var z: float = (sin_o * cos_w + cos_o * sin_w * cos_i) * orbital_x \
		+ (-sin_o * sin_w + cos_o * cos_w * cos_i) * orbital_z
	return Vec3D.new(x, y, z)


## Absolute position of `body_id` in the system frame (root body at the origin) at
## sim time `t`. Walks `parent_body_id` to the root, summing per-hop offsets with
## each parent's own GM. A parentless body (or a broken chain) resolves to origin.
static func system_position(system: Resource, body_id: String, t: float) -> Vec3D:
	if system == null:
		return Vec3D.new()
	var body: Resource = system.call("find_body", body_id) as Resource
	if body == null:
		return Vec3D.new()

	var offset: Vec3D = Vec3D.new()
	var cursor: Resource = body
	var visited: Dictionary = {}
	var guard: int = int(system.get(&"bodies").size()) + 2
	while cursor != null and guard > 0:
		guard -= 1
		var cursor_id: String = String(cursor.get(&"body_id"))
		if visited.has(cursor_id):
			break
		visited[cursor_id] = true
		var parent_id: String = String(cursor.get(&"parent_body_id"))
		if parent_id.is_empty():
			break
		var parent: Resource = system.call("find_body", parent_id) as Resource
		if parent == null:
			break
		var hop: Vec3D = orbit_offset(
			cursor.get(&"orbit") as Resource,
			maxf(float(parent.get(&"radius_m")), 1.0),
			maxf(float(cursor.get(&"radius_m")), 1.0),
			body_mu(parent), t)
		offset = offset.add(hop)
		cursor = parent
	return offset
