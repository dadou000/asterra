extends "res://scripts/world_authoring/world_authoring_editor_live_phase27.gd"
## Phase 28: shader authoring is addressed as Level x Scope x Domain.
##
## The user selects one concrete clipmap level, then Global or one biome, then edits
## the displacement or material graph for that cell directly. Existing shared slots
## remain visible/editable; new cells are canonical one-level/one-scope slots.

const PHASE28_LEVEL_COUNT: int = 15
const PHASE28_PANEL_WIDTH: float = 1240.0
const PHASE28_COMPACT_WIDTH: float = 760.0

enum ScopeMode {
	GLOBAL,
	BIOME,
}

var _phase28_scope_mode: int = ScopeMode.GLOBAL
var _phase28_biome_id: int = 7
var _phase28_domain: int = SHADER_SLOT_MODEL.Domain.DISPLACEMENT
var _phase28_selected_slot_id: String = ""
var _phase28_show_advanced: bool = false


func _show_category(category_name: String) -> void:
	if category_name == SHADERS_CATEGORY:
		_category = SHADERS_CATEGORY
		_set_shader_lab_panel_width(true)
		_clear_workspace()
		_build_terrain_shader_composer_page()
		return
	_set_shader_lab_panel_width(false)
	super._show_category(category_name)


func _set_shader_lab_panel_width(enabled: bool) -> void:
	if _authoring_panel != null:
		_authoring_panel.offset_right = PHASE28_PANEL_WIDTH if enabled else PHASE28_COMPACT_WIDTH
	_sync_eye_button_position()


func _sync_eye_button_position() -> void:
	if _eye_button == null:
		return
	var width: float = PHASE28_PANEL_WIDTH if _category == SHADERS_CATEGORY else PHASE28_COMPACT_WIDTH
	var x: float = EYE_BUTTON_MARGIN_PX
	if _authoring_panel != null and _authoring_panel.visible:
		x = width + EYE_BUTTON_MARGIN_PX
	_eye_button.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_eye_button.offset_left = x
	_eye_button.offset_top = EYE_BUTTON_MARGIN_PX
	_eye_button.offset_right = x + EYE_BUTTON_SIZE.x
	_eye_button.offset_bottom = EYE_BUTTON_MARGIN_PX + EYE_BUTTON_SIZE.y


func _is_live_viewport_point(point: Vector2) -> bool:
	var viewport_size: Vector2 = get_viewport_rect().size
	if point.x < 0.0 or point.y < 0.0 or point.x >= viewport_size.x or point.y >= viewport_size.y:
		return false
	if _authoring_panel != null and _authoring_panel.visible:
		var width: float = PHASE28_PANEL_WIDTH if _category == SHADERS_CATEGORY else PHASE28_COMPACT_WIDTH
		return point.x > width + 2.0
	return true


func _build_terrain_shader_composer_page() -> void:
	_page_title("Terrain Shader Lab",
		"Choose one L layer, then Global or a biome. Edit the displacement and texture/material graphs that actually compose that scope.")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return

	_build_phase28_address_bar()
	_build_phase28_effective_stack(terrain)
	_build_phase28_domain_tabs()
	_build_phase28_cell_editor(terrain)
	_build_phase28_scope_overview(terrain)
	_build_phase28_advanced_runtime()


