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

# A biome terrain profile is a short displacement chain: one base shape, then an
# optional water-erosion pass and an optional sediment-deposition pass. Every
# element is an ordinary serialized displacement node, so the profile keeps the
# exact all-ring CPU/GPU bytecode path of every other terrain edit.
const PHASE47_BIOME_BASE_TYPES: PackedStringArray = [
	"NOISE_LAYER", "RIDGED_MOUNTAINS", "TERRACE_RELIEF",
]
const PHASE47_BIOME_BASE_LABELS: PackedStringArray = [
	"Soft hills (FBM noise)", "Ridged peaks", "Terraced relief",
]
const PHASE47_EROSION_TYPE := "EROSION_CHANNELS"
const PHASE47_SEDIMENT_TYPE := "SEDIMENT_DEPOSIT"
# Superset used only when reading an existing graph, so a profile authored before
# the chain layout still resolves its base shape.
const PHASE47_BIOME_PROFILE_TYPES: PackedStringArray = [
	"NOISE_LAYER", "RIDGED_MOUNTAINS", "TERRACE_RELIEF",
	"EROSION_CHANNELS", "SEDIMENT_DEPOSIT",
]
# Every node type a valid biome-profile graph may contain. Anything else (e.g. a
# retired BILLOW_NOISE node left in a saved profile) makes the shared displacement
# compiler reject the whole terrain program, so such graphs are auto-repaired.
const PHASE47_BIOME_GRAPH_NODE_TYPES: PackedStringArray = [
	"CONSTANT_FLOAT", "OUTPUT_DISPLACEMENT",
	"NOISE_LAYER", "RIDGED_MOUNTAINS", "TERRACE_RELIEF",
	"EROSION_CHANNELS", "SEDIMENT_DEPOSIT",
]
const PHASE47_BIOME_LEGACY_TYPE_MAP: Dictionary = {
	"BILLOW_NOISE": "NOISE_LAYER",
	"VORONOI_RIDGES": "RIDGED_MOUNTAINS",
}
# The 15-clipmap-ring bytecode VM has 32 instructions. Every enabled biome profile
# costs ~4-6, so only this many customised profiles are kept compiled at once; the
# rest are turned off (never deleted) and can be re-enabled from their tab.
const PHASE47_MAX_ACTIVE_BIOME_PROFILES: int = 4

