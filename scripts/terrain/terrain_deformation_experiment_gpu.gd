extends "res://scripts/terrain/terrain_deformation_experiment.gd"
## GPU-stage experiment override.
##
## First contact is acquired from RenderedTerrainContactQuery, which samples the
## exact active clipmap cache + persistent edit mirror + active deformation texture
## used by the production vertex shader. Approximate procedural-query fallbacks can
## never start deformation. Once contact begins, the immediate local contact datum
## advances with the plastic yield submitted during that same physics step.
##
## Sphere footprint geometry is based on total embed depth below the original
## contact plane, not instantaneous overlap against already-yielded soil.

var _sphere_reference_ground_m := 0.0
var _sphere_contact_ground_m := 0.0
var _sphere_total_center_sink_m := 0.0
var _sphere_reference_ready := false
var _sphere_contact_started := false


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	if not _world_ready:
		return
	_sphere_reference_ground_m = 0.0
	_sphere_contact_ground_m = 0.0
	_sphere_total_center_sink_m = 0.0
	_sphere_reference_ready = false
	_sphere_contact_started = false
	RenderedTerrainContactQuery.request_height(_sphere_dir)
	var rendered_ground_m: float = _rendered_ground_height()
	if is_finite(rendered_ground_m):
		_sphere_reference_ground_m = rendered_ground_m
		_sphere_contact_ground_m = rendered_ground_m
		_sphere_reference_ready = true
		# When the exact sample is already resident, make R/Backspace start from a
		# literal ten metres above the surface the player can see.
		_sphere_altitude_msl = rendered_ground_m + SPHERE_RADIUS_M + 10.0
		_update_visual_transforms()


func _rendered_ground_height() -> float:
	RenderedTerrainContactQuery.request_height(_sphere_dir)
	return RenderedTerrainContactQuery.height_for_direction(_sphere_dir, NAN)


func _integrate_freefall(dt: float) -> void:
	_sphere_velocity_mps -= GRAVITY_MPS2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt


func _step_sphere(dt: float) -> void:
	# Before first contact, continuously refresh the exact *rendered* surface. No
	# terrain work is submitted until the sphere geometrically crosses that surface.
	if not _sphere_contact_started:
		var rendered_ground_m: float = _rendered_ground_height()
		if not is_finite(rendered_ground_m):
			_integrate_freefall(dt)
			_sphere_last_contact.clear()
			return

		_sphere_reference_ground_m = rendered_ground_m
		_sphere_contact_ground_m = rendered_ground_m
		_sphere_total_center_sink_m = 0.0
		_sphere_reference_ready = true

		var bottom_now_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
		var clearance_now_m: float = bottom_now_m - rendered_ground_m
		if clearance_now_m > 0.0:
			var next_velocity_mps: float = clampf(
				_sphere_velocity_mps - GRAVITY_MPS2 * dt, -180.0, 80.0)
			var next_altitude_m: float = _sphere_altitude_msl + next_velocity_mps * dt
			var next_bottom_m: float = next_altitude_m - SPHERE_RADIUS_M
			if next_bottom_m > rendered_ground_m:
				_sphere_velocity_mps = next_velocity_mps
				_sphere_altitude_msl = next_altitude_m
				_sphere_last_contact.clear()
				return
			# Continuous crossing: land exactly on the rendered surface and preserve
			# the incoming speed for the following contact substep.
			_sphere_velocity_mps = next_velocity_mps
			_sphere_altitude_msl = rendered_ground_m + SPHERE_RADIUS_M
			_sphere_contact_started = true
			_sphere_last_contact.clear()
			return

		# If the exact GPU sample arrived after the falling sphere marginally crossed
		# it, discard that query latency as penetration. Soil work starts from zero.
		_sphere_altitude_msl = rendered_ground_m + SPHERE_RADIUS_M
		_sphere_contact_started = true
		_sphere_last_contact.clear()
		return

	if not _sphere_reference_ready:
		_integrate_freefall(dt)
		return

	var bottom_altitude_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	var overlap_m: float = _sphere_contact_ground_m - bottom_altitude_m
	var embed_depth_m: float = maxf(_sphere_reference_ground_m - bottom_altitude_m, 0.0)
	var acceleration_mps2: float = -GRAVITY_MPS2
	var velocity_before_contact_mps: float = _sphere_velocity_mps
	var plastic_center_step_m := 0.0
	var effective_restitution := 0.0
	_sphere_last_contact.clear()

	if overlap_m > 0.0:
		var geometry_depth_m: float = minf(embed_depth_m, SPHERE_RADIUS_M)
		var contact_radius_m := SPHERE_RADIUS_M
		if geometry_depth_m < SPHERE_RADIUS_M:
			var contact_radius_sq: float = 2.0 * SPHERE_RADIUS_M * geometry_depth_m
			contact_radius_sq -= geometry_depth_m * geometry_depth_m
			contact_radius_m = sqrt(maxf(contact_radius_sq, 0.01))
		contact_radius_m = clampf(contact_radius_m, 0.12, SPHERE_RADIUS_M)

		var inward_speed_mps: float = maxf(-_sphere_velocity_mps, 0.0)
		var stopping_distance_m: float = maxf(
			0.18, minf(SPHERE_RADIUS_M, overlap_m + 0.22))
		var impact_force_n: float = _sphere_mass_kg * inward_speed_mps * inward_speed_mps
		impact_force_n /= 2.0 * stopping_distance_m
		var load_n: float = _sphere_mass_kg * GRAVITY_MPS2 + impact_force_n

		_sphere_last_contact = TerrainDeformation.apply_contact(
			_sphere_dir, contact_radius_m, load_n, maxf(embed_depth_m, 1e-5),
			_sphere_velocity_mps, 0.0, dt, 0.0, material_id,
			TerrainDeformation.FOOTPRINT_SPHERE, SPHERE_RADIUS_M)
		_sphere_last_contact["contact_overlap_m"] = overlap_m
		_sphere_last_contact["embed_depth_m"] = embed_depth_m
		_sphere_last_contact["contact_radius_m"] = contact_radius_m

		var support_force_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
		acceleration_mps2 += support_force_n / maxf(_sphere_mass_kg, 1.0)
		acceleration_mps2 = clampf(acceleration_mps2, -180.0, 260.0)

		plastic_center_step_m = maxf(
			float(_sphere_last_contact.get("deformed_m", 0.0)), 0.0)
		if plastic_center_step_m > 0.0:
			_sphere_total_center_sink_m += plastic_center_step_m
			_sphere_contact_ground_m = _sphere_reference_ground_m - _sphere_total_center_sink_m

		var plastic_fraction: float = clampf(
			plastic_center_step_m / maxf(overlap_m, 1e-5), 0.0, 1.0)
		effective_restitution = float(
			_sphere_last_contact.get("restitution", 0.0)) * (1.0 - plastic_fraction)
		_sphere_last_contact["effective_restitution"] = effective_restitution
		_sphere_last_contact["contact_ground_m"] = _sphere_contact_ground_m
		_sphere_last_contact["total_center_sink_m"] = _sphere_total_center_sink_m

	_sphere_velocity_mps += acceleration_mps2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt

	if overlap_m > 0.0:
		var minimum_center_altitude_m: float = _sphere_contact_ground_m + SPHERE_RADIUS_M
		var projected := false
		if _sphere_altitude_msl < minimum_center_altitude_m:
			_sphere_altitude_msl = minimum_center_altitude_m
			projected = true

		var terrain_retreat_speed_mps: float = -plastic_center_step_m / maxf(dt, 1e-6)
		if projected:
			if effective_restitution <= 0.03:
				_sphere_velocity_mps = terrain_retreat_speed_mps
			elif velocity_before_contact_mps < 0.0 and _sphere_velocity_mps > 0.0:
				var rebound_limit_mps: float = -velocity_before_contact_mps * effective_restitution
				_sphere_velocity_mps = minf(_sphere_velocity_mps, rebound_limit_mps)
			elif _sphere_velocity_mps < terrain_retreat_speed_mps:
				_sphere_velocity_mps = terrain_retreat_speed_mps
		elif velocity_before_contact_mps < 0.0 and _sphere_velocity_mps > 0.0:
			var rebound_limit_mps: float = -velocity_before_contact_mps * effective_restitution
			_sphere_velocity_mps = minf(_sphere_velocity_mps, rebound_limit_mps)

		if absf(_sphere_velocity_mps) < 0.015:
			var support_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
			var weight_n: float = _sphere_mass_kg * GRAVITY_MPS2
			if support_n >= weight_n * 0.98 and plastic_center_step_m < 1e-5:
				_sphere_velocity_mps = 0.0


