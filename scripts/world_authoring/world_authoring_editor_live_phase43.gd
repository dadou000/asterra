extends "res://scripts/world_authoring/world_authoring_editor_live_phase41.gd"
## Phase 43: default Planet Studio terrain workflow for non-technical authoring.
##
## The mature LOD/scope/node composer remains available under Advanced. The default
## page instead presents Base Landscape and Local Features. Both routes edit the same
## terrain profile and the same serialized graph resources.

const PHASE43_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase43.gd")
const GUIDED_FEATURE := preload(
	"res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")

const PHASE43_ALL_LEVELS_MASK: int = (1 << 15) - 1
const PHASE43_DISTANCE_LAYER_COUNT: int = 15
const SIMPLE_TAB_BASE: int = 0
const SIMPLE_TAB_FEATURES: int = 1

var _phase43_advanced_terrain: bool = false
var _phase43_simple_tab: int = SIMPLE_TAB_BASE
var _phase43_selected_feature_id: String = ""


func _build_terrain_shader_composer_page() -> void:
	if _phase43_advanced_terrain:
		super._build_terrain_shader_composer_page()
		_phase43_add_mode_bar(true)
		return

	_page_title("Terrain",
		"Shape the planet directly. Base Landscape changes the global terrain character; Local Features add mountains, cuts, deposits or height changes only where you choose.")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return

	_phase29_ensure_production_graphs(terrain)
	_phase43_add_mode_bar(false)
	_phase43_build_simple_tabs()
	if _phase43_simple_tab == SIMPLE_TAB_FEATURES:
		_phase43_build_local_features(terrain)
	else:
		_phase43_build_base_landscape(terrain)


func _phase43_add_mode_bar(advanced: bool) -> void:
	var bar := HBoxContainer.new()
	bar.name = "TerrainAuthoringModeBar"
	bar.add_theme_constant_override("separation", 8)
	var simple := Button.new()
	simple.text = "Simple"
	simple.toggle_mode = true
	simple.button_pressed = not advanced
	simple.custom_minimum_size = Vector2(130.0, 38.0)
	simple.pressed.connect(func() -> void:
		_phase43_advanced_terrain = false
		_refresh_current_category()
	)
	bar.add_child(simple)
	var advanced_button := Button.new()
	advanced_button.text = "Advanced / Node System"
	advanced_button.toggle_mode = true
	advanced_button.button_pressed = advanced
	advanced_button.custom_minimum_size = Vector2(220.0, 38.0)
	advanced_button.pressed.connect(func() -> void:
		_phase43_advanced_terrain = true
		_refresh_current_category()
	)
	bar.add_child(advanced_button)
	var explanation := Label.new()
	explanation.text = "Simple hides LOD, graph routing and scope internals. Advanced exposes the existing authoritative composer unchanged."
	explanation.modulate = Color(0.58, 0.69, 0.78)
	explanation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	bar.add_child(explanation)
	_workspace.add_child(bar)
	if advanced:
		_workspace.move_child(bar, 0)


func _phase43_build_simple_tabs() -> void:
	var tabs := HBoxContainer.new()
	tabs.add_theme_constant_override("separation", 6)
	tabs.custom_minimum_size.y = 48.0
	_workspace.add_child(tabs)
	for item: Dictionary in [
		{"label":"BASE LANDSCAPE", "tab":SIMPLE_TAB_BASE,
			"tip":"Global mountains, valleys, broad shape and fine detail."},
		{"label":"LOCAL FEATURES", "tab":SIMPLE_TAB_FEATURES,
			"tip":"Add a terrain effect and choose exactly where it applies."},
	]:
		var button := Button.new()
		button.text = String(item["label"])
		button.toggle_mode = true
		button.button_pressed = _phase43_simple_tab == int(item["tab"])
		button.custom_minimum_size = Vector2(260.0, 44.0)
		button.tooltip_text = String(item["tip"])
		var tab_value: int = int(item["tab"])
		button.pressed.connect(func() -> void:
			_phase43_simple_tab = tab_value
			_refresh_current_category()
		)
		tabs.add_child(button)


func _phase43_build_base_landscape(terrain: Resource) -> void:
	_section("Base Landscape")
	_add_note("These controls change the production terrain everywhere. They are the same values used by the production geomorph graph; Simple mode is only a clearer presentation of them.")
	var base_slot: Resource = terrain.call("find_shader_slot", PRODUCTION_SHAPE_SLOT_ID) as Resource
	if base_slot == null:
		_add_note("Base Terrain is missing from the active terrain profile.")
		return
	var graph_editor := PHASE43_GRAPH_EDITOR.new()
	graph_editor.name = "SimpleBaseLandscapeEditor"
	graph_editor.custom_minimum_size = Vector2(1120.0, 720.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, base_slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.text = "Use Local Features for geographic placement. Base Landscape intentionally stays global until native stage spatial provenance is identical through render, cache and contact."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)