# Ported per-biome terrain character. The renderer already varies its geomorph
# landform weights by climate/soil and the surface shader already colours every
# biome; these defaults give the matching *physical detail* so a freshly
# provisioned biome profile reads as that biome instead of a flat placeholder.
#   base_type      - base shape node type
#   scale          - feature frequency (higher = smaller, more frequent)
#   amount         - base feature height in metres, before the blend strength
#   passes         - FBM octaves for the noise / ridged bases (1-4)
#   steps          - plateau count for the terraced base (2-24)
#   erosion        - water-erosion channel depth in metres (0 disables the pass)
#   sedimentation  - valley / basin infill height in metres (0 disables the pass)
const PHASE47_BIOME_TERRAIN_DEFAULTS: Dictionary = {
	0:  {"base_type":"NOISE_LAYER", "scale":4.0, "amount":6.0, "passes":2, "erosion":0.0, "sedimentation":4.0},
	1:  {"base_type":"NOISE_LAYER", "scale":4.0, "amount":5.0, "passes":2, "erosion":0.0, "sedimentation":6.0},
	2:  {"base_type":"TERRACE_RELIEF", "scale":2.6, "amount":16.0, "steps":5, "erosion":0.0, "sedimentation":24.0},
	3:  {"base_type":"NOISE_LAYER", "scale":5.0, "amount":18.0, "passes":3, "erosion":4.0, "sedimentation":14.0},
	4:  {"base_type":"NOISE_LAYER", "scale":6.0, "amount":24.0, "passes":3, "erosion":10.0, "sedimentation":10.0},
	5:  {"base_type":"NOISE_LAYER", "scale":5.0, "amount":22.0, "passes":3, "erosion":3.0, "sedimentation":8.0},
	6:  {"base_type":"NOISE_LAYER", "scale":4.0, "amount":16.0, "passes":2, "erosion":6.0, "sedimentation":10.0},
	7:  {"base_type":"NOISE_LAYER", "scale":6.0, "amount":26.0, "passes":3, "erosion":14.0, "sedimentation":12.0},
	8:  {"base_type":"NOISE_LAYER", "scale":7.0, "amount":30.0, "passes":4, "erosion":26.0, "sedimentation":16.0},
	9:  {"base_type":"RIDGED_MOUNTAINS", "scale":6.0, "amount":42.0, "passes":3, "erosion":12.0, "sedimentation":6.0},
	10: {"base_type":"NOISE_LAYER", "scale":3.5, "amount":12.0, "passes":2, "erosion":3.0, "sedimentation":9.0},
	11: {"base_type":"NOISE_LAYER", "scale":5.0, "amount":34.0, "passes":2, "erosion":0.0, "sedimentation":18.0},
	12: {"base_type":"NOISE_LAYER", "scale":4.0, "amount":16.0, "passes":2, "erosion":5.0, "sedimentation":12.0},
	13: {"base_type":"NOISE_LAYER", "scale":6.0, "amount":24.0, "passes":3, "erosion":18.0, "sedimentation":14.0},
	14: {"base_type":"NOISE_LAYER", "scale":7.0, "amount":30.0, "passes":4, "erosion":34.0, "sedimentation":20.0},
	15: {"base_type":"NOISE_LAYER", "scale":2.5, "amount":3.0, "passes":1, "erosion":0.0, "sedimentation":26.0},
	16: {"base_type":"TERRACE_RELIEF", "scale":5.0, "amount":72.0, "steps":6, "erosion":20.0, "sedimentation":4.0},
	17: {"base_type":"RIDGED_MOUNTAINS", "scale":8.0, "amount":70.0, "passes":4, "erosion":14.0, "sedimentation":0.0},
}

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
var _phase47_diag_probe_cache: Dictionary = {}


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

	# Runs on every Terrain-page build. Repairs retired nodes and keeps the enabled
	# biome-profile set to what the shared displacement VM can compile. Away from
	# the Biome Terrain tab every profile is disabled, so simply opening the editor
	# never triggers an authored-shader recompile from a stale multi-biome profile;
	# a profile is enabled only while you are looking at that biome.
	_phase47_migrate_biome_slots(terrain,
		clampi(_phase28_biome_id, 0, BIOME_NAMES.size() - 1)
		if _phase47_editor_tab == PHASE47_BIOME_TAB else -1)

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
	_add_note("Every biome already has a terrain profile. It is a small displacement layer — a base shape plus optional water-erosion and sediment passes — limited to that biome on every distance ring and composed after the shared Global Terrain. Editing it never adds a shader or an LOD copy; it starts from the character that biome already has.")
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
	var slot: Resource = _phase47_ensure_biome_profile(terrain, biome_id)
	if slot == null:
		_add_note("This body's terrain profile could not be provisioned.")
		return

	_phase47_build_biome_profile_controls(terrain, slot, biome_id)
	_phase47_build_biome_diagnostics(terrain, biome_id)


