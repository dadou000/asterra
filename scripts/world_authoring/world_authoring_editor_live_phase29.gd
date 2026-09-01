extends "res://scripts/world_authoring/world_authoring_editor_live_phase28.gd"
## Phase 29: simple front-end, production-grade back-end.
##
## Phase 28 exposed the renderer-owned terrain as a locked preview card, then asked
## the user to create an overlay graph. Phase 29 migrates that production pass-through
## into ordinary serialized graphs and makes those graphs the default thing visible.
## Advanced L/scope/runtime diagnostics still exist, but they are no longer the
## primary workflow.

const PHASE29_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase29.gd")
const PRODUCTION_SHAPE_SLOT_ID := "production-terrain-shape"
const PRODUCTION_SURFACE_SLOT_ID := "production-terrain-surface"
const ALL_PRODUCTION_LEVELS_MASK: int = (1 << PHASE28_LEVEL_COUNT) - 1

var _phase29_show_advanced: bool = false


func _build_terrain_shader_composer_page() -> void:
	_page_title("Terrain",
		"Pick the detail level and where it applies. Then edit the terrain directly. The graph shown below is the graph that is active now.")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return

	_phase29_ensure_production_graphs(terrain)
	_phase29_build_simple_target_bar()
	_phase29_build_shape_surface_tabs()
	_phase29_build_active_graph(terrain)
	_phase29_build_advanced_toggle(terrain)


func _phase29_ensure_production_graphs(terrain: Resource) -> void:
	var created: bool = false
	if terrain.call("find_shader_slot", PRODUCTION_SHAPE_SLOT_ID) == null:
		_phase29_create_production_slot(terrain, SHADER_SLOT_MODEL.Domain.DISPLACEMENT,
			PRODUCTION_SHAPE_SLOT_ID, "Base Terrain")
		created = true
	if terrain.call("find_shader_slot", PRODUCTION_SURFACE_SLOT_ID) == null:
		_phase29_create_production_slot(terrain, SHADER_SLOT_MODEL.Domain.MATERIAL,
			PRODUCTION_SURFACE_SLOT_ID, "Base Surface")
		created = true
	if created:
		# This is a one-way structural migration of the staged profile, not a user
		# editing gesture. Mark it dirty/autosave without adding an Undo entry that
		# would simply be re-created the next time SHADERS is opened.
		_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)


func _phase29_create_production_slot(terrain: Resource, domain: int,
		slot_id: String, display_name: String) -> Resource:
	var slot: Resource = terrain.call("create_shader_slot", domain, display_name) as Resource
	if slot == null:
		return null
	slot.set(&"slot_id", slot_id)
	slot.set(&"display_name", display_name)
	slot.set(&"enabled", true)
	slot.set(&"domain", domain)
	slot.set(&"strength", 1.0)
	slot.set(&"clipmap_level_mask", ALL_PRODUCTION_LEVELS_MASK)
	slot.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ALL)
	slot.set(&"biome_ids", PackedInt32Array())
	slot.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD \
		if domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT \
		else SHADER_SLOT_MODEL.BlendMode.REPLACE)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph != null and graph.has_method("create_production_graph"):
		graph.call("create_production_graph", domain)
		graph.set(&"display_name", "%s Graph" % display_name)

	# Production is the first authored stage. Existing user graphs continue to
	# compose after it in their previous order.
	var property_name: StringName = &"displacement_slots" \
		if domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT else &"material_slots"
	var collection: Array = terrain.get(property_name) as Array
	collection.erase(slot)
	collection.push_front(slot)
	terrain.set(property_name, collection)
	return slot


