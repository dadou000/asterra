extends Node
## Phase 45 UI regression through the actual PlanetStudio.tscn preset shelf.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const PRESETS := preload("res://scripts/world_authoring/model/terrain_feature_preset_catalog.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase45_preset_ui_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)
	var editor: Control = PLANET_STUDIO_SCENE.instantiate() as Control
	if editor == null:
		_fail("PlanetStudio.tscn failed to instantiate")
		return
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "SHADERS")
	await _frames(6)

	if _find_button(editor, "BASE LANDSCAPE") == null \
			or _find_button(editor, "LOCAL FEATURES") == null \
			or _find_button(editor, "SURFACE") == null:
		_fail("Phase 45 lost the complete simplified Terrain tabs")
		return
	var local_tab: Button = _find_button(editor, "LOCAL FEATURES")
	local_tab.emit_signal("pressed")
	await _frames(6)

	var shelf: Node = _find_named(editor, "TerrainPresetShelf")
	if shelf == null or _find_named(shelf, "TerrainPresetGrid") == null:
		_fail("Local Features has no terrain preset shelf/grid")
		return
	for preset_id: String in PRESETS.IDS:
		if _find_named(shelf, "Preset_%s" % preset_id) == null:
			_fail("preset shelf is missing %s" % preset_id)
			return
	if _find_named(editor, "AddBlankTerrainFeature") == null:
		_fail("preset UI removed the blank-feature path")
		return

	# Click the real crater card. It must create two normal full-LOD guided features.
	var crater_button: Button = _find_named(shelf, "Preset_%s" % PRESETS.CRATER) as Button
	if crater_button == null or not crater_button.text.contains("2 parts"):
		_fail("Impact Crater card does not advertise its editable multi-part recipe")
		return
	crater_button.emit_signal("pressed")
	await _frames(8)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Planet Studio has no active terrain profile")
		return
	var features: Array[Resource] = _features(terrain)
	if features.size() != 2:
		_fail("Impact Crater did not create exactly two ordinary terrain features")
		return
	var basin: Resource = _feature_named(features, "Crater Basin")
	var rim: Resource = _feature_named(features, "Crater Rim")
	if basin == null or rim == null:
		_fail("Impact Crater did not expose Basin and Rim as independent features")
		return
	for slot: Resource in [basin, rim]:
		if int(slot.get(&"clipmap_level_mask")) != ((1 << 15) - 1):
			_fail("preset feature is not full-LOD")
			return
		var graph: Resource = slot.get(&"graph") as Resource
		if not GUIDED.is_guided_graph(graph):
			_fail("preset feature is not an ordinary guided graph")
			return
	var basin_config: Dictionary = GUIDED.config_from_graph(basin.get(&"graph") as Resource)
	var rim_config: Dictionary = GUIDED.config_from_graph(rim.get(&"graph") as Resource)
	if String(basin_config.get("effect_kind", "")) != GUIDED.EFFECT_HEIGHT \
			or String(basin_config.get("area_kind", "")) != GUIDED.AREA_RADIAL \
			or float(basin_config.get("amount_m", 0.0)) >= 0.0:
		_fail("Crater Basin preset config is not a negative radial height feature")
		return
	if String(rim_config.get("area_kind", "")) != GUIDED.AREA_RING \
			or float(rim_config.get("amount_m", 0.0)) <= 0.0:
		_fail("Crater Rim preset config is not a positive Ring Area feature")
		return

	# The first generated part is selected and must be editable using the same Simple
	# controls as a blank feature. Change Height change and inspect the serialized graph.
	var simple_editor: Node = _find_named(editor, "SimpleLocalFeatureEditor")
	var feature_panel: Node = _find_named(simple_editor, "GuidedTerrainFeatureEditor")
	var amount: SpinBox = _spin_for_label(feature_panel, "Height change")
	if amount == null:
		_fail("preset-created Crater Basin did not open in Simple feature editing")
		return
	amount.emit_signal("value_changed", -480.0)
	await _frames(3)
	basin_config = GUIDED.config_from_graph(basin.get(&"graph") as Resource)
	if not is_equal_approx(float(basin_config.get("amount_m", 0.0)), -480.0):
		_fail("editing a preset-created feature did not update its ordinary graph")
		return

	# A second multi-part preset must append normal features, not replace or hide the
	# existing crater stack.
	shelf = _find_named(editor, "TerrainPresetShelf")
	var island_button: Button = _find_named(shelf, "Preset_%s" % PRESETS.ISLAND) as Button
	if island_button == null:
		_fail("Island preset button disappeared after creating a crater")
		return
	island_button.emit_signal("pressed")
	await _frames(8)
	features = _features(terrain)
	if features.size() != 4 \
			or _feature_named(features, "Island Landmass") == null \
			or _feature_named(features, "Island Interior Relief") == null:
		_fail("Island preset did not append its two editable features")
		return

	# Blank creation remains available beside presets.
	var blank: Button = _find_named(editor, "AddBlankTerrainFeature") as Button
	blank.emit_signal("pressed")
	await _frames(7)
	features = _features(terrain)
	if features.size() != 5:
		_fail("Blank Feature path no longer works beside presets")
		return

	print("TERRAIN_PRESETS_PHASE45_UI_OK: preset shelf, multi-part crater/island creation, full-LOD guided graphs, Simple editing and blank creation match")
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _features(terrain: Resource) -> Array[Resource]:
	var out: Array[Resource] = []
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = value as Resource
		if slot != null and String(slot.get(&"slot_id")) != NATIVE.PRODUCTION_SHAPE_SLOT_ID:
			out.append(slot)
	return out


func _feature_named(features: Array[Resource], display_name: String) -> Resource:
	for slot: Resource in features:
		if String(slot.get(&"display_name")) == display_name:
			return slot
	return null


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


func _spin_for_label(root: Node, label_text: String) -> SpinBox:
	if root == null:
		return null
	for child: Node in root.get_children():
		if child is HBoxContainer:
			var matched: bool = false
			var spin: SpinBox = null
			for row_child: Node in child.get_children():
				if row_child is Label and (row_child as Label).text == label_text:
					matched = true
				elif row_child is SpinBox:
					spin = row_child as SpinBox
			if matched and spin != null:
				return spin
		var nested: SpinBox = _spin_for_label(child, label_text)
		if nested != null:
			return nested
	return null


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("TERRAIN_PRESETS_PHASE45_UI_FAILED: %s" % message)
	get_tree().quit(1)