func _build_phase28_address_bar() -> void:
	_section("1 — Target")
	var address := HBoxContainer.new()
	address.add_theme_constant_override("separation", 8)
	_workspace.add_child(address)

	var level_label := Label.new()
	level_label.text = "L layer"
	level_label.custom_minimum_size.x = 64.0
	address.add_child(level_label)
	var level_picker := OptionButton.new()
	level_picker.custom_minimum_size.x = 230.0
	for level: int in PHASE28_LEVEL_COUNT:
		level_picker.add_item(_phase28_level_name(level))
		level_picker.set_item_metadata(level, level)
	level_picker.select(clampi(_shader_composer_level, 0, PHASE28_LEVEL_COUNT - 1))
	level_picker.item_selected.connect(func(index: int) -> void:
		_shader_composer_level = int(level_picker.get_item_metadata(index))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	address.add_child(level_picker)

	var scope_label := Label.new()
	scope_label.text = "Scope"
	address.add_child(scope_label)
	var scope_picker := OptionButton.new()
	scope_picker.add_item("GLOBAL")
	scope_picker.set_item_metadata(0, ScopeMode.GLOBAL)
	scope_picker.add_item("BIOME")
	scope_picker.set_item_metadata(1, ScopeMode.BIOME)
	scope_picker.select(_phase28_scope_mode)
	scope_picker.item_selected.connect(func(index: int) -> void:
		_phase28_scope_mode = int(scope_picker.get_item_metadata(index))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	address.add_child(scope_picker)

	var biome_picker := OptionButton.new()
	biome_picker.custom_minimum_size.x = 245.0
	for biome_id: int in BIOME_NAMES.size():
		biome_picker.add_item(BIOME_NAMES[biome_id])
		biome_picker.set_item_metadata(biome_id, biome_id)
	biome_picker.select(clampi(_phase28_biome_id, 0, BIOME_NAMES.size() - 1))
	biome_picker.disabled = _phase28_scope_mode != ScopeMode.BIOME
	biome_picker.item_selected.connect(func(index: int) -> void:
		_phase28_biome_id = int(biome_picker.get_item_metadata(index))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	address.add_child(biome_picker)

	var breadcrumb := Label.new()
	breadcrumb.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	breadcrumb.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	breadcrumb.text = "L%d  /  %s" % [
		_shader_composer_level,
		"GLOBAL" if _phase28_scope_mode == ScopeMode.GLOBAL else BIOME_NAMES[_phase28_biome_id],
	]
	breadcrumb.modulate = Color(0.74, 0.84, 0.92)
	address.add_child(breadcrumb)

	var chips := HBoxContainer.new()
	chips.add_theme_constant_override("separation", 3)
	_workspace.add_child(chips)
	for level: int in PHASE28_LEVEL_COUNT:
		var chip := Button.new()
		chip.text = "L%d" % level
		chip.toggle_mode = true
		chip.button_pressed = level == _shader_composer_level
		chip.custom_minimum_size = Vector2(49.0, 30.0)
		chip.tooltip_text = _phase28_level_name(level)
		chip.pressed.connect(func() -> void:
			_shader_composer_level = level
			_phase28_selected_slot_id = ""
			_refresh_current_category()
		)
		chips.add_child(chip)

	_add_note("L0 is nearest/highest detail. L14 is the farthest/coarsest production clipmap level. Global graphs always run first; a matching biome graph composes on top.")


func _phase28_level_name(level: int) -> String:
	if level == 0:
		return "L0 — nearest / highest detail"
	if level == PHASE28_LEVEL_COUNT - 1:
		return "L14 — farthest / coarsest"
	return "L%d — progressively coarser" % level


func _build_phase28_domain_tabs() -> void:
	_section("2 — Graph domain")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	for data: Dictionary in [
		{"label":"DISPLACEMENT", "domain":SHADER_SLOT_MODEL.Domain.DISPLACEMENT,
			"tip":"Vertex displacement; also evaluated by terrain contact/physics."},
		{"label":"TEXTURE / MATERIAL", "domain":SHADER_SLOT_MODEL.Domain.MATERIAL,
			"tip":"Post-PBR albedo, normal and roughness composition with live texture nodes."},
	]:
		var button := Button.new()
		button.text = String(data["label"])
		button.toggle_mode = true
		button.button_pressed = _phase28_domain == int(data["domain"])
		button.custom_minimum_size = Vector2(210.0, 36.0)
		button.tooltip_text = String(data["tip"])
		var domain_value: int = int(data["domain"])
		button.pressed.connect(func() -> void:
			_phase28_domain = domain_value
			_phase28_selected_slot_id = ""
			_refresh_current_category()
		)
		row.add_child(button)


func _build_phase28_effective_stack(terrain: Resource) -> void:
	_section("Effective composition at this target")
	var columns := HBoxContainer.new()
	columns.add_theme_constant_override("separation", 10)
	_workspace.add_child(columns)
	for domain: int in [SHADER_SLOT_MODEL.Domain.DISPLACEMENT, SHADER_SLOT_MODEL.Domain.MATERIAL]:
		var panel := PanelContainer.new()
		panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		columns.add_child(panel)
		var box := VBoxContainer.new()
		box.add_theme_constant_override("separation", 3)
		panel.add_child(box)
		var title := Label.new()
		title.text = "DISPLACEMENT" if domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT else "TEXTURE / MATERIAL"
		title.modulate = Color(0.75, 0.84, 0.93)
		box.add_child(title)
		var effective: Array[Resource] = _phase28_effective_slots(terrain, domain)
		if effective.is_empty():
			var empty := Label.new()
			empty.text = "generated/base renderer only"
			empty.modulate = Color(0.52, 0.62, 0.70)
			box.add_child(empty)
			continue
		for index: int in effective.size():
			var slot: Resource = effective[index]
			var button := Button.new()
			button.alignment = HORIZONTAL_ALIGNMENT_LEFT
			button.text = "%d. %s  ·  %s" % [index + 1,
				String(slot.get(&"display_name")), _phase28_slot_scope_text(slot)]
			button.tooltip_text = "Click to edit this contributing graph"
			var slot_id: String = String(slot.get(&"slot_id"))
			button.pressed.connect(func() -> void:
				_phase28_domain = domain
				_phase28_selected_slot_id = slot_id
				_refresh_current_category()
			)
			box.add_child(button)


func _phase28_effective_slots(terrain: Resource, domain: int) -> Array[Resource]:
	var out: Array[Resource] = []
	var collection: Array = terrain.get(&"material_slots") if domain == SHADER_SLOT_MODEL.Domain.MATERIAL \
		else terrain.get(&"displacement_slots")
	for slot_value: Variant in collection:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")):
			continue
		if not bool(slot.call("applies_to_clipmap", _shader_composer_level)):
			continue
		var mode: int = int(slot.get(&"biome_mask_mode"))
		if _phase28_scope_mode == ScopeMode.GLOBAL:
			if mode == SHADER_SLOT_MODEL.BiomeMaskMode.ALL:
				out.append(slot)
			continue
		if mode == SHADER_SLOT_MODEL.BiomeMaskMode.ALL \
				or bool(slot.call("applies_to_biome", _phase28_biome_id)):
			out.append(slot)
	return out


func _phase28_scope_slots(terrain: Resource, domain: int) -> Array[Resource]:
	var out: Array[Resource] = []
	var collection: Array = terrain.get(&"material_slots") if domain == SHADER_SLOT_MODEL.Domain.MATERIAL \
		else terrain.get(&"displacement_slots")
	for slot_value: Variant in collection:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.call("applies_to_clipmap", _shader_composer_level)):
			continue
		var mode: int = int(slot.get(&"biome_mask_mode"))
		if _phase28_scope_mode == ScopeMode.GLOBAL and mode == SHADER_SLOT_MODEL.BiomeMaskMode.ALL:
			out.append(slot)
		elif _phase28_scope_mode == ScopeMode.BIOME \
				and mode == SHADER_SLOT_MODEL.BiomeMaskMode.ONLY:
			var ids: PackedInt32Array = slot.get(&"biome_ids")
			if ids.has(_phase28_biome_id):
				out.append(slot)
	return out


func _build_phase28_cell_editor(terrain: Resource) -> void:
	_section("3 — Edit graph")
	var scope_slots: Array[Resource] = _phase28_scope_slots(terrain, _phase28_domain)
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 7)
	_workspace.add_child(header)
	var scope_name: String = "GLOBAL" if _phase28_scope_mode == ScopeMode.GLOBAL else BIOME_NAMES[_phase28_biome_id]
	var domain_name: String = "Displacement" if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT else "Material"
	var label := Label.new()
	label.text = "L%d / %s / %s" % [_shader_composer_level, scope_name, domain_name]
	label.add_theme_font_size_override("font_size", 17)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(label)
	var create := Button.new()
	create.text = "+ Create %s Graph" % domain_name
	create.pressed.connect(_phase28_create_cell_slot.bind(terrain))
	header.add_child(create)

	if scope_slots.is_empty():
		_add_note("No authored graph exists for this cell. The renderer currently falls through to the Global/base result. Create a graph to author this exact L/scope/domain.")
		return

	if _phase28_selected_slot_id.is_empty() or terrain.call("find_shader_slot", _phase28_selected_slot_id) == null:
		_phase28_selected_slot_id = String(scope_slots[0].get(&"slot_id"))
	var selected: Resource = terrain.call("find_shader_slot", _phase28_selected_slot_id) as Resource
	if selected == null or not scope_slots.has(selected):
		selected = scope_slots[0]
		_phase28_selected_slot_id = String(selected.get(&"slot_id"))

	if scope_slots.size() > 1:
		var selector := OptionButton.new()
		selector.custom_minimum_size.x = 420.0
		for slot: Resource in scope_slots:
			selector.add_item(String(slot.get(&"display_name")))
			selector.set_item_metadata(selector.item_count - 1, String(slot.get(&"slot_id")))
			if String(slot.get(&"slot_id")) == _phase28_selected_slot_id:
				selector.select(selector.item_count - 1)
		selector.item_selected.connect(func(index: int) -> void:
			_phase28_selected_slot_id = String(selector.get_item_metadata(index))
			_refresh_current_category()
		)
		_add_control_row("Graph in this cell", selector)

	_build_phase28_slot_controls(terrain, selected)
	var graph_editor := COMPOSER_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1050.0, 620.0)
	# Attach the Control before TerrainGraphEditor builds GraphEdit/GraphNodes. Its
	# setup path connects graph nodes immediately; doing that while detached can
	# make Godot Control internals observe a null viewport during an input rebuild.
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, selected,
		Callable(self, "_refresh_current_category"))

	if _phase28_domain == SHADER_SLOT_MODEL.Domain.MATERIAL:
		_add_note("Material graphs are post-PBR: GAME_INPUT base_albedo/base_normal/base_roughness expose the current production terrain shader. TEXTURE_2D and TRIPLANAR nodes are live GPU inputs. Leaving the pass-through base nodes connected produces the current shader unchanged.")
	else:
		_add_note("Displacement graphs are authoritative geometry. The same deterministic graph is used for rendered terrain and contact/physics. L-specific graphs are evaluated before LOD triangle morphing to preserve watertight transitions.")


