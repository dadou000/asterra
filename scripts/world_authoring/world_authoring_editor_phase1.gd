extends "res://scripts/world_authoring/world_authoring_editor.gd"
## Planet Studio Phase 1: editable celestial hierarchy, biome-layer stack,
## composable terrain shader slots, node graphs, and authored water features.

const SHADER_SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const WATER_FEATURE_SCRIPT := preload("res://scripts/world_authoring/model/water_feature_definition.gd")
const GRAPH_EDITOR_SCRIPT := preload("res://scripts/world_authoring/terrain_graph_editor.gd")
const CELESTIAL_MAP_SCRIPT := preload("res://scripts/world_authoring/celestial_system_map.gd")

const BIOME_NAMES: Array[String] = [
	"Ocean", "Shelf sea", "Ice cap", "Tundra", "Taiga", "Cold desert",
	"Temperate grassland", "Temperate forest", "Temperate rainforest", "Mediterranean",
	"Steppe", "Hot desert", "Savanna", "Tropical seasonal forest", "Tropical rainforest",
	"Wetland", "Alpine", "Bare rock",
]

var _selected_biome_layer_id: String = ""
var _selected_displacement_slot_id: String = ""
var _selected_material_slot_id: String = ""
var _selected_graph_slot_id: String = ""
var _selected_water_feature_id: String = ""

func _ready() -> void:
	super._ready()
	_set_status("Planet Studio Phase 1 — authoring model is live; production runtime binding remains staged behind Apply.")