func _phase43_build_local_features(terrain: Resource) -> void:
	_section("Local Features")
	_add_note("A feature is simply WHAT happens × WHERE it happens. Every Simple feature automatically works from close-up detail to the far horizon — no terrain-layer setup needed.")

	var features: Array[Resource] = _phase43_feature_slots(terrain)
	_phase43_build_distance_and_capacity_note(terrain, features)
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 8)
	_workspace.add_child(header)
	var count := Label.new()
	count.text = "%d feature%s" % [features.size(), "" if features.size() == 1 else "s"]
	count.add_theme_font_size_override("font_size", 16)
	count.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(count)
	var add := Button.new()
	add.text = "+ Add Terrain Feature"
	add.custom_minimum_size = Vector2(220.0, 42.0)
	var add_budget: Dictionary = _phase43_feature_budget(terrain, [GUIDED_FEATURE.default_config()])
	add.disabled = not bool(add_budget.get("fits", false))
	add.tooltip_text = "Create one feature that automatically applies to all %d terrain distance layers." % PHASE43_DISTANCE_LAYER_COUNT if not add.disabled \
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
		empty_note.text = "Add one to raise/lower terrain, add roughness or mountains, carve channels, or deposit material inside a geographic area."
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
	var editor := PHASE43_GRAPH_EDITOR.new()
	editor.name = "SimpleLocalFeatureEditor"
	editor.custom_minimum_size = Vector2(1120.0, 700.0)
	_workspace.add_child(editor)
	editor.call_deferred("setup", _session, selected,
		Callable(self, "_refresh_current_category"))


func _phase43_feature_slots(terrain: Resource) -> Array[Resource]:
	var out: Array[Resource] = []
	if terrain == null:
		return out
	for slot_value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or String(slot.get(&"slot_id")) == PRODUCTION_SHAPE_SLOT_ID:
			continue
		out.append(slot)
	return out


func _phase43_build_feature_row(terrain: Resource, slot: Resource, selected: bool) -> void:
	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(820.0, 58.0)
	_workspace.add_child(panel)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	panel.add_child(row)

	var enabled := CheckButton.new()
	enabled.text = "On"
	enabled.button_pressed = bool(slot.get(&"enabled"))
	enabled.toggled.connect(func(value: bool) -> void:
		if value and not _phase43_can_enable_feature(terrain, slot):
			enabled.set_pressed_no_signal(false)
			return
		_session.stage_set(slot, &"enabled", value,
			WorldAuthoringSession.ApplyScope.GRAPH, "Toggle terrain feature")
	)
	row.add_child(enabled)

	var name := Label.new()
	name.text = String(slot.get(&"display_name"))
	name.custom_minimum_size.x = 240.0
	name.add_theme_font_size_override("font_size", 15)
	row.add_child(name)

	var graph: Resource = slot.get(&"graph") as Resource
	var summary := Label.new()
	summary.text = GUIDED_FEATURE.summary(graph)
	summary.custom_minimum_size.x = 300.0
	summary.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	summary.modulate = Color(0.62, 0.72, 0.80)
	row.add_child(summary)

	if not GUIDED_FEATURE.is_guided_graph(graph):
		var custom := Label.new()
		custom.text = "ADVANCED"
		custom.tooltip_text = "This graph was custom-authored and will not be rewritten by Simple mode."
		custom.modulate = Color(0.78, 0.68, 0.44)
		row.add_child(custom)

	var edit := Button.new()
	edit.text = "Editing" if selected else "Edit"
	edit.disabled = selected
	edit.pressed.connect(func() -> void:
		_phase43_selected_feature_id = String(slot.get(&"slot_id"))
		_refresh_current_category()
	)
	row.add_child(edit)

	var remove := Button.new()
	remove.text = "Delete"
	remove.pressed.connect(func() -> void:
		_session.remove_terrain_shader_slot(String(slot.get(&"slot_id")))
		if _phase43_selected_feature_id == String(slot.get(&"slot_id")):
			_phase43_selected_feature_id = ""
		_refresh_current_category()
	)
	row.add_child(remove)


func _phase43_build_feature_header(slot: Resource) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	_workspace.add_child(row)
	var name_label := Label.new()
	name_label.text = "Name"
	name_label.custom_minimum_size.x = 65.0
	row.add_child(name_label)
	var name := LineEdit.new()
	name.text = String(slot.get(&"display_name"))
	name.custom_minimum_size.x = 320.0
	name.text_submitted.connect(func(value: String) -> void:
		var resolved: String = value.strip_edges()
		if resolved.is_empty():
			resolved = "Terrain Feature"
		_session.stage_set(slot, &"display_name", resolved,
			WorldAuthoringSession.ApplyScope.GRAPH, "Rename terrain feature")
		_refresh_current_category()
	)
	row.add_child(name)
	var strength_label := Label.new()
	strength_label.text = "Overall strength"
	row.add_child(strength_label)
	var strength := SpinBox.new()
	strength.min_value = -10.0
	strength.max_value = 10.0
	strength.step = 0.01
	strength.value = float(slot.get(&"strength"))
	strength.custom_minimum_size.x = 110.0
	strength.value_changed.connect(func(value: float) -> void:
		_session.stage_set(slot, &"strength", value,
			WorldAuthoringSession.ApplyScope.GRAPH, "Change terrain feature strength")
	)
	row.add_child(strength)


