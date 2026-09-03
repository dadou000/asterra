extends "res://scripts/world_authoring/world_authoring_editor_live_phase46_core.gd"
## Public Phase 46 activation wrapper.
##
## The viewport tool core can be instantiated by deterministic/headless geometry
## tests before it is attached to a SceneTree. Camera discovery therefore has to be
## null-safe, and purely spherical Radial/Ring/Region handles must not depend on a
## camera at all. Latitude/Longitude bands use the camera only to choose a visible
## point along their otherwise-global edge.

# Phase 47 UI policy: terrain authoring is deliberately a direct, beginner-facing
# view of the resident production controls.  The old SHADERS category and its node
# canvas were useful during development, but they made a normal "make this world
# more mountainous" edit look like shader programming.  These controls mutate the
# same serialized production graph, so the render/cache/contact path remains one
# system and every clipmap ring receives the identical terrain program.
const PHASE47_NATIVE := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const PHASE47_SCHEMA := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_schema.gd")
const PHASE47_CATALOG := preload(
	"res://scripts/world_authoring/model/terrain_beginner_parameter_catalog.gd")

const PHASE47_BIOME_PROFILE_PREFIX := "simple-biome-terrain-"
const PHASE47_ALL_RINGS_MASK: int = (1 << 15) - 1
const PHASE47_GLOBAL_TAB: int = 0
const PHASE47_BIOME_TAB: int = 1
const PHASE47_BIOME_PROFILE_TYPES: PackedStringArray = [
	"NOISE_LAYER", "RIDGED_MOUNTAINS", "EROSION_CHANNELS", "SEDIMENT_DEPOSIT", "TERRACE_RELIEF",
]
const PHASE47_BIOME_PROFILE_LABELS: PackedStringArray = [
	"Soft Noise", "Ridged Peaks", "Eroded Channels", "Sediment Fans", "Terraced Relief",
]

const PHASE47_PRESETS: Array[Dictionary] = [
	{"id":"natural", "label":"Natural", "tip":"Balanced production terrain.", "values":{}},
	{"id":"alpine", "label":"Alpine", "tip":"Taller, stronger mountain ranges.", "values":{
		"broad_strength":1.18, "mountain_strength":1.65,
		"mountain_amplitude_m":460.0, "mid_strength":1.20,
		"channel_strength":1.20, "fine_strength":0.82}},
	{"id":"rolling", "label":"Rolling Hills", "tip":"Gentle broad relief with active hills.", "values":{
		"broad_strength":0.75, "mountain_strength":0.42,
		"mountain_amplitude_m":95.0, "mid_strength":1.42,
		"channel_strength":0.72, "fine_strength":1.12}},
	{"id":"carved", "label":"Carved Valleys", "tip":"Strong drainage, valleys and depositional fans.", "values":{
		"mountain_strength":0.82, "mid_strength":0.88,
		"channel_strength":1.85, "channel_depth_max_m":105.0,
		"deposit_strength":1.45, "fine_strength":0.92}},
	{"id":"desert", "label":"Desert Dunes", "tip":"Dry fine detail with larger dunes.", "values":{
		"mountain_strength":0.62, "mid_strength":0.72,
		"fine_strength":0.70, "dune_strength":1.85,
		"dune_amplitude_m":30.0, "glacial_strength":0.0}},
	{"id":"glacial", "label":"Glacial", "tip":"Broad ice-shaped terrain and softened detail.", "values":{
		"mountain_strength":1.18, "mid_strength":0.78,
		"fine_strength":0.62, "dune_strength":0.0,
		"glacial_strength":1.85, "glacial_amplitude_m":135.0}},
]

var _phase47_editor_tab: int = PHASE47_GLOBAL_TAB
var _phase47_profile_blend_source: int = 6
var _phase47_profile_blend_amount: float = 0.5


func _build_shell() -> void:
	super._build_shell()
	# Phase 27 adds this navigation item after the ordinary category shell exists.
	# Remove it instead of merely hiding it: keyboard focus and screen readers then
	# cannot lead a new author back to the retired node/shader workflow.
	var shader_button: Button = _find_button_with_text(self, SHADERS_CATEGORY)
	if shader_button != null:
		shader_button.queue_free()