## Keep the compiled biome-profile set small and legal. Everything here mutates the
## staged slots in place and marks dirty ONCE at the end -- it must never call
## stage_action / remove_terrain_shader_slot per slot, each of which deep-copies
## and re-serialises the whole staged system (that froze the tab).
##  - repair graphs that still contain a retired node type (BILLOW_NOISE, ...);
##  - migrate old "strength 0" turn-offs to a disabled slot;
##  - keep only the selected biome + up to N edited biomes enabled; the 15-ring
##    32-instruction VM cannot compile more. Nothing is deleted, only disabled.
func _phase47_migrate_biome_slots(terrain: Resource, selected_biome: int) -> void:
	if terrain == null:
		return
	var changed: bool = false
	var enabled_customised: Array[Resource] = []
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = value as Resource
		if slot == null:
			continue
		var sid: String = String(slot.get(&"slot_id"))
		if not sid.begins_with(PHASE47_BIOME_PROFILE_PREFIX):
			continue
		var biome_id: int = sid.trim_prefix(PHASE47_BIOME_PROFILE_PREFIX).to_int()
		var graph: Resource = slot.get(&"graph") as Resource

		if graph != null and _phase47_graph_has_unsupported_node(graph):
			_phase47_rebuild_biome_profile(graph, _phase47_biome_profile(graph))
			changed = true

		if bool(slot.get(&"enabled")) and is_equal_approx(float(slot.get(&"strength")), 0.0):
			slot.set(&"enabled", false)
			slot.set(&"strength", 1.0)
			changed = true

		if not bool(slot.get(&"enabled")):
			continue
		var at_default: bool = _phase47_biome_profile_is_default(slot, biome_id)
		if biome_id == selected_biome:
			continue
		if at_default:
			# Auto-provisioned, never edited: disable it so only the biome you are
			# actually looking at carries its default character.
			slot.set(&"enabled", false)
			changed = true
		else:
			enabled_customised.append(slot)

	if enabled_customised.size() > PHASE47_MAX_ACTIVE_BIOME_PROFILES:
		for i: int in range(PHASE47_MAX_ACTIVE_BIOME_PROFILES, enabled_customised.size()):
			enabled_customised[i].set(&"enabled", false)
			changed = true

	if changed:
		terrain.call("ensure_valid")
		_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)


func _phase47_graph_has_unsupported_node(graph: Resource) -> bool:
	if graph == null:
		return false
	for value: Variant in graph.get(&"nodes") as Array:
		if value is Dictionary:
			if not PHASE47_BIOME_GRAPH_NODE_TYPES.has(
					String((value as Dictionary).get("type", ""))):
				return true
	return false


func _phase47_biome_profile_is_default(slot: Resource, biome_id: int) -> bool:
	if slot == null:
		return false
	if not is_equal_approx(float(slot.get(&"strength")), 1.0):
		return false
	if int(slot.get(&"blend_mode")) != SHADER_SLOT_MODEL.BlendMode.ADD:
		return false
	return _phase47_biome_profile_is_default_graph(slot.get(&"graph") as Resource, biome_id)


func _phase47_biome_profile_is_default_graph(graph: Resource, biome_id: int) -> bool:
	if graph == null:
		return false
	var got: Dictionary = _phase47_biome_profile(graph)
	var want: Dictionary = _phase47_biome_terrain_defaults(biome_id)
	for key: String in ["scale", "amount", "passes", "steps", "erosion", "sedimentation"]:
		if not is_equal_approx(float(got.get(key, 0.0)), float(want.get(key, 0.0))):
			return false
	return String(got.get("base_type", "")) == String(want.get("base_type", ""))


func _phase47_build_biome_diagnostics(terrain: Resource, biome_id: int) -> void:
	var panel := PanelContainer.new()
	panel.name = "BiomeTerrainDiagnostics"
	_workspace.add_child(panel)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	panel.add_child(box)
	var title := Label.new()
	title.text = "Live render diagnostics — updates ~2×/s"
	title.add_theme_font_size_override("font_size", 15)
	box.add_child(title)
	var readout := Label.new()
	readout.name = "BiomeTerrainDiagReadout"
	readout.add_theme_font_size_override("font_size", 12)
	readout.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	readout.custom_minimum_size = Vector2(520.0, 0.0)
	box.add_child(readout)
	readout.text = _phase47_biome_diag_text(terrain, biome_id)
	var timer := Timer.new()
	timer.wait_time = 1.0
	timer.autostart = true
	timer.timeout.connect(func() -> void:
		if is_instance_valid(readout):
			readout.text = _phase47_biome_diag_text(terrain, biome_id)
	)
	panel.add_child(timer)


