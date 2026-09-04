extends Node
## Regression: the public Terrain tab is a no-code editor for the production
## geomorph graph, and no longer exposes the retired SHADERS/node-canvas route.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase42.gd")


class DummyWorld extends Node3D:
	var player: Node = null


class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase47_simple_terrain_%d.tres" % OS.get_process_id())
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
	await _frames(5)

	if _find_named(editor, "TerrainAllRingsNotice") == null:
		_fail("Terrain tab does not explain all-ring application")
		return
	if _find_button(editor, "SHADERS") != null:
		_fail("retired SHADERS navigation is still visible")
		return
	if _first_graph_edit(editor) != null:
		_fail("Terrain tab still exposes a node graph canvas")
		return
	var mountain: SpinBox = _find_named(editor, "TerrainControl_mountain_strength") as SpinBox
	if mountain == null:
		_fail("Terrain tab is missing Mountain Amount")
		return
	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	var slot: Resource = terrain.call("find_shader_slot", NATIVE.PRODUCTION_SHAPE_SLOT_ID) as Resource if terrain != null else null
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		_fail("Terrain tab has no production graph")
		return
	mountain.emit_signal("value_changed", 1.73)
	await _frames(2)
	if not is_equal_approx(float(NATIVE.extract_controls(graph).get("mountain_strength", 0.0)), 1.73):
		_fail("Mountain Amount did not update the production graph")
		return
	var desert: Button = _find_named(editor, "TerrainLook_desert") as Button
	if desert == null:
		_fail("Terrain look presets are missing")
		return
	desert.emit_signal("pressed")
	await _frames(3)
	var controls: Dictionary = NATIVE.extract_controls(graph)
	if float(controls.get("dune_strength", 0.0)) < 1.8 or float(controls.get("glacial_strength", 1.0)) != 0.0:
		_fail("Desert look did not update production controls")
		return

	# A biome profile is intentionally an ordinary, scoped displacement slot. This
	# proves it has the exact all-ring and CPU/GPU bytecode path of every other
	# terrain edit rather than being a UI-only preview setting.
	var biome_tab: Button = _find_button(editor, "BIOME TERRAIN")
	if biome_tab == null:
		_fail("Terrain tab is missing the Biome Terrain editor")
		return
	biome_tab.emit_signal("pressed")
	await _frames(3)
	if _find_named(editor, "PickBiomeUnderCursor") == null:
		_fail("Biome Terrain editor is missing Pick Biome Under Cursor")
		return
	var biome_picker: OptionButton = _find_named(editor, "BiomeTerrainPicker") as OptionButton
	if biome_picker == null:
		_fail("Biome Terrain editor is missing biome selection")
		return
	# Hot desert is a useful regression target because it is distinct from the
	# default biome and makes the selected-only mask easy to assert.
	biome_picker.select(11)
	biome_picker.emit_signal("item_selected", 11)
	await _frames(3)
	# The scoped slot is provisioned on selection (no create step) but starts
	# EMPTY -- a biome has no terrain character of its own until you compose one.
	if _find_named(editor, "CreateBiomeTerrainProfile") != null:
		_fail("Biome Terrain editor still requires a manual create step")
		return
	var biome_slot: Resource = terrain.call("find_shader_slot", "simple-biome-terrain-11") as Resource
	if biome_slot == null:
		_fail("Biome Terrain editor did not auto-provision its scoped terrain slot")
		return
	if int(biome_slot.get(&"clipmap_level_mask")) != ((1 << 15) - 1) \
			or int(biome_slot.get(&"biome_mask_mode")) != 1:
		_fail("Biome terrain profile is not constrained to the selected biome on every ring")
		return
	var fresh_layers: Array = (editor.call("_phase47_biome_stack",
		biome_slot.get(&"graph")) as Dictionary).get("layers", []) as Array
	if not fresh_layers.is_empty():
		_fail("Freshly provisioned biome profile is not empty")
		return
	if _find_named(editor, "BiomeTerrainLayerType_0") != null:
		_fail("Empty biome profile still renders layer cards")
		return
	# Opt in to the ported character, then compose on top of it.
	var load_default: Button = _find_named(editor, "ResetBiomeTerrainProfile") as Button
	if load_default == null:
		_fail("Biome terrain editor has no ported-character button")
		return
	load_default.emit_signal("pressed")
	await _frames(3)
	var biome_seed_graph: Resource = biome_slot.get(&"graph") as Resource
	if not _graph_has_node_type(biome_seed_graph, "SEDIMENT_DEPOSIT"):
		_fail("Loading the Hot desert ported character did not add its sedimentation pass")
		return
	# The profile is an ordered stack of layer cards, not a single shape dropdown.
	var layer0: OptionButton = _find_named(editor, "BiomeTerrainLayerType_0") as OptionButton
	if layer0 == null:
		_fail("Biome terrain profile has no layer-type picker")
		return
	layer0.select(2) # Terraced relief
	layer0.emit_signal("item_selected", 2)
	await _frames(3)
	var biome_graph: Resource = biome_slot.get(&"graph") as Resource
	if not _graph_has_node_type(biome_graph, "TERRACE_RELIEF"):
		_fail("Terraced Relief did not serialize into the terrain graph")
		return
	# Its sediment layer must survive the first layer changing type.
	if not _graph_has_node_type(biome_graph, "SEDIMENT_DEPOSIT"):
		_fail("Changing a layer type dropped the rest of the stack")
		return
	# Add a second layer and prove the stack grows.
	var add_layer: Button = _find_named(editor, "AddBiomeTerrainLayer") as Button
	if add_layer == null:
		_fail("Biome terrain stack has no Add layer button")
		return
	add_layer.emit_signal("pressed")
	await _frames(3)
	var stack_after: Dictionary = editor.call("_phase47_biome_stack",
		biome_slot.get(&"graph")) as Dictionary
	if int((stack_after.get("layers", []) as Array).size()) < 3:
		_fail("Add layer did not extend the biome terrain stack")
		return
	# The km biome-blend control must serialize onto the output node.
	var blend_km: SpinBox = _find_named(editor, "BiomeTerrainBlendKm") as SpinBox
	if blend_km == null:
		_fail("Biome terrain profile has no kilometre blend control")
		return
	blend_km.value = 7.5
	blend_km.emit_signal("value_changed", 7.5)
	await _frames(3)
	if not is_equal_approx(float((editor.call("_phase47_biome_stack",
			biome_slot.get(&"graph")) as Dictionary).get("blend_km", 0.0)), 7.5):
		_fail("Biome blend distance did not serialize")
		return
	# Back to a hard edge so the CPU-mirror check below does not depend on a live
	# biome field for the blend kernel's neighbour taps (absent in this headless
	# harness). The panel rebuilt after the last edit, so re-find the control.
	var blend_km2: SpinBox = _find_named(editor, "BiomeTerrainBlendKm") as SpinBox
	if blend_km2 == null:
		_fail("Biome blend control missing after panel refresh")
		return
	blend_km2.value = 0.0
	blend_km2.emit_signal("value_changed", 0.0)
	await _frames(3)
	# A biome profile is deliberately kept OUT of the displacement bytecode VM: a
	# compiled program makes the renderer swap in the authored-terrain shader
	# variant, whose 32-slot register array triggers Vulkan device loss on current
	# NVIDIA drivers. Instead it lowers to plain uniforms
	# (shaders/terrain_biome_profile.gdshaderinc) that are mirrored on the CPU for
	# contact/AGL, so render and physics still share one definition.
	var runtime: Node = RUNTIME.new()
	add_child(runtime)
	runtime.call("compile_from_terrain", terrain)
	if not _runtime_has_opcode(runtime, 29):
		pass # OP_TERRACE_RELIEF must NOT be in the VM for a biome profile.
	else:
		_fail("Biome profile leaked into the displacement bytecode VM")
		return
	if int(runtime.call("biome_profile_count")) < 1:
		_fail("Biome profile did not lower to the dedicated uniform path")
		return
	var packed: Dictionary = runtime.call("biome_profile_uniforms") as Dictionary
	var target_dir: Vector3 = Vector3(0.31, 0.62, -0.72).normalized()
	if is_zero_approx(float(runtime.call("_biome_profiles_height", target_dir, 11))):
		_fail("Biome profile CPU mirror produced no displacement for its biome")
		return
	if not is_zero_approx(float(runtime.call("_biome_profiles_height", target_dir, 4))):
		_fail("Biome profile displaced a biome it is not masked to")
		return
	runtime.queue_free()

	print("SIMPLE_TERRAIN_TAB_PHASE47_OK: Terrain controls update production geomorph, all rings, no shader/node UI")
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


func _first_graph_edit(root: Node) -> GraphEdit:
	if root is GraphEdit:
		return root as GraphEdit
	for child: Node in root.get_children():
		var found: GraphEdit = _first_graph_edit(child)
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


func _runtime_has_opcode(runtime: Node, opcode: int) -> bool:
	var headers: PackedVector4Array = runtime.get("_headers") as PackedVector4Array
	for header: Vector4 in headers:
		if int(round(header.x)) == opcode:
			return true
	return false


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("SIMPLE_TERRAIN_TAB_PHASE47_FAILED: %s" % message)
	get_tree().quit(1)