func _show_category(category_name: String) -> void:
	# Old saved UI state and obsolete buttons can still request this category.  Keep
	# those requests safe by taking the author to Terrain rather than the legacy lab.
	if category_name == SHADERS_CATEGORY:
		super._show_category("TERRAIN")
		return
	super._show_category(category_name)


func _build_terrain_page() -> void:
	_page_title("Terrain",
		"Adjust the planet's look with plain controls. Each change edits the real production terrain used by every near, middle and far clipmap ring.")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return

	_phase29_ensure_production_graphs(terrain)
	var base_slot: Resource = terrain.call("find_shader_slot",
		PHASE47_NATIVE.PRODUCTION_SHAPE_SLOT_ID) as Resource
	var graph: Resource = base_slot.get(&"graph") as Resource if base_slot != null else null
	if graph == null:
		_add_note("Base Terrain is unavailable for this body.")
		return
	if not _phase47_has_production_controls(graph):
		_phase47_build_repair_panel(graph)
		return

	_phase47_build_all_rings_notice()
	_phase47_build_editor_tabs()
	if _phase47_editor_tab == PHASE47_BIOME_TAB:
		_phase47_build_biome_editor(terrain)
	else:
		_phase47_build_preset_shelf(graph)
		_phase47_build_controls(graph)
		_phase47_build_biome_note()


func _phase47_build_editor_tabs() -> void:
	var tabs := HBoxContainer.new()
	tabs.name = "SimpleTerrainEditorTabs"
	tabs.add_theme_constant_override("separation", 8)
	_workspace.add_child(tabs)
	for item: Dictionary in [
		{"tab":PHASE47_GLOBAL_TAB, "label":"GLOBAL TERRAIN", "tip":"The shared terrain character for the whole planet."},
		{"tab":PHASE47_BIOME_TAB, "label":"BIOME TERRAIN", "tip":"Add a terrain profile that only applies inside one biome."},
	]:
		var button := Button.new()
		button.text = String(item["label"])
		button.toggle_mode = true
		button.button_pressed = _phase47_editor_tab == int(item["tab"])
		button.tooltip_text = String(item["tip"])
		button.custom_minimum_size = Vector2(210.0, 40.0)
		var tab_value: int = int(item["tab"])
		button.pressed.connect(func() -> void:
			_phase47_editor_tab = tab_value
			_refresh_current_category()
		)
		tabs.add_child(button)


func _phase47_build_all_rings_notice() -> void:
	var notice := PanelContainer.new()
	notice.name = "TerrainAllRingsNotice"
	_workspace.add_child(notice)
	var label := Label.new()
	label.text = "✓ One terrain look for the whole planet — these settings automatically apply to all 15 terrain distance rings. The cache rebuilds only after you make a change, never continuously while you travel."
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.modulate = Color(0.60, 0.80, 0.68)
	notice.add_child(label)


func _phase47_build_preset_shelf(graph: Resource) -> void:
	_section("Start with a look")
	_add_note("Pick a starting look, then make small changes below. Presets do not create a separate shader or terrain layer.")
	var grid := GridContainer.new()
	grid.name = "SimpleTerrainLookPresets"
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 8)
	grid.add_theme_constant_override("v_separation", 8)
	_workspace.add_child(grid)
	for preset: Dictionary in PHASE47_PRESETS:
		var button := Button.new()
		button.name = "TerrainLook_%s" % String(preset["id"])
		button.text = String(preset["label"])
		button.tooltip_text = String(preset["tip"])
		button.custom_minimum_size = Vector2(185.0, 38.0)
		button.pressed.connect(_phase47_apply_preset.bind(graph, preset))
		grid.add_child(button)
	var reset := Button.new()
	reset.name = "ResetTerrainLook"
	reset.text = "Reset to Natural Defaults"
	reset.tooltip_text = "Restore every beginner terrain control to the production defaults."
	reset.custom_minimum_size = Vector2(260.0, 38.0)
	reset.pressed.connect(_phase47_reset_controls.bind(graph))
	_workspace.add_child(reset)