func _phase47_biome_diag_text(terrain: Resource, biome_id: int) -> String:
	var lines: PackedStringArray = PackedStringArray()

	var all_ids := PackedStringArray()
	for sv: Variant in terrain.get(&"displacement_slots") as Array:
		if sv is Resource:
			all_ids.append(String((sv as Resource).get(&"slot_id")))
	lines.append("[0] displacement_slots: %s" % str(all_ids))

	var slot: Resource = _phase47_biome_profile_slot(terrain, biome_id)
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	var node_types := PackedStringArray()
	var output_node_id := ""
	var link_count := 0
	if graph != null:
		for value: Variant in graph.get(&"nodes") as Array:
			if value is Dictionary:
				var nd: Dictionary = value as Dictionary
				node_types.append(String(nd.get("type", "?")))
				if String(nd.get("type", "")) == "OUTPUT_DISPLACEMENT":
					output_node_id = String(nd.get("id", ""))
		link_count = (graph.get(&"links") as Array).size()
	var output_wired := false
	if graph != null and not output_node_id.is_empty():
		for lv: Variant in graph.get(&"links") as Array:
			if lv is Dictionary and String((lv as Dictionary).get("to", "")) == output_node_id:
				output_wired = true
	lines.append("[1] slot '%s%d': %s  strength=%s  blend=%s  mask_mode=%s  biome_ids=%s" % [
		PHASE47_BIOME_PROFILE_PREFIX, biome_id,
		("FOUND enabled=%s" % slot.get(&"enabled")) if slot != null else "MISSING",
		("%.2f" % float(slot.get(&"strength"))) if slot != null else "-",
		int(slot.get(&"blend_mode")) if slot != null else -1,
		int(slot.get(&"biome_mask_mode")) if slot != null else -1,
		str(slot.get(&"biome_ids")) if slot != null else "-"])
	lines.append("    graph: %d nodes %s, %d links, output_mode=%s | OUTPUT node=%s, output wired=%s | rev=%s" % [
		node_types.size(), str(node_types), link_count,
		int(graph.get(&"displacement_output_mode")) if graph != null else -1,
		("yes" if not output_node_id.is_empty() else "MISSING"), output_wired,
		int(graph.get(&"revision")) if graph != null else -1])

	var renderer: Node = get_tree().root.get_node_or_null("/root/GroundGeometryClipmap")
	if renderer == null:
		lines.append("[2] renderer /root/GroundGeometryClipmap: NOT FOUND")
		return "\n".join(lines)
	var rt: Resource = null
	if renderer.has_method("_active_authoring_terrain"):
		rt = renderer.call("_active_authoring_terrain") as Resource
	lines.append("[2] renderer authoring terrain: %s   editor-staged match: %s" % [
		"present" if rt != null else "NULL", rt == terrain])
	lines.append("    blank_backend=%s  blank_shader=%s  authored_shader_active=%s  fp=%s" % [
		renderer.call("_blank_backend") if renderer.has_method("_blank_backend") else "?",
		renderer.get("_blank_shader_active"), renderer.get("_authored_shader_active"),
		str(renderer.get("_displacement_fingerprint")).substr(0, 40)])

	var mat: ShaderMaterial = renderer.get("_material") as ShaderMaterial
	if mat != null and mat.shader != null:
		var code: String = mat.shader.code
		lines.append("[3] material shader: name='%s'  has author_procedural_sample=%s  has #define=%s" % [
			mat.shader.resource_name,
			code.find("author_procedural_sample") >= 0,
			code.find("#define ASTERRA_AUTHORED_TERRAIN") >= 0])
		lines.append("    u_author_disp_ready=%s  count=%s  output=%s  u_ctx_ready=%s  cache_ready=%s  stable_disp=%s  height_enabled=%s" % [
			mat.get_shader_parameter("u_author_disp_ready"),
			mat.get_shader_parameter("u_author_disp_count"),
			mat.get_shader_parameter("u_author_disp_output"),
			mat.get_shader_parameter("u_ctx_ready"),
			mat.get_shader_parameter("u_terrain_cache_ready"),
			mat.get_shader_parameter("u_stable_displacement"),
			mat.get_shader_parameter("u_height_enabled")])
	else:
		lines.append("[3] material shader: NULL")

	var disp: Node = get_tree().get_first_node_in_group(&"terrain_displacement_runtime")
	if disp != null and disp.has_method("stats"):
		var st: Dictionary = disp.call("stats")
		lines.append("[4] disp runtime %s: active=%s  instructions=%s  output_index=%s" % [
			(disp.get_script() as Script).resource_path.get_file(),
			st.get("active"), st.get("instructions", st.get("instruction_count", -1)),
			st.get("output_index", "?")])
		lines.append("    compile warnings: %s" % str(st.get("warnings", [])))
		lines.append("    candidate_valid=%s  rejected_candidates=%s" % [
			st.get("candidate_valid", "?"), st.get("rejected_candidates", "?")])
		lines.append("    REJECT REASON: %s" % str(st.get("candidate_warnings", [])))
		lines.append("    active_fp=%s" % str(st.get("active_fingerprint", "?")).substr(0, 120))
		lines.append("    attempt_fp=%s" % str(st.get("attempt_fingerprint", "?")).substr(0, 120))
	else:
		lines.append("[4] disp runtime: NOT in group 'terrain_displacement_runtime'")

	var probe: Vector3 = _phase47_biome_probe_dir(biome_id) if _phase47_diag_can_sample() else Vector3.ZERO
	if probe != Vector3.ZERO and disp != null and disp.has_method("evaluate_height"):
		var on_b: float = float(disp.call("evaluate_height", probe, 0.0, 0, biome_id, NAN, 0.0))
		var other: int = (biome_id + 3) % BIOME_NAMES.size()
		var off_b: float = float(disp.call("evaluate_height", probe, 0.0, 0, other, NAN, 0.0))
		lines.append("[5] CPU evaluate_height on a %s point: this-biome=%.2f m  other-biome=%.2f m" % [
			BIOME_NAMES[biome_id], on_b, off_b])
		lines.append("    (this-biome must be non-zero AND the shader flags above must be true for the render to move)")
	else:
		lines.append("[5] CPU eval: no %s land point found near you — fly over that biome" % BIOME_NAMES[biome_id])

	var pdir: Vector3 = _phase47_diag_player_dir()
	if pdir != Vector3.ZERO and _phase47_diag_can_sample():
		var info: Dictionary = Planet.call("sample_info", pdir)
		var here: int = int(info.get("biome", -1))
		lines.append("[6] you are on: %s (%d) — %s" % [
			str(info.get("biome_name", "?")), here,
			"SELECTED biome, edits show here" if here == biome_id
			else "NOT selected; pick '%s' or fly to a %s area" % [
				str(info.get("biome_name", "?")), BIOME_NAMES[biome_id]]])
	return "\n".join(lines)


