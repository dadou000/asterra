extends "res://scripts/world_authoring/world_authoring_editor_live_phase43.gd"
## Phase 44: complete the default simplified Terrain page with production Surface.
##
## Geometry and material presentation remain thin views over the same authoritative
## profile/graph resources. Advanced mode keeps the existing LOD/scope composer.

const PHASE44_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase44.gd")
const SIMPLE_TAB_SURFACE: int = 2


func _build_terrain_shader_composer_page() -> void:
	if _phase43_advanced_terrain:
		super._build_terrain_shader_composer_page()
		return

	_page_title("Terrain",
		"Shape the planet and its materials directly. Base Landscape changes global terrain character; Local Features place geometry changes geographically; Surface controls the production materials.")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return

	_phase29_ensure_production_graphs(terrain)
	_phase43_add_mode_bar(false)
	_phase44_build_simple_tabs()
	match _phase43_simple_tab:
		SIMPLE_TAB_FEATURES:
			_phase43_build_local_features(terrain)
		SIMPLE_TAB_SURFACE:
			_phase44_build_surface(terrain)
		_:
			_phase43_build_base_landscape(terrain)


func _phase44_build_simple_tabs() -> void:
	var tabs := HBoxContainer.new()
	tabs.name = "TerrainSimpleTabs"
	tabs.add_theme_constant_override("separation", 6)
	tabs.custom_minimum_size.y = 48.0
	_workspace.add_child(tabs)
	for item: Dictionary in [
		{"label":"BASE LANDSCAPE", "tab":SIMPLE_TAB_BASE,
			"tip":"Global mountains, valleys, broad shape and fine detail."},
		{"label":"LOCAL FEATURES", "tab":SIMPLE_TAB_FEATURES,
			"tip":"Add a terrain effect and choose exactly where it applies."},
		{"label":"SURFACE", "tab":SIMPLE_TAB_SURFACE,
			"tip":"Rock, soil, vegetation, snow, colors, roughness and production surface detail."},
	]:
		var button := Button.new()
		button.text = String(item["label"])
		button.toggle_mode = true
		button.button_pressed = _phase43_simple_tab == int(item["tab"])
		button.custom_minimum_size = Vector2(230.0, 44.0)
		button.tooltip_text = String(item["tip"])
		var tab_value: int = int(item["tab"])
		button.pressed.connect(func() -> void:
			_phase43_simple_tab = tab_value
			_refresh_current_category()
		)
		tabs.add_child(button)


func _phase44_build_surface(terrain: Resource) -> void:
	_section("Surface")
	_add_note("These controls edit the real production Surface graph: material classification, palette, microrelief, anti-tiling, procedural rock PBR and scanned PBR. They do not create a simplified replacement shader.")
	var surface_slot: Resource = terrain.call("find_shader_slot", PRODUCTION_SURFACE_SLOT_ID) as Resource
	if surface_slot == null:
		_add_note("Base Surface is missing from the active terrain profile.")
		return

	var editor := PHASE44_GRAPH_EDITOR.new()
	editor.name = "SimpleTerrainSurfaceEditor"
	editor.custom_minimum_size = Vector2(1120.0, 760.0)
	_workspace.add_child(editor)
	editor.call_deferred("setup", _session, surface_slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.text = "Simple changes the most useful material balances and visual controls. Detailed adds physical classifier thresholds, texture projection sizes and key geology controls. Node Graph exposes every production Surface setting and connection."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)


func _phase29_build_graph_editor(slot: Resource) -> void:
	# Advanced mode uses the same Phase 44 editor so the production Surface can move
	# between Simple/Detailed/Node Graph while guided displacement features retain
	# the Phase 43 Simple/Node Graph presentation inherited underneath it.
	var graph_editor := PHASE44_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))

	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "Advanced mode exposes LOD, biome scope and complete Node Graph routing. Guided local features remain ordinary displacement graphs; Base Terrain retains its safe native-stage lowering boundary."
	else:
		hint.text = "Advanced Surface exposes the complete production PBR outputs, classifier thresholds, palette/materials, microrelief, anti-tiling, procedural rock PBR, scanned PBR, textures, world fields and graph math."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