func _phase47_build_controls(graph: Resource) -> void:
	_section("Fine-tune the landscape")
	_add_note("Use the sliders to change the terrain shader safely. You do not need to know nodes, LODs or shader code.")
	var controls: Array[Dictionary] = PHASE47_CATALOG.controls_for_mode(PHASE47_CATALOG.MODE_SIMPLE)
	# Deposition is a visible part of the production terrain but was omitted from the
	# original short catalog. Keep it alongside valley carving rather than hiding it.
	controls.append({
		"key":"deposit_strength", "title":"Valley Sediment", "category":"Valleys",
		"description":"Controls material deposited around drainage and valley floors.",
		"min":0.0, "max":2.5, "step":0.01, "unit":"",
	})
	for category: String in PHASE47_CATALOG.simple_categories():
		var category_controls: Array[Dictionary] = []
		for control: Dictionary in controls:
			if String(control.get("category", "")) == category:
				category_controls.append(control)
		if category_controls.is_empty():
			continue
		var panel := PanelContainer.new()
		panel.name = "TerrainControls_%s" % category.replace(" ", "")
		_workspace.add_child(panel)
		var box := VBoxContainer.new()
		box.add_theme_constant_override("separation", 6)
		panel.add_child(box)
		var title := Label.new()
		title.text = category
		title.add_theme_font_size_override("font_size", 16)
		box.add_child(title)
		for control: Dictionary in category_controls:
			_phase47_add_control(box, graph, control)


func _phase47_add_control(parent: VBoxContainer, graph: Resource,
		control: Dictionary) -> void:
	var key: String = String(control.get("key", ""))
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	parent.add_child(row)
	var label := Label.new()
	label.text = String(control.get("title", key))
	label.tooltip_text = String(control.get("description", ""))
	label.custom_minimum_size.x = 230.0
	row.add_child(label)
	var spin := SpinBox.new()
	spin.name = "TerrainControl_%s" % key
	spin.min_value = float(control.get("min", 0.0))
	spin.max_value = float(control.get("max", 1.0))
	spin.step = float(control.get("step", 0.01))
	spin.suffix = String(control.get("unit", ""))
	spin.value = clampf(_phase47_control_value(graph, key), spin.min_value, spin.max_value)
	spin.custom_minimum_size.x = 170.0
	spin.tooltip_text = String(control.get("description", ""))
	spin.value_changed.connect(_phase47_set_control.bind(graph, key))
	row.add_child(spin)
	var help := Label.new()
	help.text = String(control.get("description", ""))
	help.modulate = Color(0.58, 0.68, 0.76)
	help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	help.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(help)


func _phase47_build_biome_note() -> void:
	_section("Biomes")
	_add_note("Biomes decide where climate, soil, water and elevation rules allow dunes, glaciers, vegetation and rock to appear. This Terrain page changes their physical detail everywhere; use biome paint when you want to override where a biome occurs.")


func _phase47_build_biome_editor(terrain: Resource) -> void:
	_section("Biome terrain")
	_add_note("A biome profile is a small terrain layer that applies to the selected biome on every distance ring. It composes after the shared Global Terrain, so you can make deserts dune-like, mountains sharper or wetlands flatter without maintaining LOD copies.")
	var pick_row := HBoxContainer.new()
	pick_row.name = "BiomeTerrainSelection"
	pick_row.add_theme_constant_override("separation", 8)
	_workspace.add_child(pick_row)
	var biome_picker := OptionButton.new()
	biome_picker.name = "BiomeTerrainPicker"
	biome_picker.custom_minimum_size.x = 250.0
	for biome_id: int in BIOME_NAMES.size():
		biome_picker.add_item(BIOME_NAMES[biome_id])
		biome_picker.set_item_metadata(biome_id, biome_id)
	biome_picker.select(clampi(_phase28_biome_id, 0, BIOME_NAMES.size() - 1))
	biome_picker.item_selected.connect(func(index: int) -> void:
		_phase28_biome_id = int(biome_picker.get_item_metadata(index))
		_refresh_current_category()
	)
	pick_row.add_child(biome_picker)
	var pick := Button.new()
	pick.name = "PickBiomeUnderCursor"
	pick.text = "Pick Here"
	pick.tooltip_text = "Use the live terrain point under the mouse to select its procedural biome."
	pick.pressed.connect(_phase47_pick_biome_under_cursor)
	pick_row.add_child(pick)

	var biome_id: int = clampi(_phase28_biome_id, 0, BIOME_NAMES.size() - 1)
	var slot: Resource = _phase47_biome_profile_slot(terrain, biome_id)
	if slot == null:
		var create := Button.new()
		create.name = "CreateBiomeTerrainProfile"
		create.text = "Create %s Terrain Profile" % BIOME_NAMES[biome_id]
		create.tooltip_text = "Create a safe all-ring terrain layer limited to this biome."
		create.pressed.connect(_phase47_create_biome_profile.bind(terrain, biome_id))
		_workspace.add_child(create)
		return

	_phase47_build_biome_profile_controls(terrain, slot, biome_id)