func _update_hud() -> void:
	if _hud_label == null:
		return
	if not _world_ready:
		_hud_label.text = "TERRAIN DEFORMATION EXPERIMENT\nWaiting for the generated world...\nF10: close experiment"
		return
	var rendered_ground_m: float = _rendered_ground_height()
	var clearance_text := "waiting exact render sample"
	if is_finite(rendered_ground_m):
		var clearance_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M - rendered_ground_m
		clearance_text = "clearance %+6.2f m" % clearance_m
	var bearing_ratio := 0.0
	var sink_rate := 0.0
	if not _sphere_last_contact.is_empty():
		bearing_ratio = float(_sphere_last_contact.get("bearing_ratio", 0.0))
		sink_rate = float(_sphere_last_contact.get("sink_rate_mps", 0.0))
	var sample_info: Dictionary = RenderedTerrainContactQuery.sample_info(_sphere_dir)
	var render_level: int = int(sample_info.get("level", -1))
	var render_morph: float = float(sample_info.get("morph", 0.0))
	var state_stats: Dictionary = TerrainDeformation.stats()
	_hud_label.text = (
		"TERRAIN DEFORMATION EXPERIMENT\n" +
		"Material [1-4]: %s\n" % TerrainDeformation.material_name(material_id) +
		"Tungsten sphere: %.1f t | %s | v %+6.2f m/s | bearing %.2fx | sink %.3f m/s\n" % [
			_sphere_mass_kg / 1000.0, clearance_text, _sphere_velocity_mps,
			bearing_ratio, sink_rate] +
		"Rendered contact: L%d morph %.2f | locked %s\n" % [
			render_level, render_morph, "YES" if _sphere_contact_started else "NO"] +
		"Bucket reaction: %.0f kN | moved this step %.4f m3 | state tiles %d\n\n" % [
			_bucket_total_reaction_n / 1000.0, _bucket_last_moved_volume_m3,
			int(state_stats.get("state_tiles", 0))] +
		"R place + DROP sphere at aim | P pause/resume | Backspace reset suspended\n" +
		"Bucket: I/K forward/back  J/L left/right  U/O up/down  Z/X pitch  Shift fast  B reset\n" +
		"1 topsoil  2 wet clay  3 gravel  4 rock | Delete clears terrain edits | F10 close"
	)
