extends "res://scripts/world_authoring/world_authoring_editor_live_phase26.gd"
## Phase 27: level-centric terrain shader composition UI.
##
## Clipmap membership used to be hidden inside each selected shader slot. This
## presents the inverse workflow: choose L0-L7 first, then compose the exact ordered
## displacement/material stacks active on that level. A full matrix makes all level
## assignments visible at once.

const SHADER_SLOT_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const COMPOSER_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor.gd")
const COMPOSER_LEVEL_COUNT: int = 8
const SHADERS_CATEGORY := "SHADERS"

var _shader_composer_level: int = 0


func _build_shell() -> void:
	super._build_shell()
	_install_shader_navigation_button()


func _show_category(category_name: String) -> void:
	if category_name == SHADERS_CATEGORY:
		_category = SHADERS_CATEGORY
		_clear_workspace()
		_build_terrain_shader_composer_page()
		return
	super._show_category(category_name)


# The normal Terrain page keeps its familiar sections, but the old buried slot
# inspector is replaced by one clear jump into the dedicated composer.
func _build_shader_slot_editor(_terrain: Resource, domain: int) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = "Managed in the level-centric Shader Composer."
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.modulate = Color(0.60, 0.72, 0.82)
	row.add_child(label)
	var open := Button.new()
	open.text = "Open Shader Composer"
	open.tooltip_text = "Compose %s shaders by L0-L7 clipmap level" % (
		"material" if domain == SHADER_SLOT_MODEL.Domain.MATERIAL else "displacement")
	open.pressed.connect(_show_category.bind(SHADERS_CATEGORY))
	row.add_child(open)


func _install_shader_navigation_button() -> void:
	if _find_button_with_text(self, SHADERS_CATEGORY) != null:
		return
	var terrain_button: Button = _find_button_with_text(self, "TERRAIN")
	if terrain_button == null:
		return
	var parent: Node = terrain_button.get_parent()
	if parent == null:
		return
	var button := Button.new()
	button.name = "TerrainShadersNavigation"
	button.text = SHADERS_CATEGORY
	button.tooltip_text = "Compose terrain shaders by LOD / clipmap level"
	button.custom_minimum_size.y = terrain_button.custom_minimum_size.y
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.pressed.connect(_show_category.bind(SHADERS_CATEGORY))
	parent.add_child(button)
	parent.move_child(button, mini(terrain_button.get_index() + 1, parent.get_child_count() - 1))


func _find_button_with_text(node: Node, target: String) -> Button:
	for child: Node in node.get_children():
		if child is Button and (child as Button).text == target:
			return child as Button
		var nested: Button = _find_button_with_text(child, target)
		if nested != null:
			return nested
	return null