func _phase47_pick_biome_under_cursor() -> void:
	var point: Vector2 = get_viewport().get_mouse_position()
	var hit: Dictionary = _screen_aim(point) as Dictionary
	var direction: Vector3 = Vector3(hit.get("dir", Vector3.ZERO))
	if direction.length_squared() < 0.99 or not Planet.has_method("sample_info"):
		_set_status("Move the cursor over the live planet, then pick its biome.")
		return
	var info: Dictionary = Planet.call("sample_info", direction.normalized()) as Dictionary
	_phase28_biome_id = clampi(int(info.get("biome", 0)), 0, BIOME_NAMES.size() - 1)
	_set_status("Selected biome: %s" % BIOME_NAMES[_phase28_biome_id])
	_refresh_current_category()


func _phase47_biome_profile_slot(terrain: Resource, biome_id: int) -> Resource:
	if terrain == null:
		return null
	var expected_id := "%s%d" % [PHASE47_BIOME_PROFILE_PREFIX, biome_id]
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = value as Resource
		if slot != null and String(slot.get(&"slot_id")) == expected_id:
			return slot
	return null


func _phase47_create_biome_profile(terrain: Resource, biome_id: int) -> void:
	if terrain == null or _phase47_biome_profile_slot(terrain, biome_id) != null:
		return
	_session.stage_action("Create %s terrain profile" % BIOME_NAMES[biome_id], func() -> void:
		var slot: Resource = terrain.call("create_shader_slot", SHADER_SLOT_MODEL.Domain.DISPLACEMENT,
			"%s Terrain" % BIOME_NAMES[biome_id]) as Resource
		if slot == null:
			return
		slot.set(&"slot_id", "%s%d" % [PHASE47_BIOME_PROFILE_PREFIX, biome_id])
		slot.set(&"display_name", "%s Terrain" % BIOME_NAMES[biome_id])
		slot.set(&"enabled", true)
		slot.set(&"clipmap_level_mask", PHASE47_ALL_RINGS_MASK)
		slot.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ONLY)
		slot.set(&"biome_ids", PackedInt32Array([biome_id]))
		slot.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD)
		slot.set(&"strength", 1.0)
		var graph: Resource = slot.get(&"graph") as Resource
		_phase47_rebuild_biome_profile(graph, _phase47_default_biome_profile())
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _phase47_default_biome_profile() -> Dictionary:
	return {"type":"NOISE_LAYER", "scale":6.0, "amount":35.0, "passes":3, "seed":1337, "steps":6}


func _phase47_biome_profile(graph: Resource) -> Dictionary:
	var result: Dictionary = _phase47_default_biome_profile()
	if graph == null:
		return result
	for value: Variant in graph.get(&"nodes") as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if not PHASE47_BIOME_PROFILE_TYPES.has(node_type):
			continue
		result["type"] = node_type
		for key_value: Variant in (node.get("parameters", {}) as Dictionary).keys():
			result[String(key_value)] = (node.get("parameters", {}) as Dictionary)[key_value]
		return result
	return result