func _phase29_build_simple_target_bar() -> void:
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 10)
	_workspace.add_child(bar)

	var detail_label := Label.new()
	detail_label.text = "Detail"
	detail_label.add_theme_font_size_override("font_size", 16)
	bar.add_child(detail_label)
	var level_picker := OptionButton.new()
	level_picker.custom_minimum_size.x = 225.0
	for level: int in PHASE28_LEVEL_COUNT:
		level_picker.add_item(_phase29_short_level_name(level))
		level_picker.set_item_metadata(level, level)
	level_picker.select(clampi(_shader_composer_level, 0, PHASE28_LEVEL_COUNT - 1))
	level_picker.item_selected.connect(func(index: int) -> void:
		_shader_composer_level = int(level_picker.get_item_metadata(index))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	bar.add_child(level_picker)

	var scope_label := Label.new()
	scope_label.text = "Where"
	scope_label.add_theme_font_size_override("font_size", 16)
	bar.add_child(scope_label)
	var scope_picker := OptionButton.new()
	scope_picker.custom_minimum_size.x = 155.0
	scope_picker.add_item("Everywhere")
	scope_picker.set_item_metadata(0, ScopeMode.GLOBAL)
	scope_picker.add_item("One biome")
	scope_picker.set_item_metadata(1, ScopeMode.BIOME)
	scope_picker.select(_phase28_scope_mode)
	scope_picker.item_selected.connect(func(index: int) -> void:
		_phase28_scope_mode = int(scope_picker.get_item_metadata(index))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	bar.add_child(scope_picker)

	var biome_picker := OptionButton.new()
	biome_picker.custom_minimum_size.x = 250.0
	for biome_id: int in BIOME_NAMES.size():
		biome_picker.add_item(BIOME_NAMES[biome_id])
		biome_picker.set_item_metadata(biome_id, biome_id)
	biome_picker.select(clampi(_phase28_biome_id, 0, BIOME_NAMES.size() - 1))
	biome_picker.visible = _phase28_scope_mode == ScopeMode.BIOME
	biome_picker.item_selected.connect(func(index: int) -> void:
		_phase28_biome_id = int(biome_picker.get_item_metadata(index))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	bar.add_child(biome_picker)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(spacer)
	var context := Label.new()
	context.text = "L%d · %s" % [_shader_composer_level,
		"GLOBAL" if _phase28_scope_mode == ScopeMode.GLOBAL else BIOME_NAMES[_phase28_biome_id]]
	context.modulate = Color(0.58, 0.70, 0.80)
	bar.add_child(context)


func _phase29_short_level_name(level: int) -> String:
	if level == 0:
		return "L0 — Near / finest"
	if level <= 3:
		return "L%d — Near detail" % level
	if level <= 7:
		return "L%d — Medium" % level
	if level <= 11:
		return "L%d — Far" % level
	if level == 14:
		return "L14 — Farthest / coarse"
	return "L%d — Very far" % level


func _phase29_build_shape_surface_tabs() -> void:
	var tabs := HBoxContainer.new()
	tabs.add_theme_constant_override("separation", 6)
	tabs.custom_minimum_size.y = 48.0
	_workspace.add_child(tabs)
	for item: Dictionary in [
		{"label":"SHAPE", "domain":SHADER_SLOT_MODEL.Domain.DISPLACEMENT,
			"tip":"Height, mountains, erosion and other geometry displacement."},
		{"label":"SURFACE", "domain":SHADER_SLOT_MODEL.Domain.MATERIAL,
			"tip":"Albedo, normals, roughness, metallic, AO, specular and texture logic."},
	]:
		var button := Button.new()
		button.text = String(item["label"])
		button.toggle_mode = true
		button.button_pressed = _phase28_domain == int(item["domain"])
		button.custom_minimum_size = Vector2(240.0, 44.0)
		button.tooltip_text = String(item["tip"])
		var domain_value: int = int(item["domain"])
		button.pressed.connect(func() -> void:
			_phase28_domain = domain_value
			_phase28_selected_slot_id = ""
			_refresh_current_category()
		)
		tabs.add_child(button)


func _phase29_build_active_graph(terrain: Resource) -> void:
	var scope_slots: Array[Resource] = _phase28_scope_slots(terrain, _phase28_domain)
	var production_slot: Resource = _phase29_production_slot(terrain, _phase28_domain)
	var editable_slots: Array[Resource] = scope_slots.duplicate()

	if _phase28_scope_mode == ScopeMode.BIOME:
		# The global production flow is inherited by every biome but is not silently
		# edited while the user thinks they are changing one biome.
		if editable_slots.is_empty():
			_phase29_build_inherited_biome_state(terrain, production_slot)
			return
	else:
		if production_slot != null and not editable_slots.has(production_slot):
			editable_slots.push_front(production_slot)

	if editable_slots.is_empty():
		_add_note("No graph exists for this target yet.")
		_phase29_add_layer_button(terrain, "+ Add changes here")
		return

	var selected: Resource = terrain.call("find_shader_slot", _phase28_selected_slot_id) as Resource
	if selected == null or not editable_slots.has(selected):
		selected = production_slot if _phase28_scope_mode == ScopeMode.GLOBAL \
			and production_slot != null else editable_slots[0]
		_phase28_selected_slot_id = String(selected.get(&"slot_id"))

	_phase29_build_layer_bar(terrain, editable_slots, selected)
	_phase29_build_graph_editor(selected)


func _phase29_production_slot(terrain: Resource, domain: int) -> Resource:
	var slot_id: String = PRODUCTION_SHAPE_SLOT_ID \
		if domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT else PRODUCTION_SURFACE_SLOT_ID
	return terrain.call("find_shader_slot", slot_id) as Resource


func _phase29_build_inherited_biome_state(terrain: Resource,
		production_slot: Resource) -> void:
	var title := Label.new()
	title.text = "%s currently uses the Global terrain." % BIOME_NAMES[_phase28_biome_id]
	title.add_theme_font_size_override("font_size", 18)
	_workspace.add_child(title)
	var note := Label.new()
	note.text = "Nothing special is defined for this biome at L%d. Add a biome layer to change only this biome, or edit Global to change every biome." % _shader_composer_level
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.66, 0.76, 0.84)
	_workspace.add_child(note)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	_workspace.add_child(row)
	var add := Button.new()
	add.text = "+ Add %s changes" % BIOME_NAMES[_phase28_biome_id]
	add.custom_minimum_size = Vector2(250.0, 42.0)
	add.pressed.connect(_phase29_create_layer.bind(terrain))
	row.add_child(add)
	var edit_global := Button.new()
	edit_global.text = "Edit inherited Global graph"
	edit_global.custom_minimum_size = Vector2(230.0, 42.0)
	edit_global.pressed.connect(func() -> void:
		_phase28_scope_mode = ScopeMode.GLOBAL
		_phase28_selected_slot_id = String(production_slot.get(&"slot_id")) if production_slot != null else ""
		_refresh_current_category()
	)
	row.add_child(edit_global)


func _phase29_build_layer_bar(terrain: Resource, slots: Array[Resource],
		selected: Resource) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	row.custom_minimum_size.y = 40.0
	_workspace.add_child(row)
	var label := Label.new()
	label.text = "Layer"
	label.add_theme_font_size_override("font_size", 16)
	label.custom_minimum_size.x = 60.0
	row.add_child(label)
	var picker := OptionButton.new()
	picker.custom_minimum_size.x = 390.0
	for slot: Resource in slots:
		picker.add_item(_phase29_layer_name(slot))
		picker.set_item_metadata(picker.item_count - 1, String(slot.get(&"slot_id")))
		if slot == selected:
			picker.select(picker.item_count - 1)
	picker.item_selected.connect(func(index: int) -> void:
		_phase28_selected_slot_id = String(picker.get_item_metadata(index))
		_refresh_current_category()
	)
	row.add_child(picker)
	var add := Button.new()
	add.text = "+ Add layer"
	add.pressed.connect(_phase29_create_layer.bind(terrain))
	row.add_child(add)

	var production: bool = _phase29_is_production_slot(selected)
	if production:
		var reset := Button.new()
		reset.text = "Reset base"
		reset.tooltip_text = "Restore the current rendered terrain pass-through graph"
		reset.pressed.connect(_phase29_reset_production_graph.bind(selected))
		row.add_child(reset)
	else:
		var enabled := CheckButton.new()
		enabled.text = "On"
		enabled.button_pressed = bool(selected.get(&"enabled"))
		enabled.toggled.connect(func(value: bool) -> void:
			_session.stage_set(selected, &"enabled", value,
				WorldAuthoringSession.ApplyScope.GRAPH, "Toggle terrain layer")
		)
		row.add_child(enabled)
		var remove := Button.new()
		remove.text = "Delete"
		remove.pressed.connect(func() -> void:
			_session.remove_terrain_shader_slot(String(selected.get(&"slot_id")))
			_phase28_selected_slot_id = ""
			_refresh_current_category()
		)
		row.add_child(remove)


func _phase29_layer_name(slot: Resource) -> String:
	if String(slot.get(&"slot_id")) == PRODUCTION_SHAPE_SLOT_ID:
		return "Base Terrain — current rendered shape"
	if String(slot.get(&"slot_id")) == PRODUCTION_SURFACE_SLOT_ID:
		return "Base Surface — current rendered material"
	return String(slot.get(&"display_name"))


func _phase29_is_production_slot(slot: Resource) -> bool:
	if slot == null:
		return false
	var slot_id: String = String(slot.get(&"slot_id"))
	return slot_id == PRODUCTION_SHAPE_SLOT_ID or slot_id == PRODUCTION_SURFACE_SLOT_ID


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := PHASE29_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1080.0, 680.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))
	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "This is the active terrain shape flow. CURRENT TERRAIN HEIGHT is a real graph node now: disconnect it, route it through operations, or replace it completely."
	else:
		hint.text = "This is the active surface flow. Current PBR channels and world data are real graph inputs. Add texture, classification, math or PBR nodes and wire the result to FINAL SURFACE."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)