func _build_phase28_slot_controls(terrain: Resource, slot: Resource) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	var enabled := CheckButton.new()
	enabled.text = "Enabled"
	enabled.button_pressed = bool(slot.get(&"enabled"))
	enabled.toggled.connect(func(value: bool) -> void:
		_session.stage_set(slot, &"enabled", value, WorldAuthoringSession.ApplyScope.GRAPH, "Toggle scoped shader graph")
	)
	row.add_child(enabled)
	var name_edit := LineEdit.new()
	name_edit.text = String(slot.get(&"display_name"))
	name_edit.custom_minimum_size.x = 300.0
	name_edit.text_submitted.connect(func(value: String) -> void:
		_session.stage_set(slot, &"display_name", value, WorldAuthoringSession.ApplyScope.GRAPH, "Rename scoped shader graph")
	)
	row.add_child(name_edit)
	var blend := OptionButton.new()
	for text: String in ["Add", "Subtract", "Multiply", "Min", "Max", "Replace"]:
		blend.add_item(text)
	blend.select(clampi(int(slot.get(&"blend_mode")), 0, 5))
	blend.item_selected.connect(func(index: int) -> void:
		_session.stage_set(slot, &"blend_mode", index, WorldAuthoringSession.ApplyScope.GRAPH, "Change scoped graph blend")
	)
	row.add_child(blend)
	var strength := SpinBox.new()
	strength.min_value = -1000.0
	strength.max_value = 1000.0
	strength.step = 0.01
	strength.value = float(slot.get(&"strength"))
	strength.custom_minimum_size.x = 120.0
	strength.value_changed.connect(func(value: float) -> void:
		_session.stage_set(slot, &"strength", value, WorldAuthoringSession.ApplyScope.GRAPH, "Change scoped graph strength")
	)
	row.add_child(strength)
	var delete := Button.new()
	delete.text = "Delete Graph"
	delete.pressed.connect(func() -> void:
		_session.remove_terrain_shader_slot(String(slot.get(&"slot_id")))
		_phase28_selected_slot_id = ""
		_refresh_current_category()
	)
	row.add_child(delete)

	var shared_levels: PackedStringArray = PackedStringArray()
	for level: int in PHASE28_LEVEL_COUNT:
		if bool(slot.call("applies_to_clipmap", level)):
			shared_levels.append("L%d" % level)
	var ids: PackedInt32Array = slot.get(&"biome_ids")
	if shared_levels.size() > 1 or ids.size() > 1:
		_add_note("Shared legacy graph: this resource also affects %s%s. Editing its nodes changes every listed target. New Phase 28 graphs are one-level/one-scope by default." % [
			", ".join(shared_levels),
			(" and %d biome scopes" % ids.size()) if ids.size() > 1 else "",
		])


