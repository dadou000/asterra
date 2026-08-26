class_name TerrainDebug
extends CanvasLayer
## Debug controller for the corrected latest-0.0.5 GPU terrain stack.
##
## The base DebugMenu has newer sky/exposure tools than the old GPU branch. To
## avoid replacing that file, this controller injects its GPU-only controls into
## the existing TerrainControls container after both nodes are ready.

const GEOMORPH_MODE_NAMES := [
	"Final material",
	"Landforms — R mountain / G arid / B glacial / white deposition",
	"Primary materials — rock / soil / vegetation / sand",
	"Secondary materials — mud / snow / scree / gravel",
	"Soil context — sand / silt / clay, organic darkening",
	"Geology / biome — rock ID / biome ID / erodibility",
	"Hydrology — flow direction / discharge / deposition",
]

var terrain: PlanetTerrain # retained because main.gd assigns it

var wireframe := false: set = set_wireframe
var freeze_terrain := false: set = set_freeze_terrain
var side_cut := false: set = set_side_cut
var stable_displacement := true: set = set_stable_displacement
var stable_ocean_displacement := true: set = set_stable_ocean_displacement
var microrelief := true: set = set_microrelief
var pbr_detail := true: set = set_pbr_detail
var gpu_scatter := true: set = set_gpu_scatter
var sink_scale := 2.0: set = set_sink_scale
var geomorph_mode := 0: set = set_geomorph_mode
var aerial_strength := 0.78: set = set_aerial_strength

var _wireframes_ready := false
var _status: Label
var _gpu_controls_installed := false
var _gpu_status_label: Label
var _aerial_label: Label
var _gpu_status_accum := 0.0


func _ready() -> void:
	layer = 19
	process_priority = 20
	_status = Label.new()
	_status.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_status.position = Vector2(-510, 18)
	_status.custom_minimum_size = Vector2(490, 0)
	_status.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_status.add_theme_font_size_override("font_size", 13)
	_status.add_theme_color_override("font_color", Color(1.0, 0.78, 0.28))
	_status.visible = false
	add_child(_status)
	_update_status()
	call_deferred("_install_gpu_debug_controls")


func _process(delta: float) -> void:
	if not _gpu_controls_installed:
		return
	_gpu_status_accum += delta
	if _gpu_status_accum >= 0.25:
		_gpu_status_accum = fmod(_gpu_status_accum, 0.25)
		_refresh_gpu_control_status()


func _clipmap() -> Node:
	return get_node_or_null("/root/GroundGeometryClipmap")


func _scatter() -> Node:
	return get_node_or_null("/root/TerrainScatter")


func _install_gpu_debug_controls() -> void:
	if _gpu_controls_installed:
		return
	var main: Node = get_parent()
	if main == null:
		return
	var menu: Node = null
	for child: Node in main.get_children():
		if child is DebugMenu:
			menu = child
			break
	if menu == null:
		return
	var controls := menu.find_child("TerrainControls", true, false)
	if not (controls is VBoxContainer):
		return
	var box := controls as VBoxContainer

	box.add_child(HSeparator.new())
	var title := Label.new()
	title.text = "GPU terrain synthesis"
	title.add_theme_font_size_override("font_size", 14)
	title.add_theme_color_override("font_color", Color(0.86, 0.91, 1.0))
	box.add_child(title)

	_gpu_status_label = Label.new()
	_gpu_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_gpu_status_label.add_theme_font_size_override("font_size", 11)
	_gpu_status_label.add_theme_color_override("font_color", Color(0.58, 0.72, 0.78))
	box.add_child(_gpu_status_label)

	var selector := OptionButton.new()
	selector.custom_minimum_size = Vector2(0, 34)
	selector.add_theme_font_size_override("font_size", 13)
	for mode_name: String in GEOMORPH_MODE_NAMES:
		selector.add_item(mode_name)
	selector.select(geomorph_mode)
	selector.item_selected.connect(func(index: int): set_geomorph_mode(index))
	box.add_child(selector)
	box.add_child(_menu_note("Diagnostic views are emissive/unlit and inspect the actual GPU context/material fields."))

	_add_gpu_toggle(box, "GPU terrain scatter", gpu_scatter,
		"Grass and geological/river stones. Compute-compacted on supported rendering methods.",
		func(value: bool): set_gpu_scatter(value))
	_add_gpu_toggle(box, "Near-field geometric microrelief", microrelief,
		"Dense 1/4-L0 displacement is active only while the promoted centre is L0.",
		func(value: bool): set_microrelief(value))
	_add_gpu_toggle(box, "Scanned PBR detail", pbr_detail,
		"Close triplanar albedo/roughness/normal scans; geomorph and classifier remain active when disabled.",
		func(value: bool): set_pbr_detail(value))

	_aerial_label = Label.new()
	_aerial_label.add_theme_font_size_override("font_size", 13)
	box.add_child(_aerial_label)
	var aerial_slider := HSlider.new()
	aerial_slider.min_value = 0.0
	aerial_slider.max_value = 2.0
	aerial_slider.step = 0.05
	aerial_slider.value = aerial_strength
	aerial_slider.custom_minimum_size = Vector2(0, 28)
	aerial_slider.value_changed.connect(func(value: float): set_aerial_strength(value))
	box.add_child(aerial_slider)
	box.add_child(_menu_note("Optical-path terrain veil. Near ground remains clear; contrast is progressively lost toward the horizon."))

	_gpu_controls_installed = true
	_update_aerial_label()
	_refresh_gpu_control_status()