func _phase47_rebuild_biome_profile(graph: Resource, profile: Dictionary) -> void:
	if graph == null:
		return
	var type: String = String(profile.get("type", "NOISE_LAYER"))
	if not PHASE47_BIOME_PROFILE_TYPES.has(type):
		type = "NOISE_LAYER"
	graph.call("create_default_graph", SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
	var output_id: String = ""
	for value: Variant in graph.get(&"nodes") as Array:
		var node: Dictionary = value as Dictionary
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = String(node.get("id", ""))
			break
	if output_id.is_empty():
		return
	var base_id: String = String(graph.call("add_node", "CONSTANT_FLOAT", Vector2(70.0, 180.0),
		{"value":0.0}))
	var parameters := {
		"scale":float(profile.get("scale", 6.0)),
		"amount":float(profile.get("amount", 35.0)),
		"passes":clampi(int(profile.get("passes", 3)), 1, 4),
		"seed":int(profile.get("seed", 1337)),
		"steps":clampi(int(profile.get("steps", 6)), 2, 24),
	}
	var profile_id: String = String(graph.call("add_node", type, Vector2(360.0, 180.0), parameters))
	graph.call("connect_nodes", base_id, 0, profile_id, 0)
	graph.call("connect_nodes", profile_id, 0, output_id, 0)


func _phase47_build_biome_profile_controls(terrain: Resource, slot: Resource,
		biome_id: int) -> void:
	var graph: Resource = slot.get(&"graph") as Resource
	var profile: Dictionary = _phase47_biome_profile(graph)
	var panel := PanelContainer.new()
	panel.name = "BiomeTerrainProfile"
	_workspace.add_child(panel)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	panel.add_child(box)
	var title := Label.new()
	title.text = "%s terrain profile" % BIOME_NAMES[biome_id]
	title.add_theme_font_size_override("font_size", 18)
	box.add_child(title)

	var type_row := HBoxContainer.new()
	box.add_child(type_row)
	var type_label := Label.new()
	type_label.text = "Terrain shape"
	type_label.custom_minimum_size.x = 220.0
	type_row.add_child(type_label)
	var type_picker := OptionButton.new()
	type_picker.name = "BiomeTerrainShape"
	for index: int in PHASE47_BIOME_PROFILE_TYPES.size():
		type_picker.add_item(PHASE47_BIOME_PROFILE_LABELS[index])
		type_picker.set_item_metadata(index, PHASE47_BIOME_PROFILE_TYPES[index])
		if PHASE47_BIOME_PROFILE_TYPES[index] == String(profile["type"]):
			type_picker.select(index)
	type_picker.item_selected.connect(func(index: int) -> void:
		profile["type"] = String(type_picker.get_item_metadata(index))
		_phase47_stage_biome_profile(graph, profile, "Change biome terrain shape")
	)
	type_row.add_child(type_picker)

	_phase47_add_biome_number(box, "Feature size", profile, "scale", 0.1, 100.0, 0.1,
		"Higher values make the pattern smaller and more frequent.", graph)
	_phase47_add_biome_number(box, "Feature height", profile, "amount", 0.0, 1000.0, 1.0,
		"Maximum terrain change before the profile blend strength.", graph)
	if String(profile["type"]) == "TERRACE_RELIEF":
		_phase47_add_biome_number(box, "Terrace steps", profile, "steps", 2.0, 24.0, 1.0,
			"More steps makes smaller terraces; fewer steps makes bold plateaus.", graph)
	else:
		_phase47_add_biome_number(box, "Detail passes", profile, "passes", 1.0, 4.0, 1.0,
			"More passes adds finer nested structure.", graph)
	_phase47_add_biome_number(box, "Pattern seed", profile, "seed", 0.0, 999999.0, 1.0,
		"Changes the pattern without changing the biome boundary.", graph)

	var blend_row := HBoxContainer.new()
	box.add_child(blend_row)
	var blend_label := Label.new()
	blend_label.text = "Compose with global"
	blend_label.custom_minimum_size.x = 220.0
	blend_row.add_child(blend_label)
	var blend_picker := OptionButton.new()
	for text: String in ["Add", "Subtract", "Multiply", "Min", "Max", "Replace"]:
		blend_picker.add_item(text)
	blend_picker.select(clampi(int(slot.get(&"blend_mode")), 0, 5))
	blend_picker.item_selected.connect(func(index: int) -> void:
		_session.stage_set(slot, &"blend_mode", index, WorldAuthoringSession.ApplyScope.GRAPH,
			"Change biome terrain composition")
	)
	blend_row.add_child(blend_picker)
	var strength := SpinBox.new()
	strength.name = "BiomeTerrainBlendStrength"
	strength.min_value = 0.0
	strength.max_value = 4.0
	strength.step = 0.01
	strength.value = clampf(float(slot.get(&"strength")), 0.0, 4.0)
	strength.suffix = " ×"
	strength.tooltip_text = "How strongly this biome profile contributes after composition."
	strength.value_changed.connect(func(value: float) -> void:
		_session.stage_set(slot, &"strength", value, WorldAuthoringSession.ApplyScope.GRAPH,
			"Change biome terrain blend strength")
	)
	blend_row.add_child(strength)

	_phase47_build_profile_blend(box, terrain, graph, profile, biome_id)
	var remove := Button.new()
	remove.text = "Remove %s Profile" % BIOME_NAMES[biome_id]
	remove.pressed.connect(func() -> void:
		_session.remove_terrain_shader_slot(String(slot.get(&"slot_id")))
		_refresh_current_category()
	)
	box.add_child(remove)


func _phase47_add_biome_number(parent: VBoxContainer, label_text: String, profile: Dictionary,
		key: String, minimum: float, maximum: float, step: float, help_text: String,
		graph: Resource) -> void:
	var row := HBoxContainer.new()
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 220.0
	label.tooltip_text = help_text
	row.add_child(label)
	var spin := SpinBox.new()
	spin.name = "BiomeTerrain_%s" % key
	spin.min_value = minimum
	spin.max_value = maximum
	spin.step = step
	spin.value = clampf(float(profile.get(key, 0.0)), minimum, maximum)
	spin.value_changed.connect(func(value: float) -> void:
		profile[key] = roundi(value) if step >= 1.0 else value
		_phase47_stage_biome_profile(graph, profile, "Tune biome terrain: %s" % key)
	)
	row.add_child(spin)
	var help := Label.new()
	help.text = help_text
	help.modulate = Color(0.58, 0.68, 0.76)
	help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	help.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(help)


func _phase47_stage_biome_profile(graph: Resource, profile: Dictionary, action: String) -> void:
	_session.stage_action(action, func() -> void:
		_phase47_rebuild_biome_profile(graph, profile)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _phase47_build_profile_blend(parent: VBoxContainer, terrain: Resource, graph: Resource,
		profile: Dictionary, biome_id: int) -> void:
	var title := Label.new()
	title.text = "Blend another biome profile"
	title.add_theme_font_size_override("font_size", 16)
	parent.add_child(title)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	parent.add_child(row)
	var source := OptionButton.new()
	source.name = "BiomeProfileBlendSource"
	for candidate: int in BIOME_NAMES.size():
		if candidate == biome_id:
			continue
		source.add_item(BIOME_NAMES[candidate])
		source.set_item_metadata(source.item_count - 1, candidate)
		if candidate == _phase47_profile_blend_source:
			source.select(source.item_count - 1)
	if source.selected < 0 and source.item_count > 0:
		source.select(0)
		_phase47_profile_blend_source = int(source.get_item_metadata(0))
	source.item_selected.connect(func(index: int) -> void:
		_phase47_profile_blend_source = int(source.get_item_metadata(index))
	)
	row.add_child(source)
	var amount := SpinBox.new()
	amount.min_value = 0.0
	amount.max_value = 1.0
	amount.step = 0.05
	amount.value = _phase47_profile_blend_amount
	amount.suffix = " blend"
	amount.value_changed.connect(func(value: float) -> void: _phase47_profile_blend_amount = value)
	row.add_child(amount)
	var apply := Button.new()
	apply.name = "BlendBiomeTerrainProfile"
	apply.text = "Blend Profile"
	apply.tooltip_text = "Interpolates this profile's size, height and detail from the selected biome profile."
	apply.pressed.connect(func() -> void:
		var source_slot: Resource = _phase47_biome_profile_slot(terrain, _phase47_profile_blend_source)
		if source_slot == null:
			_set_status("%s has no terrain profile yet." % BIOME_NAMES[_phase47_profile_blend_source])
			return
		var source_profile := _phase47_biome_profile(source_slot.get(&"graph") as Resource)
		var mixed := profile.duplicate(true)
		for key: String in ["scale", "amount", "passes", "steps"]:
			mixed[key] = lerpf(float(profile.get(key, 0.0)), float(source_profile.get(key, 0.0)),
				_phase47_profile_blend_amount)
		mixed["type"] = source_profile.get("type", profile.get("type", "NOISE_LAYER")) \
			if _phase47_profile_blend_amount >= 0.5 else profile.get("type", "NOISE_LAYER")
		_phase47_stage_biome_profile(graph, mixed, "Blend biome terrain profiles")
	)
	row.add_child(apply)
	var hint := Label.new()
	hint.text = "This blends authored profile settings; the terrain runtime still uses the authoritative biome boundary for the final geographic transition."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.58, 0.68, 0.76)
	parent.add_child(hint)


func _phase47_build_repair_panel(graph: Resource) -> void:
	_section("Terrain setup needs repair")
	_add_note("This saved world has a custom or older base terrain graph, so the simple editor will not overwrite it silently.")
	var repair := Button.new()
	repair.name = "RestoreSimpleTerrainControls"
	repair.text = "Restore Simple Terrain Controls"
	repair.tooltip_text = "Replace only the Base Terrain graph with the standard production terrain controls."
	repair.pressed.connect(func() -> void:
		_session.stage_action("Restore simple terrain controls", func() -> void:
			PHASE47_NATIVE.build_canonical_graph(graph)
		, WorldAuthoringSession.ApplyScope.GRAPH)
		_refresh_current_category()
	)
	_workspace.add_child(repair)


func _phase47_has_production_controls(graph: Resource) -> bool:
	if graph == null:
		return false
	for control_key: String in PHASE47_SCHEMA.CONTROL_DEFAULTS:
		if _phase47_owner_node_id(graph, control_key).is_empty():
			return false
	return true


func _phase47_owner_node_id(graph: Resource, control_key: String) -> String:
	var owner_type: String = PHASE47_NATIVE.owner_node_type_for_control(control_key)
	if owner_type.is_empty() or graph == null:
		return ""
	var legacy_settings_id: String = ""
	for value: Variant in graph.get(&"nodes") as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		if String(node.get("type", "")) == owner_type:
			return String(node.get("id", ""))
		# Phase 31's still-supported production graph keeps all controls on one
		# settings node. It is the same production program, so accept it as a
		# compatibility representation instead of forcing an unnecessary migration.
		if String(node.get("type", "")) == PHASE47_NATIVE.SETTINGS_TYPE \
				and (node.get("parameters", {}) as Dictionary).has(control_key):
			legacy_settings_id = String(node.get("id", ""))
	return legacy_settings_id


func _phase47_control_value(graph: Resource, control_key: String) -> float:
	var node_id: String = _phase47_owner_node_id(graph, control_key)
	if node_id.is_empty():
		return float(PHASE47_SCHEMA.CONTROL_DEFAULTS.get(control_key, 0.0))
	for value: Variant in graph.get(&"nodes") as Array:
		var node: Dictionary = value as Dictionary
		if String(node.get("id", "")) == node_id:
			return float((node.get("parameters", {}) as Dictionary).get(control_key,
				PHASE47_SCHEMA.CONTROL_DEFAULTS.get(control_key, 0.0)))
	return float(PHASE47_SCHEMA.CONTROL_DEFAULTS.get(control_key, 0.0))


func _phase47_set_control(next_value: float, graph: Resource, control_key: String) -> void:
	var node_id: String = _phase47_owner_node_id(graph, control_key)
	if node_id.is_empty() or is_equal_approx(_phase47_control_value(graph, control_key), next_value):
		return
	_session.stage_action("Tune terrain: %s" % control_key, func() -> void:
		graph.call("set_node_parameter", node_id, control_key, next_value)
	, WorldAuthoringSession.ApplyScope.GRAPH)


func _phase47_apply_preset(graph: Resource, preset: Dictionary) -> void:
	var values: Dictionary = PHASE47_SCHEMA.control_defaults()
	for key_value: Variant in (preset.get("values", {}) as Dictionary).keys():
		values[String(key_value)] = (preset.get("values", {}) as Dictionary)[key_value]
	_session.stage_action("Apply terrain look: %s" % String(preset.get("label", "Natural")),
		func() -> void:
			for key_value: Variant in values.keys():
				var key: String = String(key_value)
				var node_id: String = _phase47_owner_node_id(graph, key)
				if not node_id.is_empty():
					graph.call("set_node_parameter", node_id, key, values[key])
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _phase47_reset_controls(graph: Resource) -> void:
	_phase47_apply_preset(graph, {"label":"Natural Defaults", "values":{}})


func _phase46_has_live_camera() -> bool:
	if _camera != null and is_instance_valid(_camera):
		return true
	if _player == null and _world_host != null:
		_player = _world_host.get("player") as Node
	if _player != null:
		_camera = _player.get("camera") as Camera3D
		if _camera != null:
			return true
	if not is_inside_tree():
		return false
	var viewport: Viewport = get_viewport()
	if viewport == null:
		return false
	_camera = viewport.get_camera_3d()
	return _camera != null


func _phase46_handle_directions(config: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL:
			var center := _phase46_direction_from_lat_lon(
				float(config.get("center_latitude_deg", 0.0)),
				float(config.get("center_longitude_deg", 0.0)))
			out["center"] = center
			out["radius"] = _phase46_offset_direction(
				center, float(config.get("radius_deg", 15.0)))
		PHASE46_GUIDED.AREA_RING:
			var center := _phase46_direction_from_lat_lon(
				float(config.get("center_latitude_deg", 0.0)),
				float(config.get("center_longitude_deg", 0.0)))
			out["center"] = center
			out["outer_radius"] = _phase46_offset_direction(
				center, float(config.get("outer_radius_deg", 20.0)), 0.0)
			out["inner_radius"] = _phase46_offset_direction(
				center, float(config.get("inner_radius_deg", 8.0)), PI * 0.5)
		PHASE46_GUIDED.AREA_REGION:
			var west: float = float(config.get("west_deg", -45.0))
			var east: float = float(config.get("east_deg", 45.0))
			var center_lon := _phase46_wrap_longitude(
				west + _phase46_eastward_span_deg(west, east) * 0.5)
			var center_lat := (float(config.get("south_deg", -30.0))
				+ float(config.get("north_deg", 30.0))) * 0.5
			out["center"] = _phase46_direction_from_lat_lon(center_lat, center_lon)
			out["north"] = _phase46_direction_from_lat_lon(
				float(config.get("north_deg", 30.0)), center_lon)
			out["south"] = _phase46_direction_from_lat_lon(
				float(config.get("south_deg", -30.0)), center_lon)
			out["west"] = _phase46_direction_from_lat_lon(center_lat, west)
			out["east"] = _phase46_direction_from_lat_lon(center_lat, east)
		PHASE46_GUIDED.AREA_LATITUDE:
			var facing: Vector2 = _phase46_camera_facing_lat_lon()
			var center_lat := (float(config.get("south_deg", -30.0))
				+ float(config.get("north_deg", 30.0))) * 0.5
			out["center"] = _phase46_direction_from_lat_lon(center_lat, facing.y)
			out["north"] = _phase46_direction_from_lat_lon(
				float(config.get("north_deg", 30.0)), facing.y)
			out["south"] = _phase46_direction_from_lat_lon(
				float(config.get("south_deg", -30.0)), facing.y)
		PHASE46_GUIDED.AREA_LONGITUDE:
			var facing: Vector2 = _phase46_camera_facing_lat_lon()
			var west: float = float(config.get("west_deg", -45.0))
			var east: float = float(config.get("east_deg", 45.0))
			var center_lon := _phase46_wrap_longitude(
				west + _phase46_eastward_span_deg(west, east) * 0.5)
			out["center"] = _phase46_direction_from_lat_lon(facing.x, center_lon)
			out["west"] = _phase46_direction_from_lat_lon(facing.x, west)
			out["east"] = _phase46_direction_from_lat_lon(facing.x, east)
	return out
