extends Node
## Comb tool harness: does a combed direction actually reach the cards, the cap
## and the cursor, and does it survive a preset round trip.
##   godot --path . res://tests/HairComb.tscn

var editor: Node3D
var lab: Node
var groom

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	editor = load("res://scenes/CharacterEditor.tscn").instantiate() as Node3D
	add_child(editor)
	for i in 150:
		await get_tree().process_frame
	lab = editor.get_node_or_null("GroomLab")
	groom = lab._groom
	DirAccess.make_dir_recursive_absolute("user://shots")
	editor._camera_transitioning = false
	editor._turntable_enabled = false
	var ui := editor.get_node_or_null("CharacterEditorUI")
	if ui != null:
		ui.visible = false
	for child in lab.get_children():
		if child is CanvasLayer:
			child.visible = false
	lab._apply_hair_preset(2)
	lab._hair_settings["length"] = 0.055
	lab._hair_settings["side_hairline"] = 0.95
	lab._apply_hair_now()
	for i in 40:
		await get_tree().process_frame

	var head: Vector3 = groom._mount.global_position
	var centre: Vector3 = groom._mount.to_global(groom._head_field_center)
	await _shot("comb_before", head, 0.30, -12.0, 55.0)

	print("=== COMB REACHES THE FLOW FIELD ===")
	var target := _surface(Vector3(0.72, 0.55, 0.42).normalized())
	var region: Vector3 = groom._scalp_region(groom._mount.to_global(target))
	var normal_world: Vector3 = (groom._mount.global_basis * (target - groom._head_field_center)).normalized()
	var before: Vector3 = groom._growth_flow(normal_world, region, 0, target)
	for angle_deg in [0.0, 90.0, 180.0, -90.0]:
		groom.clear_scalp_comb()
		groom.comb_scalp(groom._mount.to_global(target), 0.05, deg_to_rad(angle_deg), 1.0)
		var after: Vector3 = groom._growth_flow(normal_world, region, 0, target)
		var sampled: Array = groom._sample_scalp_comb(target)
		print("  comb %7.1f deg -> flow turned %5.1f deg from analytic, comb weight %.2f" % [
			angle_deg, rad_to_deg(before.angle_to(after)), float(sampled[1])])

	print("=== COMB REACHES THE RENDER ===")
	groom.clear_scalp_comb()
	groom.rebuild_hair()
	var plain := _mean_card_direction(target)
	# Comb a wide patch hard across the head.
	for step in 12:
		var swept := Vector3(0.72, lerpf(0.85, 0.15, float(step) / 11.0), lerpf(0.60, -0.10, float(step) / 11.0)).normalized()
		groom.comb_scalp(groom._mount.to_global(_surface(swept)), 0.05, deg_to_rad(90.0), 1.0)
	groom.rebuild_hair()
	var combed := _mean_card_direction(target)
	print("  mean card direction moved %.1f deg after combing" % rad_to_deg(plain.angle_to(combed)))
	print("  cap rebuilt for the comb: %s" % str(groom._scalp_cap_instance.mesh != null))

	print("=== CURSOR ARROW ===")
	lab._paint_tool = lab.TOOL_COMB
	lab._paint_radius = 0.03
	lab._comb_angle = deg_to_rad(90.0)
	groom.update_brush_cursor(groom._mount.to_global(target), 0.03, 0.0, lab._comb_angle)
	print("  comb cursor surfaces=%d (2 rings + shaft + 2 barbs = 5)" % groom._brush_cursor_mesh.get_surface_count())
	groom.update_brush_cursor(groom._mount.to_global(target), 0.03, 0.0, INF)
	print("  length cursor surfaces=%d (2 rings)" % groom._brush_cursor_mesh.get_surface_count())

	print("=== PRESET ROUND TRIP ===")
	var saved: Dictionary = lab._settings_for_preset_category("hair")
	print("  preset carries the comb: %s" % str(saved.has("scalp_comb")))
	var before_sample: Array = groom._sample_scalp_comb(target)
	groom.clear_scalp_comb()
	groom.set_scalp_comb_state(saved["scalp_comb"])
	var after_sample: Array = groom._sample_scalp_comb(target)
	print("  restored identically: %s" % str(
		Vector3(before_sample[0]).is_equal_approx(Vector3(after_sample[0]))
		and absf(float(before_sample[1]) - float(after_sample[1])) < 0.001))

	# A legible demo: comb the whole scalp one way, then the other, and shoot the
	# top where the direction is easiest to read.
	groom.hide_brush_cursor()
	for demo in [{"deg": 90.0, "tag": "comb_demo_across"}, {"deg": 0.0, "tag": "comb_demo_up"}]:
		groom.clear_scalp_comb()
		for row in groom.PAINT_ELEVATION:
			for column in groom.PAINT_AZIMUTH:
				var index: int = row * groom.PAINT_AZIMUTH + column
				groom._ensure_scalp_comb()
				groom._scalp_comb_angle[index] = deg_to_rad(float(demo["deg"]))
				groom._scalp_comb_weight[index] = 1.0
		groom._scalp_comb_revision += 1
		groom.rebuild_hair()
		for i in 40:
			await get_tree().process_frame
		await _shot(str(demo["tag"]), head, 0.32, -62.0, 90.0)
	print("done")
	get_tree().quit()

func _surface(direction: Vector3) -> Vector3:
	return groom._head_field_center + direction * groom._sample_head_field(direction)

## Average forward axis of the cards rooted near a point, in mount-local space.
func _mean_card_direction(local_point: Vector3) -> Vector3:
	var mm: MultiMesh = groom._gpu_hair_instance.multimesh
	var total := Vector3.ZERO
	for i in mm.visible_instance_count:
		var xf := mm.get_instance_transform(i)
		if xf.origin.distance_to(local_point) < 0.030:
			total += xf.basis.y.normalized()
	return total.normalized() if total.length_squared() > 0.0001 else Vector3.UP

func _shot(name: String, target: Vector3, distance: float, pitch_deg: float, yaw_deg: float) -> void:
	var pivot: Node3D = editor._camera_pivot
	var camera: Camera3D = editor._camera
	pivot.global_position = target
	pivot.rotation = Vector3(deg_to_rad(pitch_deg), deg_to_rad(yaw_deg), 0.0)
	camera.position = Vector3(0.0, 0.0, distance)
	camera.rotation = Vector3.ZERO
	for i in 8:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png("user://shots/%s.png" % name)
	print("  %s" % name)