func _phase47_diag_player_dir() -> Vector3:
	if _player == null and _world_host != null:
		_player = _world_host.get("player") as Node
	if _player == null:
		return Vector3.ZERO
	if _player.has_method("up_dir"):
		var d: Variant = _player.call("up_dir")
		if d is Vector3 and (d as Vector3).length_squared() > 0.5:
			return (d as Vector3).normalized()
	var wp: Variant = _player.get("world_pos")
	if wp != null and wp.has_method("normalized"):
		var n: Variant = wp.normalized()
		if n is Object and n.has_method("to_v3"):
			return n.to_v3()
	return Vector3.ZERO


func _phase47_diag_can_sample() -> bool:
	return Planet != null and bool(Planet.get("ready_state")) \
		and Planet.get("fields") != null and Planet.has_method("sample_info")


func _phase47_biome_probe_dir(biome_id: int) -> Vector3:
	if _phase47_diag_probe_cache.has(biome_id):
		return _phase47_diag_probe_cache[biome_id]
	if not _phase47_diag_can_sample():
		return Vector3.ZERO
	# Prefer somewhere near the player, then fall back to a bounded global scan.
	var candidates: Array[Vector3] = []
	var here: Vector3 = _phase47_diag_player_dir()
	if here != Vector3.ZERO:
		var basis: Array = CubeSphere.tangent_basis(here)
		var east: Vector3 = basis[0]
		var north: Vector3 = basis[1]
		for i: int in 24:
			var ang: float = TAU * float(i) / 24.0
			for r: float in [0.02, 0.06, 0.14]:
				candidates.append((here + (east * cos(ang) + north * sin(ang)) * r).normalized())
	var rng := RandomNumberGenerator.new()
	rng.seed = 1234 + biome_id
	for _i: int in 120:
		candidates.append(Vector3(rng.randfn(), rng.randfn(), rng.randfn()).normalized())
	for d: Vector3 in candidates:
		if d.length_squared() < 0.5:
			continue
		var info: Dictionary = Planet.call("sample_info", d)
		if int(info.get("biome", -1)) == biome_id:
			_phase47_diag_probe_cache[biome_id] = d
			return d
	_phase47_diag_probe_cache[biome_id] = Vector3.ZERO
	return Vector3.ZERO


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