func _build_terrain_shader_composer_page() -> void:
	_page_title(
		"Terrain Shaders",
		"Pick an L level first, then define the ordered shaders used there. L0 is nearest/highest detail; L7 is farthest/coarsest.")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return

	if terrain.has_method("is_blank") and bool(terrain.call("is_blank")):
		_add_note("BLANK terrain: generated height is OFF. Displacement shaders below are the terrain source; custom biome paint remains available.")
	else:
		_add_note("PROCEDURAL terrain: generated height is the base surface. Displacement shaders compose on top of it.")

	_build_shader_level_selector()
	_build_level_stack(terrain, SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
	_build_level_stack(terrain, SHADER_SLOT_MODEL.Domain.MATERIAL)
	_build_all_levels_matrix(terrain, SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
	_build_all_levels_matrix(terrain, SHADER_SLOT_MODEL.Domain.MATERIAL)
	_build_selected_composer_graph(terrain)

	_section("Actual compiled ground shader")
	_add_note("This is the final resident ShaderMaterial. Use it to inspect uniforms or edit the live .gdshader source after composing the authored L-level stacks above.")
	if _world_host != null:
		_build_runtime_shader_inspector(
			TARGET_TERRAIN,
			"Ground terrain — actual render shader",
			_runtime_material(TARGET_TERRAIN))


func _build_shader_level_selector() -> void:
	_section("LOD / clipmap level")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = "Editing"
	label.custom_minimum_size.x = 90.0
	row.add_child(label)
	var picker := OptionButton.new()
	picker.custom_minimum_size.x = 260.0
	for level: int in COMPOSER_LEVEL_COUNT:
		picker.add_item(_level_display_name(level))
		picker.set_item_metadata(level, level)
	picker.select(clampi(_shader_composer_level, 0, COMPOSER_LEVEL_COUNT - 1))
	picker.item_selected.connect(func(index: int) -> void:
		_shader_composer_level = int(picker.get_item_metadata(index))
		_refresh_current_category()
	)
	row.add_child(picker)

	var chips := GridContainer.new()
	chips.columns = 4
	chips.add_theme_constant_override("h_separation", 4)
	chips.add_theme_constant_override("v_separation", 4)
	_workspace.add_child(chips)
	for level: int in COMPOSER_LEVEL_COUNT:
		var chip := Button.new()
		chip.text = "L%d" % level
		chip.toggle_mode = true
		chip.button_pressed = level == _shader_composer_level
		chip.tooltip_text = _level_display_name(level)
		chip.custom_minimum_size = Vector2(72.0, 31.0)
		chip.pressed.connect(func() -> void:
			_shader_composer_level = level
			_refresh_current_category()
		)
		chips.add_child(chip)
	_add_note("A slot can participate in one level, several levels, or all levels. Within a level, composition is top-to-bottom.")


func _level_display_name(level: int) -> String:
	if level == 0:
		return "L0 — nearest / highest detail"
	if level == COMPOSER_LEVEL_COUNT - 1:
		return "L%d — farthest / coarsest" % level
	return "L%d — progressively coarser" % level


func _build_level_stack(terrain: Resource, domain: int) -> void:
	var is_material: bool = domain == SHADER_SLOT_MODEL.Domain.MATERIAL
	var domain_name := "Material" if is_material else "Displacement"
	_section("L%d %s composition" % [_shader_composer_level, domain_name])

	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 7)
	_workspace.add_child(toolbar)
	var add := Button.new()
	add.text = "+ %s on L%d" % [domain_name, _shader_composer_level]
	add.tooltip_text = "Create a new %s slot assigned only to L%d" % [domain_name.to_lower(), _shader_composer_level]
	add.pressed.connect(_create_slot_for_current_level.bind(terrain, domain))
	toolbar.add_child(add)
	var active_slots: Array[Resource] = _slots_for_level(terrain, domain, _shader_composer_level)
	var count_label := Label.new()
	count_label.text = "%d slot%s in this level" % [active_slots.size(), "" if active_slots.size() == 1 else "s"]
	count_label.modulate = Color(0.58, 0.70, 0.79)
	toolbar.add_child(count_label)

	if active_slots.is_empty():
		_add_note("Nothing is assigned to L%d. Use the + button above or the matrix below." % _shader_composer_level)
		return
	for local_index: int in active_slots.size():
		_build_level_slot_row(terrain, domain, active_slots[local_index], local_index, active_slots.size())


func _build_level_slot_row(terrain: Resource, domain: int, slot: Resource,
		local_index: int, local_count: int) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	_workspace.add_child(row)

	var order := Label.new()
	order.text = "#%d" % (local_index + 1)
	order.custom_minimum_size.x = 30.0
	order.modulate = Color(0.50, 0.62, 0.72)
	row.add_child(order)

	var enabled := CheckButton.new()
	enabled.button_pressed = bool(slot.get(&"enabled"))
	enabled.tooltip_text = "Enable/disable without changing L-level assignment"
	enabled.toggled.connect(func(value: bool) -> void:
		_session.stage_set(slot, &"enabled", value, WorldAuthoringSession.ApplyScope.GRAPH,
			"Toggle terrain shader slot")
	)
	row.add_child(enabled)

	var name_edit := LineEdit.new()
	name_edit.text = String(slot.get(&"display_name"))
	name_edit.custom_minimum_size.x = 120.0
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_edit.text_submitted.connect(func(value: String) -> void:
		_session.stage_set(slot, &"display_name", value, WorldAuthoringSession.ApplyScope.GRAPH,
			"Rename terrain shader slot")
		_refresh_current_category()
	)
	row.add_child(name_edit)

	var blend := OptionButton.new()
	blend.custom_minimum_size.x = 88.0
	for blend_name: String in ["Add", "Subtract", "Multiply", "Min", "Max", "Replace"]:
		blend.add_item(blend_name)
	blend.select(clampi(int(slot.get(&"blend_mode")), 0, blend.item_count - 1))
	blend.tooltip_text = "How this slot combines with the result above it"
	blend.item_selected.connect(func(index: int) -> void:
		_session.stage_set(slot, &"blend_mode", index, WorldAuthoringSession.ApplyScope.GRAPH,
			"Change shader composition mode")
	)
	row.add_child(blend)

	var strength := SpinBox.new()
	strength.min_value = -1000.0
	strength.max_value = 1000.0
	strength.step = 0.01
	strength.value = float(slot.get(&"strength"))
	strength.custom_minimum_size.x = 76.0
	strength.suffix = "×"
	strength.tooltip_text = "Slot strength"
	strength.value_changed.connect(func(value: float) -> void:
		_session.stage_set(slot, &"strength", value, WorldAuthoringSession.ApplyScope.GRAPH,
			"Change shader slot strength")
	)
	row.add_child(strength)

	var up := Button.new()
	up.text = "↑"
	up.tooltip_text = "Move earlier in L%d composition" % _shader_composer_level
	up.disabled = local_index == 0
	up.pressed.connect(_move_slot_within_level.bind(
		terrain, domain, String(slot.get(&"slot_id")), -1, _shader_composer_level))
	row.add_child(up)
	var down := Button.new()
	down.text = "↓"
	down.tooltip_text = "Move later in L%d composition" % _shader_composer_level
	down.disabled = local_index >= local_count - 1
	down.pressed.connect(_move_slot_within_level.bind(
		terrain, domain, String(slot.get(&"slot_id")), 1, _shader_composer_level))
	row.add_child(down)

	var graph := Button.new()
	graph.text = "Graph"
	graph.tooltip_text = "Open this slot's node graph below"
	graph.pressed.connect(func() -> void:
		_selected_graph_slot_id = String(slot.get(&"slot_id"))
		_refresh_current_category()
	)
	row.add_child(graph)

	var remove_level := Button.new()
	remove_level.text = "L−"
	remove_level.tooltip_text = "Remove from L%d only" % _shader_composer_level
	remove_level.pressed.connect(func() -> void:
		_stage_slot_level(slot, _shader_composer_level, false)
	)
	row.add_child(remove_level)

	var delete := Button.new()
	delete.text = "Del"
	delete.tooltip_text = "Delete this slot from every level"
	delete.pressed.connect(func() -> void:
		var slot_id := String(slot.get(&"slot_id"))
		_session.remove_terrain_shader_slot(slot_id)
		if _selected_graph_slot_id == slot_id:
			_selected_graph_slot_id = ""
		_refresh_current_category()
	)
	row.add_child(delete)

	if not bool(slot.get(&"enabled")):
		name_edit.modulate = Color(0.55, 0.58, 0.62)


func _build_all_levels_matrix(terrain: Resource, domain: int) -> void:
	var is_material: bool = domain == SHADER_SLOT_MODEL.Domain.MATERIAL
	var domain_name := "Material" if is_material else "Displacement"
	_section("%s — all L levels" % domain_name)
	var collection: Array = _slot_collection(terrain, domain)
	if collection.is_empty():
		_add_note("No %s slots exist yet." % domain_name.to_lower())
		return
	_add_note("Rows are global composition order. Check a cell to make that shader participate in that L level.")

	var grid := GridContainer.new()
	grid.columns = COMPOSER_LEVEL_COUNT + 1
	grid.add_theme_constant_override("h_separation", 3)
	grid.add_theme_constant_override("v_separation", 3)
	_workspace.add_child(grid)
	var corner := Label.new()
	corner.text = "Shader slot"
	corner.custom_minimum_size.x = 165.0
	grid.add_child(corner)
	for level: int in COMPOSER_LEVEL_COUNT:
		var header := Button.new()
		header.text = "L%d" % level
		header.flat = true
		header.tooltip_text = _level_display_name(level) + " — click to focus"
		header.custom_minimum_size.x = 38.0
		header.pressed.connect(func() -> void:
			_shader_composer_level = level
			_refresh_current_category()
		)
		grid.add_child(header)

	for slot_value: Variant in collection:
		var slot: Resource = slot_value as Resource
		if slot == null:
			continue
		var slot_button := Button.new()
		slot_button.text = String(slot.get(&"display_name"))
		slot_button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		slot_button.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		slot_button.custom_minimum_size.x = 165.0
		slot_button.tooltip_text = "Open this slot's node graph"
		slot_button.pressed.connect(func() -> void:
			_selected_graph_slot_id = String(slot.get(&"slot_id"))
			_refresh_current_category()
		)
		if not bool(slot.get(&"enabled")):
			slot_button.modulate = Color(0.55, 0.58, 0.62)
		grid.add_child(slot_button)
		for level: int in COMPOSER_LEVEL_COUNT:
			var cell := CheckButton.new()
			cell.text = ""
			cell.custom_minimum_size = Vector2(38.0, 30.0)
			cell.button_pressed = bool(slot.call("applies_to_clipmap", level))
			cell.tooltip_text = "%s on L%d" % [String(slot.get(&"display_name")), level]
			cell.toggled.connect(func(value: bool) -> void:
				_stage_slot_level(slot, level, value)
			)
			grid.add_child(cell)


func _build_selected_composer_graph(terrain: Resource) -> void:
	if _selected_graph_slot_id.is_empty():
		return
	var slot: Resource = terrain.call("find_shader_slot", _selected_graph_slot_id) as Resource
	if slot == null:
		_selected_graph_slot_id = ""
		return
	_section("Node graph — %s" % String(slot.get(&"display_name")))
	var close := Button.new()
	close.text = "Close Graph"
	close.pressed.connect(func() -> void:
		_selected_graph_slot_id = ""
		_refresh_current_category()
	)
	_workspace.add_child(close)
	var graph_editor := COMPOSER_GRAPH_EDITOR.new()
	graph_editor.setup(_session, slot, Callable(self, "_refresh_current_category"))
	_workspace.add_child(graph_editor)


func _slot_collection(terrain: Resource, domain: int) -> Array:
	if domain == SHADER_SLOT_MODEL.Domain.MATERIAL:
		return terrain.get(&"material_slots") as Array
	return terrain.get(&"displacement_slots") as Array


func _slot_property(domain: int) -> StringName:
	if domain == SHADER_SLOT_MODEL.Domain.MATERIAL:
		return &"material_slots"
	return &"displacement_slots"


func _slots_for_level(terrain: Resource, domain: int, level: int) -> Array[Resource]:
	var result: Array[Resource] = []
	for slot_value: Variant in _slot_collection(terrain, domain):
		var slot: Resource = slot_value as Resource
		if slot != null and bool(slot.call("applies_to_clipmap", level)):
			result.append(slot)
	return result


func _create_slot_for_current_level(terrain: Resource, domain: int) -> void:
	var property_name: StringName = _slot_property(domain)
	var collection: Array = terrain.get(property_name) as Array
	var domain_name := "Material" if domain == SHADER_SLOT_MODEL.Domain.MATERIAL else "Displacement"
	var display_name := "%s L%d %d" % [domain_name, _shader_composer_level, collection.size() + 1]
	_session.stage_action("Create %s shader on L%d" % [domain_name.to_lower(), _shader_composer_level], func() -> void:
		var created: Resource = terrain.call("create_shader_slot", domain, display_name) as Resource
		if created != null:
			created.set(&"clipmap_level_mask", 1 << _shader_composer_level)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	collection = terrain.get(property_name) as Array
	if not collection.is_empty():
		var created: Resource = collection.back() as Resource
		if created != null:
			if domain == SHADER_SLOT_MODEL.Domain.MATERIAL:
				_selected_material_slot_id = String(created.get(&"slot_id"))
			else:
				_selected_displacement_slot_id = String(created.get(&"slot_id"))
	_refresh_current_category()


func _stage_slot_level(slot: Resource, level: int, value: bool) -> void:
	if slot == null:
		return
	_session.stage_action("%s L%d for %s" % ["Enable" if value else "Disable", level, String(slot.get(&"display_name"))], func() -> void:
		slot.call("set_clipmap_enabled", level, value)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _move_slot_within_level(terrain: Resource, domain: int, slot_id: String,
		direction: int, level: int) -> void:
	var property_name: StringName = _slot_property(domain)
	_session.stage_action("Reorder terrain shader composition", func() -> void:
		var slots: Array = terrain.get(property_name) as Array
		var current_index: int = -1
		for index: int in slots.size():
			var candidate: Resource = slots[index] as Resource
			if candidate != null and String(candidate.get(&"slot_id")) == slot_id:
				current_index = index
				break
		if current_index < 0:
			return
		var neighbor_index: int = -1
		var cursor: int = current_index + direction
		while cursor >= 0 and cursor < slots.size():
			var candidate: Resource = slots[cursor] as Resource
			if candidate != null and bool(candidate.call("applies_to_clipmap", level)):
				neighbor_index = cursor
				break
			cursor += direction
		if neighbor_index < 0:
			return
		var temporary: Variant = slots[current_index]
		slots[current_index] = slots[neighbor_index]
		slots[neighbor_index] = temporary
		terrain.set(property_name, slots)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()
