extends Node3D
## M7 verification: far celestial bodies are distance-compressed so the near
## camera keeps a bounded far plane (never AU scale) while every remote body's
## on-screen angular size is preserved exactly.
##   godot --headless --path . res://tests/validate_system_camera.tscn

const SYSTEM_SCALE_VIEW := preload("res://scripts/rendering/system_scale_view.gd")
const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const PREVIEW_RUNTIME := preload("res://scripts/world_authoring/celestial_body_preview_runtime.gd")

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	_test_remap_math()
	await _test_preview_placement()

	if _failed:
		get_tree().quit(1)
		return
	print("SYSTEM_CAMERA_OK")
	get_tree().quit(0)


func _test_remap_math() -> void:
	var V := SYSTEM_SCALE_VIEW
	# Un-remapped below NEAR_M.
	_assert(is_equal_approx(V.compressed_distance(1_000_000.0), 1_000_000.0), "distances below NEAR_M pass through unchanged")
	_assert(is_equal_approx(V.scale_for(500_000.0), 1.0), "scale multiplier is 1 for a near body")
	# Monotonic + bounded above.
	var prev := 0.0
	var d := V.NEAR_M
	while d < 2.0e12:
		var r: float = V.compressed_distance(d)
		_assert(r > prev, "compressed_distance is monotonic increasing (d=%s)" % String.num_scientific(d))
		_assert(r < V.VIEW_FAR_M, "compressed_distance(%s) = %s stays inside VIEW_FAR_M %s" % [
			String.num_scientific(d), String.num_scientific(r), String.num_scientific(V.VIEW_FAR_M)])
		prev = r
		d *= 3.0
	# Angular-size preservation: remap(d) == scale_for(d) * d.
	for dd: float in [5.0e6, 4.0e8, 1.495978707e11, 7.0e11]:
		_assert(absf(V.scale_for(dd) * dd - V.compressed_distance(dd)) < 1.0,
			"scale_for(d)*d == remap(d) at d=%s (angular size preserved)" % String.num_scientific(dd))
	# near_far never AU scale.
	_assert(V.near_far(1_000.0) >= V.VIEW_FAR_M, "near_far floors at VIEW_FAR_M")
	_assert(V.near_far(5.0e11) < 8.0e11, "near_far is bounded by the active family, not the system")


func _test_preview_placement() -> void:
	var cam := Camera3D.new()
	cam.current = true
	cam.near = 0.25
	cam.far = 400_000.0
	add_child(cam)

	var baseline := GEN_CONFIG.new()
	baseline.system_seed = 8571
	baseline.face_res = 24
	var system: CelestialSystemDefinition = GENERATOR.generate(8571, baseline.axial_tilt_deg)

	var preview: Node = PREVIEW_RUNTIME.new()
	add_child(preview)
	preview.call("show_system", system, GENERATOR.HOME_BODY_ID, GENERATOR.HOME_BODY_ID)
	await get_tree().process_frame   # let _process run _sync_floating_origin + _sync_camera_clip
	await get_tree().process_frame

	var V := SYSTEM_SCALE_VIEW
	# Surface branch: far is the tight family need, never AU scale.
	_assert(cam.far > 0.0 and cam.far < 8.0e7,
		"near-camera far is bounded on a surface (%s m)" % String.num_scientific(cam.far))
	_assert(cam.far / cam.near < 3.0e7,
		"far/near ratio stays out of the light-culler failure range on a surface (%.2e)" % (cam.far / cam.near))

	# Space branch: lift the camera well above NEAR_M -> far reaches the system
	# budget so every compressed body is in view, near rises to keep the ratio sane.
	cam.global_position = Vector3(0.0, 0.0, V.NEAR_M * 3.0)
	await get_tree().process_frame
	await get_tree().process_frame
	_assert(cam.far >= V.VIEW_FAR_M and cam.far < 8.0e7,
		"in space the near-camera far reaches the system budget but not AU scale (%s m)" % String.num_scientific(cam.far))
	_assert(cam.far / cam.near < 3.0e7,
		"far/near ratio stays sane in the space branch (%.2e, near=%.2f)" % [cam.far / cam.near, cam.near])

	var far_body_checked := false
	for body_value: Variant in system.bodies:
		var body: Resource = body_value as Resource
		var body_id: String = String(body.get(&"body_id"))
		if body_id == GENERATOR.HOME_BODY_ID:
			continue
		var root: Node3D = preview.find_child("CelestialPreview_%s" % body_id, true, false) as Node3D
		if root == null:
			continue
		var render_dist: float = root.position.length()
		var true_dbg: float = _preview_true_render(preview, body_id).length()
		var wp: Vec3D = preview.call("body_world_position", body_id)
		print("PROBE %s: root.pos.len=%s root.scale=%.6f true_render=%s bwp.len=%s Frames.origin.len=%s vis=%s" % [
			body_id, String.num_scientific(render_dist), root.scale.x, String.num_scientific(true_dbg),
			String.num_scientific(wp.length()), String.num_scientific(Frames.origin.length()), root.visible])
		_assert(render_dist <= V.VIEW_FAR_M * 1.02,
			"body %s is placed inside the view budget (%s m), not at its true AU distance" % [
				body_id, String.num_scientific(render_dist)])

		# Angular size of the drawn proxy vs. the true body.
		var true_pos: Vector3 = _preview_true_render(preview, body_id)
		var true_dist: float = maxf(true_pos.length(), 1.0)
		if true_dist < V.NEAR_M:
			_assert(is_equal_approx(root.scale.x, 1.0), "a near body (%s) is not compressed" % body_id)
			continue
		var true_radius: float = maxf(float(body.get(&"radius_m")), 1.0)
		var drawn_radius: float = true_radius * root.scale.x   # surface.scale = radius_m, root.scale = mul
		var true_ang: float = atan(true_radius / true_dist)
		var drawn_ang: float = atan(drawn_radius / render_dist)
		_assert(absf(true_ang - drawn_ang) < deg_to_rad(0.02),
			"body %s angular size preserved (%.5f vs %.5f deg)" % [
				body_id, rad_to_deg(true_ang), rad_to_deg(drawn_ang)])
		if true_dist > 1.0e10:
			_assert(root.scale.x < 0.2, "a ~AU body (%s) is strongly compressed (scale %.4f)" % [body_id, root.scale.x])
			far_body_checked = true

	_assert(far_body_checked, "the seeded system had at least one ~AU body to compress")
	preview.free()
	cam.queue_free()


## The proxy's UN-compressed render position (Frames.to_render of its stored
## anchor-relative world), for the angular-size comparison.
func _preview_true_render(preview: Node, body_id: String) -> Vector3:
	var wp: Vec3D = preview.call("body_world_position", body_id)
	return Frames.to_render(wp)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("SYSTEM_CAMERA_FAILED: %s" % message)