func _phase47_ensure_biome_profile(terrain: Resource, biome_id: int) -> Resource:
	# The profile for every biome is provisioned on demand rather than created by
	# a button press. It is a normal scoped displacement slot, so it travels the
	# identical all-ring cache / contact path as the Global Terrain edit.
	var slot: Resource = _phase47_biome_profile_slot(terrain, biome_id)
	if slot != null:
		# The recovery loader and the away-from-tab migration both disable biome
		# profiles to keep the shared displacement VM compilable. The profile you
		# are actually viewing must be live so edits reload the terrain, so always
		# re-enable the selected biome's slot here.
		if not bool(slot.get(&"enabled")):
			slot.set(&"enabled", true)
			terrain.call("ensure_valid")
			_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)
		return slot
	if terrain == null:
		return null
	slot = terrain.call("create_shader_slot", SHADER_SLOT_MODEL.Domain.DISPLACEMENT,
		"%s Terrain" % BIOME_NAMES[biome_id]) as Resource
	if slot == null:
		return null
	slot.set(&"slot_id", "%s%d" % [PHASE47_BIOME_PROFILE_PREFIX, biome_id])
	slot.set(&"display_name", "%s Terrain" % BIOME_NAMES[biome_id])
	slot.set(&"enabled", true)
	slot.set(&"clipmap_level_mask", PHASE47_ALL_RINGS_MASK)
	slot.set(&"biome_mask_mode", SHADER_SLOT_MODEL.BiomeMaskMode.ONLY)
	slot.set(&"biome_ids", PackedInt32Array([biome_id]))
	slot.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD)
	slot.set(&"strength", 1.0)
	_phase47_rebuild_biome_profile(slot.get(&"graph") as Resource,
		_phase47_biome_terrain_defaults(biome_id))
	terrain.call("ensure_valid")
	_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)
	return slot


func _phase47_default_biome_profile() -> Dictionary:
	return {
		"base_type":"NOISE_LAYER", "scale":6.0, "amount":35.0, "passes":3, "steps":6,
		"seed":1337, "erosion":0.0, "erosion_scale":8.0,
		"sedimentation":0.0, "sediment_scale":3.0,
	}


func _phase47_biome_terrain_defaults(biome_id: int) -> Dictionary:
	var profile: Dictionary = _phase47_default_biome_profile()
	profile["seed"] = 1337 + biome_id * 101
	var ported: Dictionary = PHASE47_BIOME_TERRAIN_DEFAULTS.get(biome_id, {}) as Dictionary
	for key: String in ["base_type", "scale", "amount", "passes", "steps",
			"erosion", "sedimentation"]:
		if ported.has(key):
			profile[key] = ported[key]
	return profile


func _phase47_biome_profile(graph: Resource) -> Dictionary:
	var result: Dictionary = _phase47_default_biome_profile()
	if graph == null:
		return result
	for value: Variant in graph.get(&"nodes") as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if PHASE47_BIOME_LEGACY_TYPE_MAP.has(node_type):
			node_type = String(PHASE47_BIOME_LEGACY_TYPE_MAP[node_type])
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		if PHASE47_BIOME_BASE_TYPES.has(node_type):
			result["base_type"] = node_type
			result["scale"] = float(parameters.get("scale", result["scale"]))
			result["amount"] = float(parameters.get("amount", result["amount"]))
			result["passes"] = int(parameters.get("passes", result["passes"]))
			result["steps"] = int(parameters.get("steps", result["steps"]))
			result["seed"] = int(parameters.get("seed", result["seed"]))
		elif node_type == PHASE47_EROSION_TYPE:
			result["erosion"] = float(parameters.get("amount", 0.0))
			result["erosion_scale"] = float(parameters.get("scale", result["erosion_scale"]))
		elif node_type == PHASE47_SEDIMENT_TYPE:
			result["sedimentation"] = float(parameters.get("amount", 0.0))
			result["sediment_scale"] = float(parameters.get("scale", result["sediment_scale"]))
	return result