func _phase43_create_feature(terrain: Resource) -> void:
	if terrain == null or _session == null:
		return
	var budget: Dictionary = _phase43_feature_budget(terrain, [GUIDED_FEATURE.default_config()])
	if not bool(budget.get("fits", false)):
		_set_status(String(budget.get("message", "This terrain program is full. Disable or remove a feature first.")))
		return
	var created_box: Array[Resource] = [null]
	_session.stage_action("Create simplified terrain feature", func() -> void:
		var index: int = _phase43_feature_slots(terrain).size() + 1
		created_box[0] = terrain.call("create_shader_slot",
			SHADER_SLOT_MODEL.Domain.DISPLACEMENT, "Terrain Feature %d" % index) as Resource
		var created: Resource = created_box[0]
		if created == null:
			return
		created.set(&"enabled", true)
		created.set(&"clipmap_level_mask", PHASE43_ALL_LEVELS_MASK)
		created.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ALL)
		created.set(&"biome_ids", PackedInt32Array())
		created.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD)
		created.set(&"strength", 1.0)
		var graph: Resource = created.get(&"graph") as Resource
		if graph != null:
			GUIDED_FEATURE.rebuild(graph, GUIDED_FEATURE.default_config())
	, WorldAuthoringSession.ApplyScope.GRAPH)
	var created: Resource = created_box[0]
	if created != null:
		_phase43_selected_feature_id = String(created.get(&"slot_id"))
	_refresh_current_category()


func _phase43_build_distance_and_capacity_note(terrain: Resource,
		features: Array[Resource]) -> void:
	var note := Label.new()
	note.name = "SimpleTerrainAllDistanceNotice"
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.58, 0.72, 0.66)
	note.text = "✓ Automatically applied to all %d terrain distance layers. The near, middle and far rings use the same feature, so it stays continuous while you travel." % PHASE43_DISTANCE_LAYER_COUNT
	_workspace.add_child(note)

	var budget: Dictionary = _phase43_feature_budget(terrain)
	var capacity := Label.new()
	capacity.name = "SimpleTerrainFeatureCapacity"
	capacity.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	capacity.modulate = Color(0.70, 0.76, 0.55) if bool(budget.get("fits", false)) else Color(0.95, 0.68, 0.32)
	capacity.text = String(budget.get("message", "Feature capacity unavailable."))
	_workspace.add_child(capacity)


func _phase43_feature_budget(terrain: Resource,
		extra_configs: Array[Dictionary] = []) -> Dictionary:
	var used: int = 0
	var enabled_count: int = 0
	if terrain == null:
		return {"fits":false, "message":"Terrain feature capacity is unavailable."}
	for slot: Resource in _phase43_feature_slots(terrain):
		if slot == null or not bool(slot.get(&"enabled")):
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		var cost: int = GUIDED_FEATURE.estimated_vertex_instruction_cost(graph)
		if cost < 0:
			return {"fits":false, "message":"This terrain has a custom feature. Manage its capacity in Advanced / Node System before adding Simple features."}
		used += cost + (1 if enabled_count > 0 else 0)
		enabled_count += 1
	for config: Dictionary in extra_configs:
		var cost: int = GUIDED_FEATURE.estimated_vertex_instruction_cost_from_config(config)
		used += cost + (1 if enabled_count > 0 else 0)
		enabled_count += 1
	var maximum: int = GUIDED_FEATURE.VERTEX_PROGRAM_MAX_INSTRUCTIONS
	var fits: bool = used <= maximum
	return {
		"fits":fits,
		"used":used,
		"maximum":maximum,
		"remaining":maxi(maximum - used, 0),
		"message":"Feature capacity: %d / %d. %s" % [used, maximum,
			"You can add more." if fits else "Full — disable or delete a feature before adding another."],
	}


func _phase43_can_enable_feature(terrain: Resource, slot: Resource) -> bool:
	if slot == null:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	var cost: int = GUIDED_FEATURE.estimated_vertex_instruction_cost(graph)
	if cost < 0:
		_set_status("This is a custom graph. Enable it from Advanced / Node System, where its program capacity can be managed.")
		return false
	var config: Dictionary = GUIDED_FEATURE.config_from_graph(graph)
	var budget: Dictionary = _phase43_feature_budget(terrain, [config])
	if not bool(budget.get("fits", false)):
		_set_status(String(budget.get("message", "This terrain program is full. Disable or delete another feature first.")))
		return false
	return true


func _phase29_build_graph_editor(slot: Resource) -> void:
	# Advanced mode also receives Phase 43 so guided graphs can switch between their
	# simplified view and the exact node graph without leaving the selected layer.
	var graph_editor := PHASE43_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "Advanced mode exposes LOD, biome scope and complete Node Graph routing. Guided feature graphs remain ordinary nodes and can be inspected or extended here. Base Terrain retains its safe native-stage lowering boundary."
	else:
		hint.text = "Advanced Surface exposes the existing production PBR outputs, classifier, palette/material, microrelief, anti-tiling, rock/scan PBR, textures, world fields and graph math."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