func _menu_note(text: String) -> Label:
	var label := Label.new()
	label.text = "     %s" % text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.add_theme_font_size_override("font_size", 11)
	label.add_theme_color_override("font_color", Color(0.58, 0.65, 0.76))
	return label


func _add_gpu_toggle(box: VBoxContainer, text: String, initial: bool,
		note: String, callback: Callable) -> void:
	var button := CheckButton.new()
	button.text = text
	button.button_pressed = initial
	button.custom_minimum_size = Vector2(0, 34)
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.add_theme_font_size_override("font_size", 13)
	button.toggled.connect(callback)
	box.add_child(button)
	box.add_child(_menu_note(note))


func _refresh_gpu_control_status() -> void:
	if _gpu_status_label == null:
		return
	var clipmap: Node = _clipmap()
	var parts := PackedStringArray()
	if clipmap != null and clipmap.has_method("gpu_stream_stats"):
		var gs: Dictionary = clipmap.call("gpu_stream_stats")
		parts.append("logical L%d–L%d" % [
			int(gs.get("active_min_level", 0)), int(gs.get("active_max_level", 0))])
		parts.append("physical rings %d" % int(gs.get("physical_ring_instances", 0)))
		parts.append("horizon max L%d" % int(gs.get("horizon_max_level", 0)))
		parts.append("micro %s" % ("ON" if bool(gs.get("micro_active", false)) else "OFF"))
	var scatter: Node = _scatter()
	if scatter != null and scatter.has_method("gpu_scatter_stats"):
		var ss: Dictionary = scatter.call("gpu_scatter_stats")
		parts.append("scatter %s" % (
			"COMPUTE" if bool(ss.get("compute_ready", false)) else "FALLBACK"))
	_gpu_status_label.text = "     %s" % "  •  ".join(parts)


func set_wireframe(value: bool) -> void:
	wireframe = value
	var viewport := get_viewport()
	if viewport == null:
		return
	if value and not _wireframes_ready:
		RenderingServer.set_debug_generate_wireframes(true)
		_wireframes_ready = true
		var clipmap: Node = _clipmap()
		if clipmap != null and clipmap.has_method("rebuild_static_topology"):
			clipmap.rebuild_static_topology()
	viewport.debug_draw = Viewport.DEBUG_DRAW_WIREFRAME if value else Viewport.DEBUG_DRAW_DISABLED
	_update_status()


func set_freeze_terrain(value: bool) -> void:
	freeze_terrain = value
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_freeze"):
		clipmap.set_debug_freeze(value)
	_update_status()


func set_side_cut(value: bool) -> void:
	side_cut = value
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_side_cut"):
		clipmap.set_debug_side_cut(value)
	_update_status()


func set_stable_displacement(value: bool) -> void:
	stable_displacement = value
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_stable_displacement"):
		clipmap.set_debug_stable_displacement(value)
	_update_status()


func set_stable_ocean_displacement(value: bool) -> void:
	stable_ocean_displacement = value
	var ocean: Node = get_node_or_null("/root/OceanSystem")
	if ocean != null and ocean.has_method("set_debug_stable_displacement"):
		ocean.set_debug_stable_displacement(value)
	_update_status()