func _phase28_create_cell_slot(terrain: Resource) -> void:
	var scope_name: String = "Global" if _phase28_scope_mode == ScopeMode.GLOBAL else BIOME_NAMES[_phase28_biome_id]
	var domain_name: String = "Displacement" if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT else "Material"
	var created: Resource = null
	_session.stage_action("Create L%d %s %s graph" % [_shader_composer_level, scope_name, domain_name], func() -> void:
		created = terrain.call("create_shader_slot", _phase28_domain,
			"L%d %s %s" % [_shader_composer_level, scope_name, domain_name]) as Resource
		if created == null:
			return
		created.set(&"clipmap_level_mask", 1 << _shader_composer_level)
		if _phase28_scope_mode == ScopeMode.GLOBAL:
			created.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ALL)
			created.set(&"biome_ids", PackedInt32Array())
		else:
			created.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ONLY)
			created.set(&"biome_ids", PackedInt32Array([_phase28_biome_id]))
		if _phase28_domain == SHADER_SLOT_MODEL.Domain.MATERIAL:
			created.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.REPLACE)
			created.set(&"strength", 1.0)
			_phase28_make_material_passthrough(created)
		else:
			created.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD)
			created.set(&"strength", 1.0)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	if created != null:
		_phase28_selected_slot_id = String(created.get(&"slot_id"))
	_refresh_current_category()


