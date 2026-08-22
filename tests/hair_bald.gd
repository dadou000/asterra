extends Node
## Balding / crown coherence: the top of the skull is where a spherical chart
## degenerates, so it gets its own macro shots and a numeric check that follicle
## spacing does not collapse there.
##   godot --path . res://tests/HairBald.tscn

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
	lab._hair_settings["length"] = 0.030
	lab._hair_settings["front_hairline"] = 0.62
	lab._hair_settings["side_hairline"] = 0.97
	lab._hair_settings["back_hairline"] = 0.97
	lab._apply_hair_now()
	for i in 40:
		await get_tree().process_frame

	_chart_report()
	var head: Vector3 = groom._mount.global_position
	# Uniform lengths, no gradient, so any variation that shows is the chart and
	# not the paint. The crown is where a spherical chart degenerates, so it gets
	# the macro shot at each length.
	for millimetres in [1.0, 2.0, 5.0, 9.0]:
		groom._ensure_scalp_paint()
		for index in groom._scalp_paint_value.size():
			groom._scalp_paint_value[index] = float(millimetres) / 30.0
			groom._scalp_paint_weight[index] = 1.0
		groom._scalp_paint_revision += 1
		groom.rebuild_hair()
		for i in 30:
			await get_tree().process_frame
		await _shot("bald_crown_%02dmm" % int(millimetres), head, 0.17, -84.0, 90.0)
	await _shot("bald_crown_wide", head, 0.30, -66.0, 90.0)
	print("done")
	get_tree().quit()

## Chart units per millimetre of scalp, sampled from the crown down to the ear.
## A converging chart shows up here as the number running away toward the crown.
func _chart_report() -> void:
	print("=== SCALP CHART DENSITY (chart units per mm of scalp) ===")
	print("  from crown   density   follicle spacing")
	var radius: float = groom._sample_head_field(Vector3.UP)
	for degrees in [0.0, 10.0, 25.0, 45.0, 65.0, 85.0]:
		var angle := deg_to_rad(degrees)
		var a := Vector3(sin(angle), cos(angle), 0.0).normalized()
		var step := deg_to_rad(0.5)
		var b := Vector3(sin(angle + step), cos(angle + step), 0.0).normalized()
		var chart_delta: float = (groom._scalp_chart(b) - groom._scalp_chart(a)).length()
		var world_delta: float = radius * step * 1000.0
		var density := chart_delta / world_delta
		print("  %5.0f deg %10.4f   %.2f mm" % [
			degrees, density, 1.0 / maxf(density * float(groom.FOLLICLE_GRID), 0.00001)])

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
