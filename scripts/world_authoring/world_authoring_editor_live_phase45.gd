extends "res://scripts/world_authoring/world_authoring_editor_live_phase44.gd"
## Phase 45: one-click terrain templates for the simplified Local Features workflow.
##
## Presets are creation recipes only. Each generated item is still an ordinary
## full-LOD guided displacement slot, editable through Simple or the exact Node Graph.

const PHASE45_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase44.gd")
const PHASE45_GUIDED := preload(
	"res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const TERRAIN_PRESETS := preload(
	"res://scripts/world_authoring/model/terrain_feature_preset_catalog.gd")


func _phase43_build_local_features(terrain: Resource) -> void:
	_section("Local Features")
	_add_note("Start from a preset or add a blank feature. Presets create the same editable WHAT × WHERE features shown below; they do not introduce special runtime terrain types.")
	_phase45_build_preset_shelf(terrain)

	var features: Array[Resource] = _phase43_feature_slots(terrain)
	var header := HBoxContainer.new()
	header.name = "LocalFeatureHeader"
	header.add_theme_constant_override("separation", 8)
	_workspace.add_child(header)
	var count := Label.new()
	count.text = "%d feature%s" % [features.size(), "" if features.size() == 1 else "s"]
	count.add_theme_font_size_override("font_size", 16)
	count.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(count)
	var add := Button.new()
	add.name = "AddBlankTerrainFeature"
	add.text = "+ Add Terrain Feature"
	add.custom_minimum_size = Vector2(205.0, 42.0)
	add.tooltip_text = "Create one blank full-LOD WHAT × WHERE displacement feature"
	add.pressed.connect(_phase43_create_feature.bind(terrain))
	header.add_child(add)

	if features.is_empty():
		var empty := PanelContainer.new()
		empty.custom_minimum_size = Vector2(760.0, 115.0)
		_workspace.add_child(empty)
		var empty_box := VBoxContainer.new()
		empty.add_child(empty_box)
		var empty_title := Label.new()
		empty_title.text = "No local terrain features yet"
		empty_title.add_theme_font_size_override("font_size", 17)
		empty_box.add_child(empty_title)
		var empty_note := Label.new()
		empty_note.text = "Choose a preset above or add a blank feature. Every created feature can be moved, reshaped, renamed, disabled or opened as nodes."
		empty_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty_note.modulate = Color(0.62, 0.72, 0.80)
		empty_box.add_child(empty_note)
		return

	var selected: Resource = terrain.call("find_shader_slot", _phase43_selected_feature_id) as Resource
	if selected == null or not features.has(selected):
		selected = features[0]
		_phase43_selected_feature_id = String(selected.get(&"slot_id"))

	for slot: Resource in features:
		_phase43_build_feature_row(terrain, slot, slot == selected)

	var separator := HSeparator.new()
	_workspace.add_child(separator)
	_section("Edit Feature")
	_phase43_build_feature_header(selected)
	var editor := PHASE45_GRAPH_EDITOR.new()
	editor.name = "SimpleLocalFeatureEditor"
	editor.custom_minimum_size = Vector2(1120.0, 700.0)
	_workspace.add_child(editor)
	editor.call_deferred("setup", _session, selected,
		Callable(self, "_refresh_current_category"))


func _phase45_build_preset_shelf(terrain: Resource) -> void:
	var shelf := PanelContainer.new()
	shelf.name = "TerrainPresetShelf"
	shelf.custom_minimum_size = Vector2(900.0, 245.0)
	_workspace.add_child(shelf)
	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", 7)
	shelf.add_child(outer)

	var title := Label.new()
	title.text = "START FROM A TERRAIN PRESET"
	title.add_theme_font_size_override("font_size", 16)
	outer.add_child(title)
	var intro := Label.new()
	intro.text = "One click creates a useful starting graph. Multi-part presets such as Crater and Island create several normal features so each part stays independently editable."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.modulate = Color(0.62, 0.72, 0.80)
	outer.add_child(intro)

	var grid := GridContainer.new()
	grid.name = "TerrainPresetGrid"
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 8)
	grid.add_theme_constant_override("v_separation", 8)
	outer.add_child(grid)
	for preset_id: String in TERRAIN_PRESETS.IDS:
		var card := VBoxContainer.new()
		card.custom_minimum_size = Vector2(285.0, 78.0)
		card.add_theme_constant_override("separation", 2)
		grid.add_child(card)
		var button := Button.new()
		button.name = "Preset_%s" % preset_id
		var count: int = TERRAIN_PRESETS.feature_count(preset_id)
		button.text = "+ %s%s" % [
			TERRAIN_PRESETS.label(preset_id),
			"  ·  %d parts" % count if count > 1 else "",
		]
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.custom_minimum_size = Vector2(280.0, 34.0)
		button.tooltip_text = TERRAIN_PRESETS.description(preset_id)
		button.pressed.connect(_phase45_create_preset.bind(terrain, preset_id))
		card.add_child(button)
		var description := Label.new()
		description.text = TERRAIN_PRESETS.description(preset_id)
		description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		description.modulate = Color(0.55, 0.65, 0.73)
		description.custom_minimum_size.x = 275.0
		card.add_child(description)


func _phase45_create_preset(terrain: Resource, preset_id: String) -> void:
	if terrain == null or _session == null or not TERRAIN_PRESETS.IDS.has(preset_id):
		return
	var salt: int = _phase43_feature_slots(terrain).size() + 1
	var specs: Array[Dictionary] = TERRAIN_PRESETS.specs(preset_id, salt)
	if specs.is_empty():
		return
	var created_box: Array[Resource] = []
	_session.stage_action("Create %s terrain preset" % TERRAIN_PRESETS.label(preset_id), func() -> void:
		for spec: Dictionary in specs:
			var display_name: String = String(spec.get("name", TERRAIN_PRESETS.label(preset_id)))
			var created: Resource = terrain.call("create_shader_slot",
				SHADER_SLOT_MODEL.Domain.DISPLACEMENT, display_name) as Resource
			if created == null:
				continue
			created.set(&"enabled", true)
			created.set(&"clipmap_level_mask", PHASE43_ALL_LEVELS_MASK)
			created.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ALL)
			created.set(&"biome_ids", PackedInt32Array())
			created.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD)
			created.set(&"strength", 1.0)
			var graph: Resource = created.get(&"graph") as Resource
			var config: Dictionary = spec.get("config", {}) as Dictionary
			if graph == null or not PHASE45_GUIDED.rebuild(graph, config):
				terrain.call("remove_shader_slot", String(created.get(&"slot_id")))
				continue
			created_box.append(created)
	, WorldAuthoringSession.ApplyScope.GRAPH)

	if not created_box.is_empty():
		_phase43_selected_feature_id = String(created_box[0].get(&"slot_id"))
	_refresh_current_category()