func _build_planet_page() -> void:
	super._build_planet_page()
	var body: Resource = _session.active_body()
	if body == null:
		return
	_section("Orbit parent and orientation")
	var parent_picker := OptionButton.new()
	parent_picker.custom_minimum_size.x = 340.0
	parent_picker.add_item("System root / no parent")
	parent_picker.set_item_metadata(0, "")
	var body_id := String(body.get(&"body_id"))
	var current_parent := String(body.get(&"parent_body_id"))
	var bodies: Array = _session.staged_system.get(&"bodies")
	for candidate_value: Variant in bodies:
		var candidate := candidate_value as Resource
		if candidate == null:
			continue
		var candidate_id := String(candidate.get(&"body_id"))
		if candidate_id == body_id:
			continue
		if not bool(_session.staged_system.call("can_parent_body", body_id, candidate_id)):
			continue
		parent_picker.add_item(String(candidate.get(&"display_name")))
		parent_picker.set_item_metadata(parent_picker.item_count - 1, candidate_id)
		if candidate_id == current_parent:
			parent_picker.select(parent_picker.item_count - 1)
	parent_picker.item_selected.connect(func(index: int) -> void:
		_session.set_active_body_parent(String(parent_picker.get_item_metadata(index)))
		_refresh_current_category()
	)
	_add_control_row("Orbits", parent_picker)
	var orbit: Resource = body.get(&"orbit") as Resource
	if orbit != null:
		_add_number_field("Ascending node", float(orbit.get(&"longitude_ascending_node_deg")), -360.0, 360.0, 0.1, "°", func(value: float) -> void:
			_session.stage_set(orbit, &"longitude_ascending_node_deg", value, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change ascending node")
		)
		_add_number_field("Argument of periapsis", float(orbit.get(&"argument_periapsis_deg")), -360.0, 360.0, 0.1, "°", func(value: float) -> void:
			_session.stage_set(orbit, &"argument_periapsis_deg", value, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change periapsis argument")
		)
		_add_number_field("Mean anomaly at epoch", float(orbit.get(&"mean_anomaly_at_epoch_deg")), -3600.0, 3600.0, 0.1, "°", func(value: float) -> void:
			_session.stage_set(orbit, &"mean_anomaly_at_epoch_deg", value, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change orbital anomaly")
		)
		_add_number_field("Orbit epoch", float(orbit.get(&"epoch_s")), -1.0e12, 1.0e12, 1.0, " s", func(value: float) -> void:
			_session.stage_set(orbit, &"epoch_s", value, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change orbit epoch")
		)

	_section("Rings")
	var rings: Resource = body.get(&"rings") as Resource
	if rings == null:
		_add_note("No ring definition is attached to this body.")
		return
	_add_toggle("Ring system enabled", bool(rings.get(&"enabled")), func(value: bool) -> void:
		_session.stage_set(rings, &"enabled", value, SESSION_SCRIPT.ApplyScope.GRAPH, "Toggle rings")
	)
	var radius_km := float(body.get(&"radius_m")) / 1000.0
	_add_number_field("Ring inner radius", float(rings.get(&"inner_radius_m")) / 1000.0, 0.0, 1.0e9, 1.0, " km", func(value: float) -> void:
		_session.stage_action("Change ring inner radius", func() -> void:
			rings.set(&"inner_radius_m", value * 1000.0)
			rings.call("normalize_ranges", float(body.get(&"radius_m")))
		, SESSION_SCRIPT.ApplyScope.GRAPH)
	)
	_add_number_field("Ring outer radius", float(rings.get(&"outer_radius_m")) / 1000.0, 0.0, 1.0e9, 1.0, " km", func(value: float) -> void:
		_session.stage_action("Change ring outer radius", func() -> void:
			rings.set(&"outer_radius_m", value * 1000.0)
			rings.call("normalize_ranges", float(body.get(&"radius_m")))
		, SESSION_SCRIPT.ApplyScope.GRAPH)
	)
	_add_number_field("Ring optical depth", float(rings.get(&"optical_depth")), 0.0, 1.0, 0.001, "", func(value: float) -> void:
		_session.stage_set(rings, &"optical_depth", value, SESSION_SCRIPT.ApplyScope.HOT, "Change ring optical depth")
	)
	_add_number_field("Ring roughness", float(rings.get(&"roughness")), 0.0, 1.0, 0.001, "", func(value: float) -> void:
		_session.stage_set(rings, &"roughness", value, SESSION_SCRIPT.ApplyScope.HOT, "Change ring roughness")
	)
	if not bool(rings.get(&"enabled")) and float(rings.get(&"inner_radius_m")) <= 0.0:
		_add_note("Tip: a first ring can start around %.0f–%.0f km and then be tuned visually." % [radius_km * 1.25, radius_km * 2.2])

func _build_terrain_page() -> void:
	var body: Resource = _session.active_body()
	_page_title("Terrain", "Procedural generation stays pristine; biome paint and shader graphs are layered non-destructively on top.")
	if body == null:
		return
	var terrain: Resource = _session.active_terrain_profile()
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return
	var generation: Resource = terrain.call("ensure_generation_profile") as Resource
	_section("Generation")
	_add_text_field("World seed (int64)", str(int(generation.get(&"world_seed"))), func(value: String) -> void:
		if value.is_valid_int():
			_session.stage_set(generation, &"world_seed", value.to_int(), SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change world seed")
		else:
			_set_status("World seed must be a signed 64-bit integer.")
	)
	_add_number_field("Ocean fraction", float(generation.get(&"ocean_fraction")), 0.0, 1.0, 0.001, "", func(value: float) -> void:
		_session.stage_set(generation, &"ocean_fraction", value, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change ocean fraction")
	)
	_add_number_field("Maximum uplift", float(generation.get(&"max_uplift")), 0.0, 30000.0, 10.0, " m", func(value: float) -> void:
		_session.stage_set(generation, &"max_uplift", value, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "Change uplift")
	)
	_add_number_field("Detail amplitude", float(generation.get(&"detail_amplitude")), 0.0, 5000.0, 1.0, " m", func(value: float) -> void:
		_session.stage_set(generation, &"detail_amplitude", value, SESSION_SCRIPT.ApplyScope.CLIPMAP, "Change detail amplitude")
	)

	_section("Post-generation biome paint")
	_build_biome_layer_editor(terrain)
	_section("Displacement shader stack")
	_build_shader_slot_editor(terrain, SHADER_SLOT_SCRIPT.Domain.DISPLACEMENT)
	_section("Material shader stack")
	_build_shader_slot_editor(terrain, SHADER_SLOT_SCRIPT.Domain.MATERIAL)

func _build_biome_layer_editor(terrain: Resource) -> void:
	var layers: Array = terrain.get(&"biome_override_layers")
	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 6)
	_workspace.add_child(toolbar)
	var add_layer := _toolbar_button("Add Layer")
	add_layer.pressed.connect(func() -> void:
		var layer: Resource = _session.create_biome_layer("Biome Paint %d" % (layers.size() + 1))
		if layer != null:
			_selected_biome_layer_id = String(layer.get(&"layer_id"))
		_refresh_current_category()
	)
	toolbar.add_child(add_layer)
	var selector := OptionButton.new()
	selector.custom_minimum_size.x = 320.0
	for layer_value: Variant in layers:
		var layer := layer_value as Resource
		if layer == null:
			continue
		selector.add_item(String(layer.get(&"display_name")))
		selector.set_item_metadata(selector.item_count - 1, String(layer.get(&"layer_id")))
	if selector.item_count > 0:
		if _selected_biome_layer_id.is_empty() or terrain.call("find_biome_layer", _selected_biome_layer_id) == null:
			_selected_biome_layer_id = String(selector.get_item_metadata(0))
		for index: int in selector.item_count:
			if String(selector.get_item_metadata(index)) == _selected_biome_layer_id:
				selector.select(index)
		selector.item_selected.connect(func(index: int) -> void:
			_selected_biome_layer_id = String(selector.get_item_metadata(index))
			_refresh_current_category()
		)
	else:
		selector.add_item("No biome paint layers")
		selector.disabled = true
	toolbar.add_child(selector)
	var delete_layer := _toolbar_button("Delete")
	delete_layer.disabled = layers.is_empty()
	delete_layer.pressed.connect(func() -> void:
		if _session.remove_biome_layer(_selected_biome_layer_id):
			_selected_biome_layer_id = ""
		_refresh_current_category()
	)
	toolbar.add_child(delete_layer)
	if layers.is_empty():
		_add_note("Create a layer to paint categorical biome overrides after generation. Strokes are stored sparsely in planet-centric coordinates and will rasterize only affected terrain tiles.")
		return
	var selected: Resource = terrain.call("find_biome_layer", _selected_biome_layer_id) as Resource
	if selected == null:
		return
	_add_toggle("Layer enabled", bool(selected.get(&"enabled")), func(value: bool) -> void:
		_session.stage_set(selected, &"enabled", value, SESSION_SCRIPT.ApplyScope.TILES, "Toggle biome layer")
	)
	_add_text_field("Layer name", String(selected.get(&"display_name")), func(value: String) -> void:
		_session.stage_set(selected, &"display_name", value, SESSION_SCRIPT.ApplyScope.TILES, "Rename biome layer")
	)
	_add_number_field("Layer opacity", float(selected.get(&"opacity")), 0.0, 1.0, 0.01, "", func(value: float) -> void:
		_session.stage_set(selected, &"opacity", value, SESSION_SCRIPT.ApplyScope.TILES, "Change biome layer opacity")
	)
	var biome_picker := OptionButton.new()
	biome_picker.custom_minimum_size.x = 300.0
	var current_biome := int(selected.get(&"active_biome_id"))
	for biome_id: int in BIOME_NAMES.size():
		biome_picker.add_item(BIOME_NAMES[biome_id])
		biome_picker.set_item_metadata(biome_id, biome_id)
	biome_picker.select(clampi(current_biome, 0, BIOME_NAMES.size() - 1))
	biome_picker.item_selected.connect(func(index: int) -> void:
		_session.stage_set(selected, &"active_biome_id", int(biome_picker.get_item_metadata(index)), SESSION_SCRIPT.ApplyScope.TILES, "Select paint biome")
	)
	_add_control_row("Paint biome", biome_picker)
	_add_number_field("Brush radius", float(selected.get(&"brush_radius_m")), 0.05, 1000000.0, 1.0, " m", func(value: float) -> void:
		_session.stage_set(selected, &"brush_radius_m", value, SESSION_SCRIPT.ApplyScope.TILES, "Change biome brush radius")
	)
	_add_number_field("Brush hardness", float(selected.get(&"brush_hardness")), 0.0, 1.0, 0.01, "", func(value: float) -> void:
		_session.stage_set(selected, &"brush_hardness", value, SESSION_SCRIPT.ApplyScope.TILES, "Change biome brush hardness")
	)
	_add_number_field("Brush opacity", float(selected.get(&"brush_opacity")), 0.0, 1.0, 0.01, "", func(value: float) -> void:
		_session.stage_set(selected, &"brush_opacity", value, SESSION_SCRIPT.ApplyScope.TILES, "Change biome brush opacity")
	)
	var stroke_row := HBoxContainer.new()
	stroke_row.add_theme_constant_override("separation", 8)
	_workspace.add_child(stroke_row)
	var stroke_label := Label.new()
	stroke_label.text = "%d stored brush strokes" % int(selected.call("stroke_count"))
	stroke_label.custom_minimum_size.x = 260.0
	stroke_row.add_child(stroke_label)
	var clear_button := Button.new()
	clear_button.text = "Clear Strokes"
	clear_button.disabled = int(selected.call("stroke_count")) == 0
	clear_button.pressed.connect(func() -> void:
		_session.stage_action("Clear biome paint", func() -> void: selected.call("clear_strokes"), SESSION_SCRIPT.ApplyScope.TILES)
		_refresh_current_category()
	)
	stroke_row.add_child(clear_button)
	_add_note("The data path for painting is now live. The next viewport pass will feed cursor hit directions into this layer instead of baking a second heightmap or editing the procedural generator.")

func _build_shader_slot_editor(terrain: Resource, domain: int) -> void:
	var collection: Array = terrain.get(&"material_slots") if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL else terrain.get(&"displacement_slots")
	var selected_id := _selected_material_slot_id if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL else _selected_displacement_slot_id
	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 6)
	_workspace.add_child(toolbar)
	var add_button := _toolbar_button("Add Slot")
	add_button.pressed.connect(func() -> void:
		var prefix := "Material" if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL else "Displacement"
		var slot: Resource = _session.create_terrain_shader_slot(domain, "%s %d" % [prefix, collection.size() + 1])
		if slot != null:
			if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL:
				_selected_material_slot_id = String(slot.get(&"slot_id"))
			else:
				_selected_displacement_slot_id = String(slot.get(&"slot_id"))
		_refresh_current_category()
	)
	toolbar.add_child(add_button)
	var selector := OptionButton.new()
	selector.custom_minimum_size.x = 300.0
	for slot_value: Variant in collection:
		var slot := slot_value as Resource
		if slot == null:
			continue
		selector.add_item(String(slot.get(&"display_name")))
		selector.set_item_metadata(selector.item_count - 1, String(slot.get(&"slot_id")))
	if selector.item_count == 0:
		selector.add_item("No slots")
		selector.disabled = true
		toolbar.add_child(selector)
		_add_note("Slots compile into a single terrain path; adding multiple slots does not imply one terrain draw call per slot.")
		return
	if selected_id.is_empty() or terrain.call("find_shader_slot", selected_id) == null:
		selected_id = String(selector.get_item_metadata(0))
		if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL:
			_selected_material_slot_id = selected_id
		else:
			_selected_displacement_slot_id = selected_id
	for index: int in selector.item_count:
		if String(selector.get_item_metadata(index)) == selected_id:
			selector.select(index)
	selector.item_selected.connect(func(index: int) -> void:
		var next_id := String(selector.get_item_metadata(index))
		if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL:
			_selected_material_slot_id = next_id
		else:
			_selected_displacement_slot_id = next_id
		_refresh_current_category()
	)
	toolbar.add_child(selector)
	var remove_button := _toolbar_button("Delete")
	remove_button.pressed.connect(func() -> void:
		_session.remove_terrain_shader_slot(selected_id)
		if _selected_graph_slot_id == selected_id:
			_selected_graph_slot_id = ""
		if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL:
			_selected_material_slot_id = ""
		else:
			_selected_displacement_slot_id = ""
		_refresh_current_category()
	)
	toolbar.add_child(remove_button)
	var slot: Resource = terrain.call("find_shader_slot", selected_id) as Resource
	if slot == null:
		return
	_add_toggle("Slot enabled", bool(slot.get(&"enabled")), func(value: bool) -> void:
		_session.stage_set(slot, &"enabled", value, SESSION_SCRIPT.ApplyScope.GRAPH, "Toggle terrain slot")
	)
	_add_text_field("Slot name", String(slot.get(&"display_name")), func(value: String) -> void:
		_session.stage_set(slot, &"display_name", value, SESSION_SCRIPT.ApplyScope.GRAPH, "Rename terrain slot")
	)
	_add_number_field("Strength", float(slot.get(&"strength")), -1000.0, 1000.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(slot, &"strength", value, SESSION_SCRIPT.ApplyScope.GRAPH, "Change terrain slot strength")
	)
	var blend_picker := OptionButton.new()
	for label: String in ["Add", "Subtract", "Multiply", "Min", "Max", "Replace"]:
		blend_picker.add_item(label)
	blend_picker.select(clampi(int(slot.get(&"blend_mode")), 0, blend_picker.item_count - 1))
	blend_picker.item_selected.connect(func(index: int) -> void:
		_session.stage_set(slot, &"blend_mode", index, SESSION_SCRIPT.ApplyScope.GRAPH, "Change slot composition")
	)
	_add_control_row("Composition", blend_picker)
	var biome_mode_picker := OptionButton.new()
	for label: String in ["All biomes", "Only selected", "All except selected"]:
		biome_mode_picker.add_item(label)
	biome_mode_picker.select(clampi(int(slot.get(&"biome_mask_mode")), 0, 2))
	biome_mode_picker.item_selected.connect(func(index: int) -> void:
		_session.stage_set(slot, &"biome_mask_mode", index, SESSION_SCRIPT.ApplyScope.GRAPH, "Change slot biome mask")
		_refresh_current_category()
	)
	_add_control_row("Biome mask", biome_mode_picker)
	_build_clipmap_mask(slot)
	if int(slot.get(&"biome_mask_mode")) != SHADER_SLOT_SCRIPT.BiomeMaskMode.ALL:
		_build_biome_mask(slot)
	var graph_button := Button.new()
	graph_button.text = "Close Node Graph" if _selected_graph_slot_id == selected_id else "Open Node Graph"
	graph_button.pressed.connect(func() -> void:
		_selected_graph_slot_id = "" if _selected_graph_slot_id == selected_id else selected_id
		_refresh_current_category()
	)
	_workspace.add_child(graph_button)
	if _selected_graph_slot_id == selected_id:
		var graph_editor := GRAPH_EDITOR_SCRIPT.new()
		graph_editor.setup(_session, slot, Callable(self, "_refresh_current_category"))
		_workspace.add_child(graph_editor)

func _build_clipmap_mask(slot: Resource) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 5)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = "Clipmap levels"
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	for level: int in 8:
		var toggle := CheckButton.new()
		toggle.text = "L%d" % level
		toggle.button_pressed = bool(slot.call("applies_to_clipmap", level))
		toggle.toggled.connect(func(value: bool) -> void:
			_session.stage_action("Change slot clipmap mask", func() -> void: slot.call("set_clipmap_enabled", level, value), SESSION_SCRIPT.ApplyScope.GRAPH)
		)
		row.add_child(toggle)

func _build_biome_mask(slot: Resource) -> void:
	var grid := GridContainer.new()
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 10)
	grid.add_theme_constant_override("v_separation", 2)
	_workspace.add_child(grid)
	var selected_ids: PackedInt32Array = slot.get(&"biome_ids")
	for biome_id: int in BIOME_NAMES.size():
		var toggle := CheckButton.new()
		toggle.text = BIOME_NAMES[biome_id]
		toggle.button_pressed = selected_ids.has(biome_id)
		toggle.toggled.connect(func(value: bool) -> void:
			var next_ids: PackedInt32Array = slot.get(&"biome_ids")
			if value and not next_ids.has(biome_id):
				next_ids.append(biome_id)
			elif not value:
				var index := next_ids.find(biome_id)
				if index >= 0:
					next_ids.remove_at(index)
			_session.stage_set(slot, &"biome_ids", next_ids, SESSION_SCRIPT.ApplyScope.GRAPH, "Change slot biome selection")
		)
		grid.add_child(toggle)

func _build_water_page() -> void:
	var body: Resource = _session.active_body()
	_page_title("Water", "Ocean, freeform lakes and cubic 3D Bézier rivers are authored into one water profile for shared clipmap rasterization and simulation.")
	if body == null:
		return
	var water: Resource = _session.active_water_profile()
	if water == null:
		_add_note("The selected body has no water profile.")
		return
	_section("Ocean")
	_add_toggle("Ocean enabled", bool(water.get(&"ocean_enabled")), func(value: bool) -> void:
		_session.stage_set(water, &"ocean_enabled", value, SESSION_SCRIPT.ApplyScope.CLIPMAP, "Toggle ocean")
	)
	_add_number_field("Sea level", float(water.get(&"sea_level_m")), -20000.0, 20000.0, 0.1, " m", func(value: float) -> void:
		_session.stage_set(water, &"sea_level_m", value, SESSION_SCRIPT.ApplyScope.CLIPMAP, "Change sea level")
	)
	_add_number_field("Wave amplitude scale", float(water.get(&"wave_amplitude_scale")), 0.0, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(water, &"wave_amplitude_scale", value, SESSION_SCRIPT.ApplyScope.HOT, "Change wave amplitude")
	)
	_add_number_field("Wave frequency scale", float(water.get(&"wave_frequency_scale")), 0.01, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(water, &"wave_frequency_scale", value, SESSION_SCRIPT.ApplyScope.HOT, "Change wave frequency")
	)
	_section("Authored lakes and rivers")
	_build_water_feature_editor(water, body)

func _build_water_feature_editor(water: Resource, body: Resource) -> void:
	var features: Array = water.get(&"authored_features")
	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 6)
	_workspace.add_child(toolbar)
	var add_lake := _toolbar_button("Add Lake")
	add_lake.pressed.connect(func() -> void:
		var feature: Resource = _session.create_water_feature(WATER_FEATURE_SCRIPT.FeatureType.LAKE, "Lake %d" % (features.size() + 1))
		if feature != null:
			_selected_water_feature_id = String(feature.get(&"feature_id"))
		_refresh_current_category()
	)
	toolbar.add_child(add_lake)
	var add_river := _toolbar_button("Add River")
	add_river.pressed.connect(func() -> void:
		var feature: Resource = _session.create_water_feature(WATER_FEATURE_SCRIPT.FeatureType.RIVER, "River %d" % (features.size() + 1))
		if feature != null:
			_selected_water_feature_id = String(feature.get(&"feature_id"))
		_refresh_current_category()
	)
	toolbar.add_child(add_river)
	var selector := OptionButton.new()
	selector.custom_minimum_size.x = 320.0
	for feature_value: Variant in features:
		var feature := feature_value as Resource
		if feature == null:
			continue
		var kind := "Lake" if int(feature.get(&"feature_type")) == WATER_FEATURE_SCRIPT.FeatureType.LAKE else "River"
		selector.add_item("%s — %s" % [kind, String(feature.get(&"display_name"))])
		selector.set_item_metadata(selector.item_count - 1, String(feature.get(&"feature_id")))
	if selector.item_count == 0:
		selector.add_item("No authored water features")
		selector.disabled = true
		toolbar.add_child(selector)
		_add_note("Add a lake or river. Geometry is stored in body-centric 3D coordinates, ready for the upcoming on-planet placement/manipulator viewport.")
		return
	if _selected_water_feature_id.is_empty() or water.call("find_feature", _selected_water_feature_id) == null:
		_selected_water_feature_id = String(selector.get_item_metadata(0))
	for index: int in selector.item_count:
		if String(selector.get_item_metadata(index)) == _selected_water_feature_id:
			selector.select(index)
	selector.item_selected.connect(func(index: int) -> void:
		_selected_water_feature_id = String(selector.get_item_metadata(index))
		_refresh_current_category()
	)
	toolbar.add_child(selector)
	var remove_button := _toolbar_button("Delete")
	remove_button.pressed.connect(func() -> void:
		_session.remove_water_feature(_selected_water_feature_id)
		_selected_water_feature_id = ""
		_refresh_current_category()
	)
	toolbar.add_child(remove_button)
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		return
	_add_toggle("Feature enabled", bool(feature.get(&"enabled")), func(value: bool) -> void:
		_session.stage_set(feature, &"enabled", value, SESSION_SCRIPT.ApplyScope.TILES, "Toggle water feature")
	)
	_add_toggle("Clipmap simulation", bool(feature.get(&"clipmap_simulation_enabled")), func(value: bool) -> void:
		_session.stage_set(feature, &"clipmap_simulation_enabled", value, SESSION_SCRIPT.ApplyScope.CLIPMAP, "Toggle water feature simulation")
	)
	_add_text_field("Feature name", String(feature.get(&"display_name")), func(value: String) -> void:
		_session.stage_set(feature, &"display_name", value, SESSION_SCRIPT.ApplyScope.TILES, "Rename water feature")
	)
	_add_number_field("Surface level", float(feature.get(&"surface_level_m")), -20000.0, 20000.0, 0.1, " m", func(value: float) -> void:
		_session.stage_set(feature, &"surface_level_m", value, SESSION_SCRIPT.ApplyScope.TILES, "Change water surface level")
	)
	_add_number_field("Shore falloff", float(feature.get(&"shore_falloff_m")), 0.0, 100000.0, 0.1, " m", func(value: float) -> void:
		_session.stage_set(feature, &"shore_falloff_m", value, SESSION_SCRIPT.ApplyScope.TILES, "Change shore falloff")
	)
	_add_number_field("Default depth", float(feature.get(&"default_depth_m")), 0.0, 100000.0, 0.1, " m", func(value: float) -> void:
		_session.stage_set(feature, &"default_depth_m", value, SESSION_SCRIPT.ApplyScope.TILES, "Change water depth")
	)
	_add_number_field("Feature wave amplitude", float(feature.get(&"wave_amplitude_scale")), 0.0, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(feature, &"wave_amplitude_scale", value, SESSION_SCRIPT.ApplyScope.HOT, "Change feature waves")
	)
	_add_number_field("Current scale", float(feature.get(&"current_scale")), 0.0, 100.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(feature, &"current_scale", value, SESSION_SCRIPT.ApplyScope.HOT, "Change water current")
	)
	_build_water_geometry_controls(feature, body)

func _build_water_geometry_controls(feature: Resource, body: Resource) -> void:
	var is_river := int(feature.get(&"feature_type")) == WATER_FEATURE_SCRIPT.FeatureType.RIVER
	var points_count := (feature.get(&"river_knots") as Array).size() if is_river else (feature.get(&"lake_polygon_body_m") as PackedVector3Array).size()
	var label := Label.new()
	label.text = "%d Bézier knots" % points_count if is_river else "%d polygon vertices" % points_count
	label.modulate = Color(0.70, 0.80, 0.90)
	_workspace.add_child(label)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 5)
	_workspace.add_child(row)
	var row_label := Label.new()
	row_label.text = "Seed 3D body-space point"
	row_label.custom_minimum_size.x = 260.0
	row.add_child(row_label)
	var default_radius := float(body.get(&"radius_m")) + float(feature.get(&"surface_level_m"))
	var x := _small_spin(-1.0e9, 1.0e9, 1.0, default_radius)
	var y := _small_spin(-1.0e9, 1.0e9, 1.0, 0.0)
	var z := _small_spin(-1.0e9, 1.0e9, 1.0, 0.0)
	row.add_child(x)
	row.add_child(y)
	row.add_child(z)
	var add := Button.new()
	add.text = "Add Knot" if is_river else "Add Vertex"
	add.pressed.connect(func() -> void:
		var position := Vector3(x.value, y.value, z.value)
		_session.stage_action("Add water control point", func() -> void:
			if is_river:
				feature.call("add_river_knot", position, 20.0, float(feature.get(&"default_depth_m")), 1.0)
			else:
				feature.call("add_lake_point", position)
		, SESSION_SCRIPT.ApplyScope.TILES)
		_refresh_current_category()
	)
	row.add_child(add)
	if points_count > 0:
		var remove_last := Button.new()
		remove_last.text = "Remove Last"
		remove_last.pressed.connect(func() -> void:
			_session.stage_action("Remove water control point", func() -> void:
				if is_river:
					feature.call("remove_river_knot", points_count - 1)
				else:
					feature.call("remove_lake_point", points_count - 1)
			, SESSION_SCRIPT.ApplyScope.TILES)
			_refresh_current_category()
		)
		row.add_child(remove_last)
	_add_note("The numeric seed control is mainly for validating the persistent geometry path. The next 3D placement tool will create/move these same points directly under the cursor, with Bézier handles for rivers and polygon handles for lakes.")

func _build_celestials_page() -> void:
	super._build_celestials_page()
	_section("Celestial map")
	var map := CELESTIAL_MAP_SCRIPT.new()
	map.setup(_session.staged_system, String(_session.staged_system.get(&"active_body_id")))
	map.body_selected.connect(func(body_id: String) -> void:
		_session.select_body(body_id)
		_refresh_all()
	)
	_workspace.add_child(map)

func _add_control_row(label_text: String, control: Control) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	row.add_child(control)

func _small_spin(min_value: float, max_value: float, step: float, value: float) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.value = clampf(value, min_value, max_value)
	spin.custom_minimum_size.x = 135.0
	return spin

func _refresh_current_category() -> void:
	_show_category(_category)
	_refresh_toolbar()
