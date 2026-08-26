extends "res://scripts/terrain/spherical_geometry_clipmap_cached.gd"
## Final horizon-safe terrain layer.
##
## Horizon distance is allowed to choose how many logical rings exist, but it must
## never alter rendered vertex positions or shorten the submitted ring prefix. The
## previous dynamic visible-cap uniform made far spherical projection follow camera
## altitude, so descending could pull the apparent terrain edge toward the player.
## The inherited nadir optimisation also shortened the ring prefix from AGL. Both
## are disabled here without changing the proven cache, handoff, topology or LOD
## process chain.
##
## Sector topology is authored in the stable anchor tangent frame. Camera azimuth
## culling must use that same frame: using the moving centre tangent frame can rotate
## the cull wedges away from the actual compact sector meshes after long movement.

const RENDER_HEMISPHERE_CAP_RAD: float = PI * 0.5

var _sector_cull_failsafe_frames: int = 0


func _sync_uniforms(origin: Vector3) -> void:
	super._sync_uniforms(origin)
	if _material != null:
		# Keep the shader's geometric projection/cap independent of observer altitude.
		# _visible_cap_arc_m still drives automatic ring-count selection on the CPU.
		_material.set_shader_parameter("u_visible_cap_angle", RENDER_HEMISPHERE_CAP_RAD)


func _update_sector_visibility() -> void:
	# Keep global_gpu authoritative for sky/underground rejection and any other
	# whole-surface view decisions, then replace only the azimuth wedge selection.
	super._update_sector_visibility()
	if _view_surface_culled:
		return

	# Side-cut is an explicit topology debug mode; do not reinterpret its visibility.
	if _debug_side_cut:
		_set_view_ring_prefix(_physical_ring_count)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		return

	var forward: Vector3 = -camera.global_transform.basis.z.normalized()
	# IMPORTANT: compact sector ownership is defined by cell angle in the stable
	# anchor basis (anchor_right/anchor_up), not the moving centre tangent basis.
	var radial_dot: float = forward.dot(_anchor_dir)
	var forward_plane := Vector2(
		forward.dot(_anchor_right),
		forward.dot(_anchor_up))

	var show_all: bool = not is_finite(radial_dot) \
		or not is_finite(forward_plane.x) or not is_finite(forward_plane.y) \
		or absf(radial_dot) >= SECTOR_SHOW_ALL_RADIAL_DOT \
		or forward_plane.length_squared() < 1e-6

	var cos_limit: float = -1.0
	var forward_2d := Vector2.RIGHT
	if not show_all:
		forward_2d = forward_plane.normalized()
		var viewport_size: Vector2 = get_viewport().get_visible_rect().size
		var viewport_h: float = maxf(viewport_size.y, 1.0)
		var aspect: float = viewport_size.x / viewport_h
		var vertical_half: float = deg_to_rad(camera.fov) * 0.5
		var horizontal_half: float
		if camera.keep_aspect == Camera3D.KEEP_WIDTH:
			horizontal_half = vertical_half
		else:
			horizontal_half = atan(tan(vertical_half) * aspect)
		var limit: float = minf(
			horizontal_half + SECTOR_HALF_ANGLE + SECTOR_CULL_MARGIN_RAD,
			PI)
		cos_limit = cos(limit)

	var visible_mask := PackedByteArray()
	visible_mask.resize(SECTOR_COUNT)
	var visible_count: int = 0
	for sector: int in SECTOR_COUNT:
		var sector_visible: bool = show_all
		if not show_all:
			var angle: float = (float(sector) + 0.5) * TAU / float(SECTOR_COUNT)
			var sector_dir := Vector2(cos(angle), sin(angle))
			sector_visible = sector_dir.dot(forward_2d) >= cos_limit
		visible_mask[sector] = 1 if sector_visible else 0
		if sector_visible:
			visible_count += 1

	# A valid horizontal view can never mathematically select zero 30-degree
	# sectors with the current FOV/margins. Treat that state as numerical/stale-frame
	# corruption and fail open for one frame instead of making L0 disappear.
	if visible_count <= 0:
		_sector_cull_failsafe_frames += 1
		visible_count = SECTOR_COUNT
		for sector: int in SECTOR_COUNT:
			visible_mask[sector] = 1

	var ring_count: int = _active_ring_count()
	_visible_sector_count = visible_count
	for sector: int in SECTOR_COUNT:
		var sector_visible: bool = visible_mask[sector] != 0
		_center_sector_batches[sector].visible = sector_visible
		_sector_batches[sector].visible = sector_visible and ring_count > 0

	# Preserve the complete logical ring stack selected by the geometric horizon.
	# The old AGL-dependent nadir prefix is intentionally disabled in this layer.
	_set_view_ring_prefix(_physical_ring_count)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_horizon_safe"] = true
	out["terrain_render_cap_dynamic"] = false
	out["terrain_nadir_ring_prefix_cull"] = false
	out["terrain_sector_cull_anchor_basis"] = true
	out["terrain_sector_cull_failsafe_frames"] = _sector_cull_failsafe_frames
	return out
