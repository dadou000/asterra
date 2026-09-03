extends "res://scripts/world_authoring/world_authoring_editor_live_phase44.gd"
## Phase 45: one-click terrain templates for the simplified Local Features workflow.
##
## Presets are creation recipes only. Each generated item is still an ordinary
## full-LOD guided displacement slot, editable through Simple or the exact Node Graph.

const PHASE45_RAW_DISPLACEMENT_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase41.gd")
const PHASE45_FEATURE_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase43.gd")
const PHASE45_SURFACE_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase44.gd")
const PHASE45_GUIDED := preload(
	"res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const TERRAIN_PRESETS := preload(
	"res://scripts/world_authoring/model/terrain_feature_preset_catalog.gd")


func _phase28_focus_existing_slot(slot: Resource) -> void:
	# Phase 28/41 tooling and saved editor actions use this API to request a concrete
	# graph. Phase 45 opens Terrain in Simple mode, so make that explicit raw-graph
	# request a compatibility bridge into Advanced rather than leaving the requested
	# graph hidden behind the simplified presentation.
	_phase43_advanced_terrain = true
	super._phase28_focus_existing_slot(slot)


func _phase43_build_local_features(terrain: Resource) -> void:
	_section("Local Features")
	_add_note("Start from a preset or add a blank feature. Presets create the same editable WHAT × WHERE features shown below; they do not introduce special runtime terrain types.")
	_phase45_build_preset_shelf(terrain)

	var features: Array[Resource] = _phase43_feature_slots(terrain)
	_phase43_build_distance_and_capacity_note(terrain, features)
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
	var add_budget: Dictionary = _phase43_feature_budget(terrain, [PHASE45_GUIDED.default_config()])
	add.disabled = not bool(add_budget.get("fits", false))
	add.tooltip_text = "Create one feature that automatically applies to all terrain distance layers." if not add.disabled \
		else String(add_budget.get("message", "This terrain program is full."))
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
	var editor := PHASE45_FEATURE_EDITOR.new()
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
		var preset_budget: Dictionary = _phase43_feature_budget(terrain, _phase45_preset_configs(preset_id))
		button.disabled = not bool(preset_budget.get("fits", false))
		button.tooltip_text = TERRAIN_PRESETS.description(preset_id) if not button.disabled \
			else String(preset_budget.get("message", "This terrain program is full."))
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
	var budget: Dictionary = _phase43_feature_budget(terrain, _phase45_preset_configs(preset_id, salt))
	if not bool(budget.get("fits", false)):
		_set_status(String(budget.get("message", "This terrain program is full. Disable or delete a feature first.")))
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


func _phase45_preset_configs(preset_id: String, seed_salt: int = 0) -> Array[Dictionary]:
	var configs: Array[Dictionary] = []
	for spec: Dictionary in TERRAIN_PRESETS.specs(preset_id, seed_salt):
		var config: Variant = spec.get("config", {})
		if config is Dictionary:
			configs.append(config as Dictionary)
	return configs


func _phase29_build_graph_editor(slot: Resource) -> void:
	# Advanced is a raw-document view, but each document should still use the editor
	# phase that owns its semantics. Do not wrap everything in the newest subclass:
	# that obscures the Phase 41 spatial-mask boundary and makes old raw-graph tools
	# unable to identify the editor contract they intentionally requested.
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	var graph_editor: Control
	if slot != null and int(slot.get(&"domain")) == SHADER_SLOT_MODEL.Domain.MATERIAL:
		graph_editor = PHASE45_SURFACE_EDITOR.new() as Control
	elif graph != null and PHASE45_GUIDED.is_guided_graph(graph):
		graph_editor = PHASE45_FEATURE_EDITOR.new() as Control
	else:
		graph_editor = PHASE45_RAW_DISPLACEMENT_EDITOR.new() as Control

	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if slot != null and int(slot.get(&"domain")) == SHADER_SLOT_MODEL.Domain.MATERIAL:
		hint.text = "Advanced Surface exposes the complete production PBR outputs, classifier thresholds, palette/materials, microrelief, anti-tiling, procedural rock PBR, scanned PBR, textures, world fields and graph math."
	elif graph != null and PHASE45_GUIDED.is_guided_graph(graph):
		hint.text = "This guided feature is still an ordinary displacement graph. Simple and Node Graph edit the same document; custom routing can be added here without introducing a preset-only runtime."
	else:
		hint.text = "Raw displacement graphs use the Phase 41 spatial editor contract directly. Base Terrain retains the safe native Phase 40 lowering boundary; custom displacement graphs retain Latitude, Longitude, Region, Radial and Ring masks with identical render/contact bytecode."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