func _phase28_make_material_passthrough(slot: Resource) -> void:
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return
	graph.call("create_default_graph", 1)
	var output_id: String = ""
	var nodes: Array = graph.get(&"nodes")
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == "OUTPUT_MATERIAL":
			output_id = String(node.get("id", ""))
			break
	if output_id.is_empty():
		return
	var sources: Array[String] = ["base_albedo", "base_normal", "base_roughness", "base_metallic", "base_ao"]
	for port: int in sources.size():
		var source_id: String = String(graph.call("add_node", "GAME_INPUT",
			Vector2(100.0, 80.0 + float(port) * 105.0), {"source": sources[port]}))
		graph.call("connect_nodes", source_id, 0, output_id, port)


func _phase28_slot_scope_text(slot: Resource) -> String:
	var mode: int = int(slot.get(&"biome_mask_mode"))
	if mode == SHADER_SLOT_MODEL.BiomeMaskMode.ALL:
		return "Global"
	var ids: PackedInt32Array = slot.get(&"biome_ids")
	var names := PackedStringArray()
	for biome_id: int in ids:
		if biome_id >= 0 and biome_id < BIOME_NAMES.size():
			names.append(BIOME_NAMES[biome_id])
	var prefix: String = "Only " if mode == SHADER_SLOT_MODEL.BiomeMaskMode.ONLY else "Except "
	return prefix + (", ".join(names) if not names.is_empty() else "none")


