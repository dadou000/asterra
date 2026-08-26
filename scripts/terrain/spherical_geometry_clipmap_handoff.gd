extends "res://scripts/terrain/spherical_geometry_clipmap_micro.gd"
## Active cached-renderer handoff layer.
##
## This keeps the production topology/LOD/culling stack unchanged, but makes the
## tangent-anchor reset overridable. The cached renderer can therefore delay an
## anchor switch until a second GPU cache has been warmed for the destination.
## Non-cached behavior remains the exact immediate reset used by the parent.


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_visible(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_visible(false)
		return

	var observer_world: Vec3D = Frames.to_world(camera.global_position)
	var observer_radius: float = observer_world.length()
	if observer_radius <= 1.0:
		_set_visible(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_unit_world: Vec3D = observer_world.normalized()
	var observer_dir: Vector3 = observer_unit_world.to_v3()
	if not _have_anchor:
		_reset_anchor(observer_dir)
		_capture_stable_anchor(observer_unit_world.mul(radius))
		_update_visible_cap(observer_radius, radius)
		_update_active_levels()

	if not _debug_freeze:
		var observer_surface_world: Vec3D = observer_unit_world.mul(radius)
		var rel: Vec3D = observer_surface_world.sub(_stable_anchor_world)
		var px: float = rel.x * _anchor_right.x \
			+ rel.y * _anchor_right.y + rel.z * _anchor_right.z
		var py: float = rel.x * _anchor_up.x \
			+ rel.y * _anchor_up.y + rel.z * _anchor_up.z
		var resolved: Vector2 = _resolve_reanchor(
			observer_dir, observer_surface_world, Vector2(px, py))
		px = resolved.x
		py = resolved.y

		var snapped_center := Vector2(
			round(px / _base_spacing) * _base_spacing,
			round(py / _base_spacing) * _base_spacing)
		if snapped_center.distance_squared_to(_center_plane) > 1e-8:
			_center_plane = snapped_center
			_update_center_basis()

		_update_visible_cap(observer_radius, radius)
		_update_active_levels()

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_bind_gpu_resources(false)
	_sync_uniforms(origin)
	_sync_material_control()
	_set_visible(_bound_orbit != null)
	if _terrain_visible and _debug_side_cut:
		_show_all_active_sectors()

	# Preserve the two small process wrappers bypassed by this active-path override.
	_update_ring_labels(dt)
	_bind_gpu_context(false)


func _resolve_reanchor(observer_dir: Vector3, observer_surface_world: Vec3D,
		plane_offset: Vector2) -> Vector2:
	if absf(plane_offset.x) > REANCHOR_M or absf(plane_offset.y) > REANCHOR_M:
		_reset_anchor(observer_dir)
		_capture_stable_anchor(observer_surface_world)
		return Vector2.ZERO
	return plane_offset