func _phase47_rebuild_biome_profile(graph: Resource, profile: Dictionary) -> void:
	if graph == null:
		return
	var base_type: String = String(profile.get("base_type", "NOISE_LAYER"))
	if not PHASE47_BIOME_BASE_TYPES.has(base_type):
		base_type = "NOISE_LAYER"
	graph.call("create_default_graph", SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
	var output_id: String = ""
	for value: Variant in graph.get(&"nodes") as Array:
		var node: Dictionary = value as Dictionary
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = String(node.get("id", ""))
			break
	if output_id.is_empty():
		return
	var seed: int = int(profile.get("seed", 1337))
	var cursor: String = String(graph.call("add_node", "CONSTANT_FLOAT", Vector2(70.0, 180.0),
		{"value":0.0}))
	var base_id: String = String(graph.call("add_node", base_type, Vector2(360.0, 180.0), {
		"scale":maxf(0.1, float(profile.get("scale", 6.0))),
		"amount":maxf(0.0, float(profile.get("amount", 35.0))),
		"passes":clampi(int(profile.get("passes", 3)), 1, 4),
		"seed":seed,
		"steps":clampi(int(profile.get("steps", 6)), 2, 24),
	}))
	graph.call("connect_nodes", cursor, 0, base_id, 0)
	cursor = base_id
	var column: float = 640.0
	var erosion: float = maxf(0.0, float(profile.get("erosion", 0.0)))
	if erosion > 0.0:
		var erosion_id: String = String(graph.call("add_node", PHASE47_EROSION_TYPE,
			Vector2(column, 180.0), {
				"scale":maxf(0.1, float(profile.get("erosion_scale", 8.0))),
				"amount":erosion, "passes":3, "seed":seed + 7919,
			}))
		graph.call("connect_nodes", cursor, 0, erosion_id, 0)
		cursor = erosion_id
		column += 280.0
	var sedimentation: float = maxf(0.0, float(profile.get("sedimentation", 0.0)))
	if sedimentation > 0.0:
		var sediment_id: String = String(graph.call("add_node", PHASE47_SEDIMENT_TYPE,
			Vector2(column, 180.0), {
				"scale":maxf(0.1, float(profile.get("sediment_scale", 3.0))),
				"amount":sedimentation, "passes":3, "seed":seed + 104729,
			}))
		graph.call("connect_nodes", cursor, 0, sediment_id, 0)
		cursor = sediment_id
	graph.call("connect_nodes", cursor, 0, output_id, 0)


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
	type_label.text = "Base shape / noise type"
	type_label.custom_minimum_size.x = 220.0
	type_row.add_child(type_label)
	var type_picker := OptionButton.new()
	type_picker.name = "BiomeTerrainShape"
	for index: int in PHASE47_BIOME_BASE_TYPES.size():
		type_picker.add_item(PHASE47_BIOME_BASE_LABELS[index])
		type_picker.set_item_metadata(index, PHASE47_BIOME_BASE_TYPES[index])
		if PHASE47_BIOME_BASE_TYPES[index] == String(profile["base_type"]):
			type_picker.select(index)
	type_picker.item_selected.connect(func(index: int) -> void:
		profile["base_type"] = String(type_picker.get_item_metadata(index))
		_phase47_stage_biome_profile(graph, profile, "Change biome terrain shape")
	)
	type_row.add_child(type_picker)

	_phase47_add_biome_number(box, "Feature size", profile, "scale", 0.1, 100.0, 0.1,
		"Higher values make the pattern smaller and more frequent.", graph)
	_phase47_add_biome_number(box, "Feature height", profile, "amount", 0.0, 1000.0, 1.0,
		"Height of the base shape in metres, before the profile blend strength.", graph)
	if String(profile["base_type"]) == "TERRACE_RELIEF":
		_phase47_add_biome_number(box, "Terrace steps", profile, "steps", 2.0, 24.0, 1.0,
			"More steps makes smaller terraces; fewer steps makes bold plateaus.", graph)
	else:
		_phase47_add_biome_number(box, "Detail passes", profile, "passes", 1.0, 4.0, 1.0,
			"More passes adds finer nested structure.", graph)
	_phase47_add_biome_number(box, "Pattern seed", profile, "seed", 0.0, 999999.0, 1.0,
		"Changes the pattern without changing the biome boundary.", graph)

	var process_label := Label.new()
	process_label.text = "Surface processes"
	process_label.add_theme_font_size_override("font_size", 16)
	box.add_child(process_label)
	_phase47_add_biome_number(box, "Water erosion", profile, "erosion", 0.0, 400.0, 1.0,
		"Carves branching drainage channels into this biome, in metres. 0 disables the erosion pass.", graph)
	_phase47_add_biome_number(box, "Erosion feature size", profile, "erosion_scale", 0.1, 100.0, 0.1,
		"Spacing of the erosion channel network. Higher values make a finer, denser network.", graph)
	_phase47_add_biome_number(box, "Sedimentation", profile, "sedimentation", 0.0, 400.0, 1.0,
		"Fills valley floors and basins with deposited material, in metres. 0 disables the sediment pass.", graph)
	_phase47_add_biome_number(box, "Sediment feature size", profile, "sediment_scale", 0.1, 100.0, 0.1,
		"Spacing of the sediment fans. Higher values make smaller, more frequent deposits.", graph)

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
	var reset := Button.new()
	reset.name = "ResetBiomeTerrainProfile"
	reset.text = "Reset %s Terrain to Default" % BIOME_NAMES[biome_id]
	reset.tooltip_text = "Restore this biome's ported terrain character and its Add / 1× composition."
	reset.pressed.connect(func() -> void:
		_session.stage_action("Reset %s terrain profile" % BIOME_NAMES[biome_id], func() -> void:
			slot.set(&"blend_mode", SHADER_SLOT_MODEL.BlendMode.ADD)
			slot.set(&"strength", 1.0)
			_phase47_rebuild_biome_profile(slot.get(&"graph") as Resource,
				_phase47_biome_terrain_defaults(biome_id))
		, WorldAuthoringSession.ApplyScope.GRAPH)
		_refresh_current_category()
	)
	box.add_child(reset)
	var budget := Label.new()
	budget.text = "Live biome profiles: %d / %d (the biome you edit is always live; " \
		% [_phase47_active_biome_profile_count(terrain), PHASE47_MAX_ACTIVE_BIOME_PROFILES + 1] \
		+ "extra customised biomes past the limit stop rendering until you visit them). " \
		+ "Use Reset to Default to give a biome plain terrain again."
	budget.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	budget.modulate = Color(0.58, 0.68, 0.76)
	box.add_child(budget)


func _phase47_active_biome_profile_count(terrain: Resource) -> int:
	var count: int = 0
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = value as Resource
		if slot == null:
			continue
		if String(slot.get(&"slot_id")).begins_with(PHASE47_BIOME_PROFILE_PREFIX) \
				and bool(slot.get(&"enabled")):
			count += 1
	return count


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
		for key: String in ["scale", "amount", "passes", "steps",
				"erosion", "erosion_scale", "sedimentation", "sediment_scale"]:
			mixed[key] = lerpf(float(profile.get(key, 0.0)), float(source_profile.get(key, 0.0)),
				_phase47_profile_blend_amount)
		mixed["base_type"] = source_profile.get("base_type", profile.get("base_type", "NOISE_LAYER")) \
			if _phase47_profile_blend_amount >= 0.5 else profile.get("base_type", "NOISE_LAYER")
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