func _phase29_create_layer(terrain: Resource) -> void:
	_phase28_create_cell_slot(terrain)


func _phase29_add_layer_button(terrain: Resource, text: String) -> void:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size = Vector2(240.0, 42.0)
	button.pressed.connect(_phase29_create_layer.bind(terrain))
	_workspace.add_child(button)


func _phase29_reset_production_graph(slot: Resource) -> void:
	if slot == null:
		return
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or not graph.has_method("create_production_graph"):
		return
	_session.stage_action("Reset production terrain graph", func() -> void:
		graph.call("create_production_graph", int(slot.get(&"domain")))
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _phase29_build_advanced_toggle(terrain: Resource) -> void:
	var separator := HSeparator.new()
	_workspace.add_child(separator)
	var toggle := Button.new()
	toggle.text = "Hide Advanced" if _phase29_show_advanced else "Advanced"
	toggle.tooltip_text = "LOD scope maps, effective stacks and raw production shader source"
	toggle.pressed.connect(func() -> void:
		_phase29_show_advanced = not _phase29_show_advanced
		_refresh_current_category()
	)
	_workspace.add_child(toggle)
	if not _phase29_show_advanced:
		return
	_build_phase28_effective_stack(terrain)
	_build_phase28_scope_overview(terrain)
	_build_phase28_advanced_runtime()
