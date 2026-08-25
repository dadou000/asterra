class_name TerrainDebug
extends CanvasLayer
## Debug controller for the current spherical procedural clipmap.
##
## Debug operations map directly to the active GPU renderer:
##   - wireframe
##   - freeze the camera-following clipmap in planet space
##   - side-cut the concentric rings to inspect overlap/sinking
##   - adjust sink depth for inspection
##   - inspect GPU context / landform / material-classification channels
##   - enable/disable close scanned PBR detail independently

const GEOMORPH_MODE_NAMES := [
	"Final",
	"Landforms",
	"Primary materials",
	"Secondary materials",
	"Soil context",
	"Geology / biome",
	"Hydrology",
]

var terrain: PlanetTerrain # retained only because main.gd assigns it

var wireframe := false: set = set_wireframe
var freeze_terrain := false: set = set_freeze_terrain
var side_cut := false: set = set_side_cut
var pbr_detail := true: set = set_pbr_detail
var sink_scale := 2.0: set = set_sink_scale
var geomorph_mode := 0: set = set_geomorph_mode

var _wireframes_ready := false
var _status: Label


func _ready() -> void:
	layer = 19
	process_priority = 20
	_status = Label.new()
	_status.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_status.position = Vector2(-430, 18)
	_status.custom_minimum_size = Vector2(410, 0)
	_status.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_status.add_theme_font_size_override("font_size", 13)
	_status.add_theme_color_override("font_color", Color(1.0, 0.78, 0.28))
	_status.visible = false
	add_child(_status)
	_update_status()


func _clipmap() -> Node:
	return get_node_or_null("/root/GroundGeometryClipmap")


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


func set_pbr_detail(value: bool) -> void:
	pbr_detail = value
	var clipmap: Node = _clipmap()
	if clipmap != null and clipmap.has_method("set_debug_pbr_enabled"):
		clipmap.set_debug_pbr_enabled(value)
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


func reset_inspection() -> void:
	freeze_terrain = false
	side_cut = false
	pbr_detail = true
	sink_scale = 2.0
	geomorph_mode = 0
	var clipmap: Node = _clipmap()
	if clipmap != null:
		if clipmap.has_method("set_debug_freeze"):
			clipmap.set_debug_freeze(false)
		if clipmap.has_method("set_debug_side_cut"):
			clipmap.set_debug_side_cut(false)
		if clipmap.has_method("set_debug_pbr_enabled"):
			clipmap.set_debug_pbr_enabled(true)
		if clipmap.has_method("set_debug_sink_scale"):
			clipmap.set_debug_sink_scale(2.0)
		if clipmap.has_method("set_debug_geomorph_mode"):
			clipmap.set_debug_geomorph_mode(0)
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
	if not pbr_detail:
		modes.append("PBR OFF")
	if geomorph_mode > 0 and geomorph_mode < GEOMORPH_MODE_NAMES.size():
		modes.append("GPU %s" % GEOMORPH_MODE_NAMES[geomorph_mode].to_upper())
	if modes.is_empty():
		_status.visible = false
		_status.text = ""
		return
	_status.visible = true
	_status.text = "TERRAIN DEBUG  %s  |  sink %.2f× spacing" % [
		" + ".join(modes), sink_scale]
