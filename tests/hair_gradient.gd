extends Node
## Scalp coherence harness.
##
## Paints an exact length gradient over the scalp (shaved at the bottom, full at
## the crown, like a fade) and renders it at several distances, so the short-hair
## end of the range can be judged instead of guessed at. Also reports, per length
## band, what the cap actually resolves to.
##
##   godot --path . res://tests/HairGradient.tscn
##
## Writes to user://shots/gradient_*.png

const BANDS := [0.0, 0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 16.0, 22.0, 30.0]

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

	# A short groom, so the whole gradient lives in the range that matters.
	lab._apply_hair_preset(2)
	lab._hair_settings["length"] = 0.030
	lab._hair_settings["front_density"] = 1.0
	lab._hair_settings["front_length"] = 1.0
	lab._hair_settings["part_style"] = 0
	# A fade needs hair all the way down to the ears. With the default hairline
	# the short end of the gradient lands BELOW it, where there is no cap and no
	# cards, so the test would be measuring bare scalp instead of short hair.
	lab._hair_settings["front_hairline"] = 0.62
	lab._hair_settings["side_hairline"] = 0.97
	lab._hair_settings["back_hairline"] = 0.97
	lab._apply_hair_now()
	for i in 40:
		await get_tree().process_frame

	var bake_started := Time.get_ticks_usec()
	groom._bake_follicle_map()
	print("  follicle map bake: %.1f ms (once, at load)" % (float(Time.get_ticks_usec() - bake_started) / 1000.0))
	var cap_started := Time.get_ticks_usec()
	groom._scalp_cap_signature = ""
	groom._build_scalp_cap(0.0012, 0, 0.030)
	print("  scalp cap rebuild: %.1f ms" % (float(Time.get_ticks_usec() - cap_started) / 1000.0))
	if groom._follicle_texture != null:
		groom._follicle_texture.get_image().save_png("user://shots/gradient_follicle_map.png")
		print("  follicle map written")
	_paint_gradient(0.030)
	groom.rebuild_hair()
	for i in 40:
		await get_tree().process_frame

	_report(0.030)
	var head: Vector3 = groom._mount.global_position
	# The key light comes from the front upper left, so the short end of the
	# gradient has to be shot from there or it is judged in shadow.
	var lit: Vector3 = head + Vector3(0.0, -0.055, 0.0)
	await _shot("gradient_far", head, 0.80, -6.0, 40.0)
	await _shot("gradient_mid", head, 0.42, -8.0, 40.0)
	await _shot("gradient_fade_lit", lit, 0.20, -8.0, 40.0)
	await _shot("gradient_fade_tight", lit, 0.13, -6.0, 34.0)
	await _shot("gradient_temple", head + Vector3(0.0, -0.03, 0.0), 0.15, -2.0, 62.0)
	await _shot("gradient_back", head, 0.36, -10.0, 160.0)
	print("shots in %s" % ProjectSettings.globalize_path("user://shots"))
	get_tree().quit()

## Writes the gradient straight into the paint map rather than painting dabs, so
## the length at any point is exact and the render can be read as a measurement.
##
## Keyed to distance above the hairline, not to a world axis. Mapping the ramp
## onto a raw Y range put most of it below the hairline, where there is neither
## cap nor cards, so the short end of the test was measuring bare scalp.
func _paint_gradient(base_length: float) -> void:
	groom._ensure_scalp_paint()
	var longest: float = BANDS[BANDS.size() - 1] * 0.001
	for row in groom.PAINT_ELEVATION:
		for column in groom.PAINT_AZIMUTH:
			var direction: Vector3 = groom._paint_direction(column, row)
			var margin := _hairline_margin_for(direction)
			# 0 mm right at the hairline, full length by the time it reaches the crown.
			var t := clampf(margin / 1.25, 0.0, 1.0)
			var index: int = row * groom.PAINT_AZIMUTH + column
			groom._scalp_paint_value[index] = clampf(t * longest / base_length, 0.0, 1.0)
			groom._scalp_paint_weight[index] = 1.0
	groom._scalp_paint_revision += 1

func _hairline_margin_for(direction: Vector3) -> float:
	var local: Vector3 = groom._head_field_center + direction * groom._sample_head_field(direction)
	return groom._hairline_margin(groom._scalp_region(groom._mount.to_global(local)))

## Finds the scalp direction whose painted length is closest to a target. Searched
## rather than inverted, because the gradient now follows the hairline field.
func _direction_for_length(target: float, base_length: float) -> Vector3:
	var best := Vector3.UP
	var best_error := INF
	var longest: float = BANDS[BANDS.size() - 1] * 0.001
	for row in groom.PAINT_ELEVATION:
		for column in groom.PAINT_AZIMUTH:
			var direction: Vector3 = groom._paint_direction(column, row)
			# Keep the probe on the lit side of the head so the shots and the
			# numbers are describing the same place.
			if direction.x < 0.35 or absf(direction.z) > 0.55:
				continue
			var margin := _hairline_margin_for(direction)
			if margin < 0.0:
				continue
			var length := clampf(margin / 1.25, 0.0, 1.0) * longest
			var error := absf(length - target)
			if error < best_error:
				best_error = error
				best = direction
	return best

func _report(base_length: float) -> void:
	print("=== SCALP COHERENCE (groom length %.0f mm) ===" % (base_length * 1000.0))
	print("  target   actual   cards  present  dots  flow  strands   what it should read as")
	var mm: MultiMesh = groom._gpu_hair_instance.multimesh
	for band in BANDS:
		var wanted := float(band) * 0.001
		var direction := _direction_for_length(wanted, base_length)
		var local: Vector3 = groom._head_field_center + direction * groom._sample_head_field(direction)
		var world: Vector3 = groom._mount.to_global(local)
		var region: Vector3 = groom._scalp_region(world)
		var actual: float = groom._root_length_scale(region, local) * base_length
		var cards := 0
		for i in mm.visible_instance_count:
			if mm.get_instance_transform(i).origin.distance_to(local) < 0.018:
				cards += 1
		var mm_value := actual * 1000.0
		# These mirror character_scalp_cap.gdshader. If the shader is retuned they
		# have to move with it, or the report quietly describes the old build.
		var present := smoothstep(0.30, 1.10, mm_value)
		var grow := pow(clampf((mm_value - 0.4) / 12.6, 0.0, 1.0), 0.62)
		var streak := smoothstep(1.0, 8.0, mm_value)
		var strand_mix := smoothstep(13.0, 24.0, mm_value)
		print("  %5.1f mm %6.1f mm %6d   %.2f  %.2f  %.2f   %.2f   %s" % [
			band, mm_value, cards, present, grow, streak, strand_mix,
			_expected(float(band))])

func _expected(mm_value: float) -> String:
	if mm_value < 0.8:
		return "bare skin"
	if mm_value < 6.0:
		return "stipple only, no cards"
	if mm_value < 12.0:
		return "dense stipple, skin still showing"
	return "continuous, strands take over"

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
