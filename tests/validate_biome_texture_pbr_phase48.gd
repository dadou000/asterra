extends Node
## Regression for the extended Biome Texture feature set: gradients, full
## material overrides (roughness/metallic/anisotropy/emission), and imported
## PBR textures. Drives the real Planet Studio UI controls end to end rather
## than calling internals directly -- the TEXTURE_BAND node-type-registration
## trap (see memory: biome-texture-surface-authoring) proved that a graph node
## type left out of terrain_shader_graph_definition.gd's whitelists silently
## downgrades to CONSTANT_FLOAT with no error anywhere in the pipeline, so this
## test exists specifically to catch that class of bug for CUSTOM_TEXTURE too.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase42.gd")


class DummyWorld extends Node3D:
	var player: Node = null


class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase48_biome_texture_pbr_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	host.add_child(layer)
	var editor: Control = PLANET_STUDIO_SCENE.instantiate() as Control
	if editor == null:
		_fail("Planet Studio failed to instantiate")
		return
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "TERRAIN")
	await _frames(3)

	var texture_tab: Button = _find_button(editor, "BIOME TEXTURE")
	if texture_tab == null:
		_fail("Terrain tab is missing the Biome Texture editor")
		return
	texture_tab.emit_signal("pressed")
	await _frames(3)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("No active terrain profile")
		return

	# --- Import a custom texture through the real Import button/dialog ---
	var import_button: Button = _find_named(editor, "AddCustomTexture") as Button
	if import_button == null:
		_fail("Texture library section is missing its Import button")
		return
	var import_dialog: FileDialog = _find_named(editor, "ImportCustomTextureDialog") as FileDialog
	if import_dialog == null:
		_fail("Texture library section is missing its import FileDialog")
		return
	var probe_png_path: String = "user://phase48_probe_texture.png"
	var probe_image := Image.create(4, 4, false, Image.FORMAT_RGB8)
	probe_image.fill(Color(0.2, 0.6, 0.3))
	probe_image.save_png(probe_png_path)
	import_dialog.emit_signal("file_selected", ProjectSettings.globalize_path(probe_png_path))
	await _frames(3)

	var library_slot: Resource = terrain.call("find_shader_slot",
		TerrainDisplacementRuntime.BIOME_TEXTURE_LIBRARY_SLOT_ID) as Resource
	if library_slot == null:
		_fail("Importing a texture did not provision the texture library slot")
		return
	var library_graph: Resource = library_slot.get(&"graph") as Resource
	# The exact trap this test is here to catch: an unregistered node type
	# silently becomes CONSTANT_FLOAT instead of CUSTOM_TEXTURE, with no error.
	if not _graph_has_node_type(library_graph, "CUSTOM_TEXTURE"):
		_fail("Imported texture did not serialize as a CUSTOM_TEXTURE node -- possible node-type registration trap")
		return
	var entries: Array = editor.call("_phase47_custom_texture_stack", library_graph) as Array
	if entries.size() != 1:
		_fail("Texture library does not contain exactly one imported entry after import")
		return
	var imported_name: String = String((entries[0] as Dictionary).get("name", ""))
	if imported_name.is_empty():
		_fail("Imported texture has no name")
		return
	if (entries[0] as Dictionary).get("albedo_png", PackedByteArray()).is_empty():
		_fail("Imported texture has no embedded albedo PNG bytes")
		return

	# The CPU runtime must actually build a Texture2DArray from the embedded PNG.
	var probe_rt: Node = RUNTIME.new()
	probe_rt.call("compile_from_terrain", terrain)
	var custom_names: PackedStringArray = probe_rt.call("biome_texture_custom_names") as PackedStringArray
	if custom_names.size() != 1 or custom_names[0] != imported_name:
		_fail("Runtime did not parse the imported texture library (names=%s)" % [custom_names])
		probe_rt.free()
		return
	var tex_packed: Dictionary = probe_rt.call("biome_texture_uniforms") as Dictionary
	if int(tex_packed.get("custom_count", 0)) != 1:
		_fail("biome_texture_uniforms did not report the imported texture")
		probe_rt.free()
		return
	var custom_albedo: Variant = tex_packed.get("custom_albedo")
	if custom_albedo == null or not (custom_albedo is Texture2DArray):
		_fail("biome_texture_uniforms did not build a custom albedo Texture2DArray")
		probe_rt.free()
		return
	probe_rt.free()

	# --- Import a normal map for that same texture through the real
	#     per-entry Import-normal-map button/dialog. ---
	var normal_button: Button = _find_named(editor, "ImportCustomTextureNormal_0") as Button
	if normal_button == null:
		_fail("Custom texture card is missing its Import normal map button")
		return
	var normal_dialog: FileDialog = _find_named(editor, "ImportCustomTextureNormalDialog_0") as FileDialog
	if normal_dialog == null:
		_fail("Custom texture card is missing its normal-map import FileDialog")
		return
	var normal_png_path: String = "user://phase48_probe_normal.png"
	var normal_image := Image.create(4, 4, false, Image.FORMAT_RGB8)
	normal_image.fill(Color(0.5, 0.5, 1.0))
	normal_image.save_png(normal_png_path)
	normal_dialog.emit_signal("file_selected", ProjectSettings.globalize_path(normal_png_path))
	await _frames(3)

	var entries_with_normal: Array = editor.call("_phase47_custom_texture_stack", library_graph) as Array
	if (entries_with_normal[0] as Dictionary).get("normal_png", PackedByteArray()).is_empty():
		_fail("Imported normal map did not serialize into the CUSTOM_TEXTURE node")
		return

	var normal_rt: Node = RUNTIME.new()
	normal_rt.call("compile_from_terrain", terrain)
	var normal_packed: Dictionary = normal_rt.call("biome_texture_uniforms") as Dictionary
	var custom_normal: Variant = normal_packed.get("custom_normal")
	if custom_normal == null or not (custom_normal is Texture2DArray):
		_fail("biome_texture_uniforms did not build a custom normal Texture2DArray")
		normal_rt.free()
		return
	normal_rt.free()

	# --- Now compose a band that uses gradients, the imported texture, and
	#     every new material override. ---
	var biome_picker: OptionButton = _find_named(editor, "BiomeTexturePicker") as OptionButton
	if biome_picker == null:
		_fail("Biome Texture editor is missing biome selection")
		return
	biome_picker.select(9) # a biome distinct from the default (0)
	biome_picker.emit_signal("item_selected", 9)
	await _frames(3)

	var add_band: Button = _find_named(editor, "AddBiomeTextureLayer") as Button
	if add_band == null:
		_fail("Biome Texture editor has no Add band button")
		return
	add_band.emit_signal("pressed")
	await _frames(3)

	var texture_slot: Resource = terrain.call("find_shader_slot", "simple-biome-texture-9") as Resource
	if texture_slot == null:
		_fail("Biome Texture editor did not auto-provision its scoped slot")
		return
	var texture_graph: Resource = texture_slot.get(&"graph") as Resource

	var choice_picker: OptionButton = _find_named(editor, "BiomeTextureLayerChoice_0") as OptionButton
	if choice_picker == null:
		_fail("Texture band is missing its Appearance dropdown")
		return
	if choice_picker.item_count < 6:
		_fail("Appearance dropdown does not include the imported texture (item_count=%d)" % choice_picker.item_count)
		return
	choice_picker.select(5) # first (and only) imported texture
	choice_picker.emit_signal("item_selected", 5)
	await _frames(3)

	var layers: Array = editor.call("_phase47_texture_stack", texture_graph) as Array
	if layers.size() != 1:
		_fail("Texture band stack does not have exactly one band")
		return
	var band0: Dictionary = layers[0] as Dictionary
	if int(band0.get("texture_choice", -1)) != 5 or int(band0.get("custom_texture_index", -1)) != 0:
		_fail("Selecting the imported texture did not set texture_choice=5/custom_texture_index=0 (got %s/%s)"
			% [band0.get("texture_choice"), band0.get("custom_texture_index")])
		return

	var gradient_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_gradient_strength") as SpinBox
	if gradient_spin == null:
		_fail("Texture band is missing the gradient strength control")
		return
	gradient_spin.value = 0.75
	gradient_spin.emit_signal("value_changed", 0.75)
	await _frames(3)

	# A flat curve at y=1 makes the distribution curve's effect on the packed
	# grad_t fully deterministic (always 1.0, no matter which x it's given),
	# so this proves the curve is wired end to end without needing to
	# hand-compute the real smoothstep shape.
	var gradient_curve_field: CurveFieldControl = _find_named(editor,
		"BiomeTextureLayerGradientCurve_0") as CurveFieldControl
	if gradient_curve_field == null:
		_fail("Texture band is missing the gradient distribution curve editor")
		return
	gradient_curve_field.set_points(PackedFloat32Array([0.0, 1.0, 1.0, 1.0]))
	gradient_curve_field.curve_changed.emit(gradient_curve_field.get_points())
	await _frames(3)

	var roughness_toggle: CheckButton = _find_named(editor, "BiomeTextureLayerField_roughness_enabled") as CheckButton
	var roughness_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_roughness_value") as SpinBox
	if roughness_toggle == null or roughness_spin == null:
		_fail("Texture band is missing roughness override controls")
		return
	roughness_toggle.button_pressed = true
	roughness_toggle.emit_signal("toggled", true)
	await _frames(3)
	roughness_spin = _find_named(editor, "BiomeTextureLayerField_roughness_value") as SpinBox
	roughness_spin.value = 0.2
	roughness_spin.emit_signal("value_changed", 0.2)
	await _frames(3)

	var metallic_toggle: CheckButton = _find_named(editor, "BiomeTextureLayerField_metallic_enabled") as CheckButton
	if metallic_toggle == null:
		_fail("Texture band is missing the metallic override toggle")
		return
	metallic_toggle.button_pressed = true
	metallic_toggle.emit_signal("toggled", true)
	await _frames(3)
	var metallic_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_metallic_value") as SpinBox
	metallic_spin.value = 0.9
	metallic_spin.emit_signal("value_changed", 0.9)
	await _frames(3)

	var anisotropy_toggle: CheckButton = _find_named(editor, "BiomeTextureLayerField_anisotropy_enabled") as CheckButton
	if anisotropy_toggle == null:
		_fail("Texture band is missing the anisotropy override toggle")
		return
	anisotropy_toggle.button_pressed = true
	anisotropy_toggle.emit_signal("toggled", true)
	await _frames(3)
	var anisotropy_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_anisotropy_value") as SpinBox
	anisotropy_spin.value = 0.6
	anisotropy_spin.emit_signal("value_changed", 0.6)
	await _frames(3)

	var emission_toggle: CheckButton = _find_named(editor, "BiomeTextureLayerEmissionEnabled_0") as CheckButton
	if emission_toggle == null:
		_fail("Texture band is missing the emission toggle")
		return
	emission_toggle.button_pressed = true
	emission_toggle.emit_signal("toggled", true)
	await _frames(3)
	var emission_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_emission_strength") as SpinBox
	if emission_spin == null:
		_fail("Emission strength control did not appear once emission was enabled")
		return
	emission_spin.value = 3.5
	emission_spin.emit_signal("value_changed", 3.5)
	await _frames(3)

	var height_ref_picker: OptionButton = _find_named(editor, "BiomeTextureLayerHeightRelative_0") as OptionButton
	if height_ref_picker == null:
		_fail("Texture band is missing the height-reference dropdown")
		return
	height_ref_picker.select(1) # Local terrain (relative)
	height_ref_picker.emit_signal("item_selected", 1)
	await _frames(3)

	var cavity_min_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_cavity_min") as SpinBox
	var cavity_max_spin: SpinBox = _find_named(editor, "BiomeTextureLayerField_cavity_max") as SpinBox
	if cavity_min_spin == null or cavity_max_spin == null:
		_fail("Texture band is missing the cavity range controls")
		return
	cavity_min_spin.value = -0.4
	cavity_min_spin.emit_signal("value_changed", -0.4)
	await _frames(3)
	cavity_max_spin = _find_named(editor, "BiomeTextureLayerField_cavity_max") as SpinBox
	cavity_max_spin.value = 0.5
	cavity_max_spin.emit_signal("value_changed", 0.5)
	await _frames(3)

	if _find_named(editor, "BiomeTexturePreview") == null:
		_fail("Biome Texture editor is missing the height-sweep preview swatch")
		return

	var final_layers: Array = editor.call("_phase47_texture_stack", texture_graph) as Array
	var final_band: Dictionary = final_layers[0] as Dictionary
	var checks := {
		"gradient_strength": [float(final_band.get("gradient_strength", -1.0)), 0.75],
		"roughness_value": [float(final_band.get("roughness_value", -1.0)), 0.2],
		"metallic_value": [float(final_band.get("metallic_value", -1.0)), 0.9],
		"anisotropy_value": [float(final_band.get("anisotropy_value", -1.0)), 0.6],
		"emission_strength": [float(final_band.get("emission_strength", -1.0)), 3.5],
		"cavity_min": [float(final_band.get("cavity_min", 99.0)), -0.4],
		"cavity_max": [float(final_band.get("cavity_max", -99.0)), 0.5],
	}
	for key: String in checks:
		var pair: Array = checks[key]
		if not is_equal_approx(float(pair[0]), float(pair[1])):
			_fail("%s did not round-trip through the UI (got %.3f, expected %.3f)" % [key, pair[0], pair[1]])
			return
	if not bool(final_band.get("roughness_enabled", false)) \
			or not bool(final_band.get("metallic_enabled", false)) \
			or not bool(final_band.get("anisotropy_enabled", false)) \
			or not bool(final_band.get("emission_enabled", false)):
		_fail("One of the material override enable flags did not round-trip")
		return
	if not bool(final_band.get("height_relative", false)):
		_fail("Height reference mode did not round-trip to relative")
		return
	var final_gradient_curve: PackedFloat32Array = final_band.get("gradient_curve", PackedFloat32Array()) as PackedFloat32Array
	if CurveFieldData.point_count(final_gradient_curve) != 2 \
			or not is_equal_approx(CurveFieldData.get_point(final_gradient_curve, 0).y, 1.0) \
			or not is_equal_approx(CurveFieldData.get_point(final_gradient_curve, 1).y, 1.0):
		_fail("Gradient distribution curve did not round-trip through the UI (curve=%s)" % [final_gradient_curve])
		return

	# --- Full CPU-runtime pack of the final state, matching what the GPU sees. ---
	var final_rt: Node = RUNTIME.new()
	final_rt.call("compile_from_terrain", terrain)
	var final_packed: Dictionary = final_rt.call("biome_texture_uniforms") as Dictionary
	if int(final_packed.get("count", 0)) < 1:
		_fail("Final compile lost the biome texture band entirely")
		final_rt.free()
		return
	var f: PackedVector4Array = final_packed.get("f") as PackedVector4Array
	var flags: int = int(round(f[0].w))
	if (flags & 1) == 0 or (flags & 2) == 0 or (flags & 4) == 0 or (flags & 8) == 0:
		_fail("Packed enable_flags did not include all four overrides (flags=%d)" % flags)
		final_rt.free()
		return
	if (flags & 16) == 0:
		_fail("Packed enable_flags did not include the height-relative bit (flags=%d)" % flags)
		final_rt.free()
		return
	if not is_equal_approx(f[0].x, 0.2) or not is_equal_approx(f[0].y, 0.9) \
			or not is_equal_approx(f[0].z, 0.6):
		_fail("Packed roughness/metallic/anisotropy values do not match what was authored")
		final_rt.free()
		return
	var h: PackedVector4Array = final_packed.get("h") as PackedVector4Array
	if h.is_empty() or not is_equal_approx(h[0].x, -0.4) or not is_equal_approx(h[0].y, 0.5):
		_fail("Packed cavity range does not match what was authored (h[0]=%s)" % [h[0] if not h.is_empty() else "n/a"])
		final_rt.free()
		return
	if int(round(h[0].z)) != 2:
		_fail("Packed gradient curve point count does not match what was authored (h[0].z=%.1f)" % h[0].z)
		final_rt.free()
		return
	var curve_ab: PackedVector4Array = final_packed.get("curve_ab") as PackedVector4Array
	if curve_ab.is_empty() or not is_equal_approx(curve_ab[0].y, 1.0) or not is_equal_approx(curve_ab[0].w, 1.0):
		_fail("Packed gradient curve does not evaluate to a flat y=1 (curve_ab[0]=%s)"
			% [curve_ab[0] if not curve_ab.is_empty() else "n/a"])
		final_rt.free()
		return
	var d: PackedVector4Array = final_packed.get("d") as PackedVector4Array
	if int(round(d[0].w)) != 0:
		_fail("Packed custom_texture_index did not match the imported texture's index")
		final_rt.free()
		return
	final_rt.free()

	print("BIOME_TEXTURE_PBR_PHASE48_OK: gradients, material overrides, and PBR texture import round-trip through the real UI and CPU runtime")
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _find_named(root: Node, node_name: String) -> Node:
	if root == null:
		return null
	if String(root.name) == node_name:
		return root
	for child: Node in root.get_children():
		var found: Node = _find_named(child, node_name)
		if found != null:
			return found
	return null


func _find_button(root: Node, text: String) -> Button:
	if root == null:
		return null
	if root is Button and (root as Button).text == text:
		return root as Button
	for child: Node in root.get_children():
		var found: Button = _find_button(child, text)
		if found != null:
			return found
	return null


func _graph_has_node_type(graph: Resource, node_type: String) -> bool:
	if graph == null:
		return false
	for value: Variant in graph.get(&"nodes") as Array:
		if value is Dictionary and String((value as Dictionary).get("type", "")) == node_type:
			return true
	return false


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("BIOME_TEXTURE_PBR_PHASE48_FAILED: %s" % message)
	get_tree().quit(1)