func _build_phase28_scope_overview(terrain: Resource) -> void:
	_section("Scope map")
	var grid := GridContainer.new()
	grid.columns = PHASE28_LEVEL_COUNT + 1
	grid.add_theme_constant_override("h_separation", 2)
	grid.add_theme_constant_override("v_separation", 2)
	_workspace.add_child(grid)
	var corner := Label.new()
	corner.text = "Scope"
	corner.custom_minimum_size.x = 150.0
	grid.add_child(corner)
	for level: int in PHASE28_LEVEL_COUNT:
		var h := Label.new()
		h.text = "L%d" % level
		h.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		h.custom_minimum_size.x = 48.0
		grid.add_child(h)
	for scope_index: int in BIOME_NAMES.size() + 1:
		var global_row: bool = scope_index == 0
		var biome_id: int = scope_index - 1
		var row_label := Button.new()
		row_label.text = "GLOBAL" if global_row else BIOME_NAMES[biome_id]
		row_label.alignment = HORIZONTAL_ALIGNMENT_LEFT
		row_label.pressed.connect(func() -> void:
			_phase28_scope_mode = ScopeMode.GLOBAL if global_row else ScopeMode.BIOME
			if not global_row:
				_phase28_biome_id = biome_id
			_phase28_selected_slot_id = ""
			_refresh_current_category()
		)
		grid.add_child(row_label)
		for level: int in PHASE28_LEVEL_COUNT:
			var cell := Button.new()
			cell.custom_minimum_size = Vector2(48.0, 29.0)
			var d_count: int = _phase28_count_scope_cell(terrain, SHADER_SLOT_MODEL.Domain.DISPLACEMENT, level, global_row, biome_id)
			var m_count: int = _phase28_count_scope_cell(terrain, SHADER_SLOT_MODEL.Domain.MATERIAL, level, global_row, biome_id)
			cell.text = ("D%d M%d" % [d_count, m_count]) if d_count + m_count > 0 else "·"
			cell.tooltip_text = "%s / L%d — %d displacement, %d material graph(s)" % [row_label.text, level, d_count, m_count]
			cell.pressed.connect(func() -> void:
				_shader_composer_level = level
				_phase28_scope_mode = ScopeMode.GLOBAL if global_row else ScopeMode.BIOME
				if not global_row:
					_phase28_biome_id = biome_id
				_phase28_selected_slot_id = ""
				_refresh_current_category()
			)
			grid.add_child(cell)


func _phase28_count_scope_cell(terrain: Resource, domain: int, level: int,
		global_scope: bool, biome_id: int) -> int:
	var count: int = 0
	var collection: Array = terrain.get(&"material_slots") if domain == SHADER_SLOT_MODEL.Domain.MATERIAL \
		else terrain.get(&"displacement_slots")
	for slot_value: Variant in collection:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.call("applies_to_clipmap", level)):
			continue
		var mode: int = int(slot.get(&"biome_mask_mode"))
		if global_scope:
			if mode == SHADER_SLOT_MODEL.BiomeMaskMode.ALL:
				count += 1
		elif mode == SHADER_SLOT_MODEL.BiomeMaskMode.ONLY:
			var ids: PackedInt32Array = slot.get(&"biome_ids")
			if ids.has(biome_id):
				count += 1
	return count


func _build_phase28_advanced_runtime() -> void:
	_section("Advanced runtime shader")
	var toggle := Button.new()
	toggle.text = "Hide raw ShaderMaterial / source" if _phase28_show_advanced else "Show raw ShaderMaterial / live source"
	toggle.pressed.connect(func() -> void:
		_phase28_show_advanced = not _phase28_show_advanced
		_refresh_current_category()
	)
	_workspace.add_child(toggle)
	if not _phase28_show_advanced:
		_add_note("Normally you should author with the scoped node graphs above. The raw production .gdshader editor remains available for engine-level work.")
		return
	if _world_host != null:
		_build_runtime_shader_inspector(TARGET_TERRAIN,
			"Ground terrain — actual compiled render shader", _runtime_material(TARGET_TERRAIN))