func set_microrelief(value: bool) -> void:
	microrelief = value
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_microrelief_enabled"):
		clipmap.set_debug_microrelief_enabled(value)
	_update_status()


func set_pbr_detail(value: bool) -> void:
	pbr_detail = value
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_pbr_enabled"):
		clipmap.set_debug_pbr_enabled(value)
	_update_status()


func set_gpu_scatter(value: bool) -> void:
	gpu_scatter = value
	var scatter: Node = _scatter()
	if scatter != null and scatter.has_method("set_debug_enabled"):
		scatter.call("set_debug_enabled", value)
	_update_status()


func set_sink_scale(value: float) -> void:
	sink_scale = clampf(value, 0.0, 16.0)
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_sink_scale"):
		clipmap.set_debug_sink_scale(sink_scale)
	_update_status()


func set_geomorph_mode(value: int) -> void:
	geomorph_mode = clampi(value, 0, GEOMORPH_MODE_NAMES.size() - 1)
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_geomorph_mode"):
		clipmap.set_debug_geomorph_mode(geomorph_mode)
	_update_status()


func set_aerial_strength(value: float) -> void:
	aerial_strength = clampf(value, 0.0, 2.0)
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_aerial_strength"):
		clipmap.set_aerial_strength(aerial_strength)
	_update_aerial_label()
	_update_status()


func _update_aerial_label() -> void:
	if _aerial_label != null:
		_aerial_label.text = "Terrain aerial veil: %.2f ×" % aerial_strength


func reset_inspection() -> void:
	freeze_terrain = false
	side_cut = false
	stable_displacement = true
	stable_ocean_displacement = true
	microrelief = true
	pbr_detail = true
	gpu_scatter = true
	sink_scale = 2.0
	geomorph_mode = 0
	aerial_strength = 0.78
	var clipmap: Node = _clipmap()
	if clipmap != null:
		if clipmap.has_method("set_debug_freeze"):
			clipmap.set_debug_freeze(false)
		if clipmap.has_method("set_debug_side_cut"):
			clipmap.set_debug_side_cut(false)
		if clipmap.has_method("set_debug_stable_displacement"):
			clipmap.set_debug_stable_displacement(true)
		if clipmap.has_method("set_debug_microrelief_enabled"):
			clipmap.set_debug_microrelief_enabled(true)
		if clipmap.has_method("set_debug_pbr_enabled"):
			clipmap.set_debug_pbr_enabled(true)
		if clipmap.has_method("set_debug_sink_scale"):
			clipmap.set_debug_sink_scale(2.0)
		if clipmap.has_method("set_debug_geomorph_mode"):
			clipmap.set_debug_geomorph_mode(0)
		if clipmap.has_method("set_aerial_strength"):
			clipmap.set_aerial_strength(0.78)
	var scatter: Node = _scatter()
	if scatter != null and scatter.has_method("set_debug_enabled"):
		scatter.call("set_debug_enabled", true)
	var ocean: Node = get_node_or_null("/root/OceanSystem")
	if ocean != null and ocean.has_method("set_debug_stable_displacement"):
		ocean.set_debug_stable_displacement(true)
	_update_aerial_label()
	_update_status()


func _update_status() -> void:
	if _status == null:
		return
	var modes := PackedStringArray()
	if freeze_terrain:
		modes.append("FROZEN")
	if side_cut:
		modes.append("SIDE CUT")
	if wireframe:
		modes.append("WIREFRAME")
	if not stable_displacement:
		modes.append("LEGACY TERRAIN")
	if not stable_ocean_displacement:
		modes.append("LEGACY OCEAN")
	if not microrelief:
		modes.append("MICRO OFF")
	if not pbr_detail:
		modes.append("PBR OFF")
	if not gpu_scatter:
		modes.append("SCATTER OFF")
	if geomorph_mode > 0 and geomorph_mode < GEOMORPH_MODE_NAMES.size():
		modes.append("GPU %s" % GEOMORPH_MODE_NAMES[geomorph_mode].to_upper())
	if absf(aerial_strength - 0.78) > 0.001:
		modes.append("AERIAL %.2f×" % aerial_strength)
	if modes.is_empty():
		_status.visible = false
		_status.text = ""
		return
	_status.visible = true
	_status.text = "TERRAIN DEBUG  %s  |  sink %.2f× spacing" % [
		" + ".join(modes), sink_scale]