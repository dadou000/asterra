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
const HEIGHT_DIAGNOSTIC_INTERVAL_S: float = 0.10
const AGL_CURSOR_COLOR := Color(0.08, 0.92, 1.0, 0.78)
const GPU_CURSOR_COLOR := Color(1.0, 0.80, 0.08, 0.78)
const PHYSICS_CURSOR_COLOR := Color(1.0, 0.12, 0.66, 0.78)

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

# Height diagnostics are opt-in so opening the debug menu cannot silently add GPU
# readback/contact-query work. Each provider has its own independent cursor.
var agl_height_cursor := false
var gpu_height_cursor := false
var physics_height_cursor := false

var _wireframes_ready := false
var _status: Label
var _gpu_controls_installed := false
var _gpu_status_label: Label
var _aerial_label: Label
var _gpu_status_accum := 0.0
var _height_status_label: Label
var _height_debug_accum := 0.0
var _height_cursor_root: Node3D
var _agl_marker: MeshInstance3D
var _gpu_marker: MeshInstance3D
var _physics_marker: MeshInstance3D
var _agl_sample: Dictionary = {}
var _gpu_sample: Dictionary = {}
var _physics_sample: Dictionary = {}


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


func _exit_tree() -> void:
	if _height_cursor_root != null and is_instance_valid(_height_cursor_root):
		_height_cursor_root.queue_free()


func _process(delta: float) -> void:
	_sync_height_cursor_positions()
	if not _gpu_controls_installed:
		return
	_gpu_status_accum += delta
	if _gpu_status_accum >= 0.25:
		_gpu_status_accum = fmod(_gpu_status_accum, 0.25)
		_refresh_gpu_control_status()
	if _height_diagnostics_enabled():
		_height_debug_accum += delta
		if _height_debug_accum >= HEIGHT_DIAGNOSTIC_INTERVAL_S:
			_height_debug_accum = fmod(_height_debug_accum, HEIGHT_DIAGNOSTIC_INTERVAL_S)
			_refresh_height_diagnostics()


func _clipmap() -> Node:
	return get_node_or_null("/root/GroundGeometryClipmap")


func _scatter() -> Node:
	return get_node_or_null("/root/TerrainScatter")


func _main_node() -> Node:
	return get_parent()


func _player_node() -> Node:
	var main: Node = _main_node()
	if main == null:
		return null
	var value: Variant = main.get("player")
	return value as Node if value is Node else null


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

	_install_height_diagnostic_controls(box)
	_gpu_controls_installed = true
	_update_aerial_label()
	_refresh_gpu_control_status()
	_refresh_height_diagnostics()


func _install_height_diagnostic_controls(box: VBoxContainer) -> void:
	box.add_child(HSeparator.new())
	var title := Label.new()
	title.text = "Terrain height agreement cursors"
	title.add_theme_font_size_override("font_size", 14)
	title.add_theme_color_override("font_color", Color(0.86, 0.91, 1.0))
	box.add_child(title)

	_height_status_label = Label.new()
	_height_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_height_status_label.add_theme_font_size_override("font_size", 11)
	_height_status_label.add_theme_color_override("font_color", Color(0.74, 0.82, 0.92))
	box.add_child(_height_status_label)

	_add_gpu_toggle(box, "CYAN — AGL ground cursor", agl_height_cursor,
		"Directly below the player at the surface implied by the player's reported AGL.",
		func(value: bool): set_agl_height_cursor(value))
	_add_gpu_toggle(box, "YELLOW — GPU terrain cursor", gpu_height_cursor,
		"At the nearest rendered clipmap lattice vertex to the terrain point under the crosshair.",
		func(value: bool): set_gpu_height_cursor(value))
	_add_gpu_toggle(box, "MAGENTA — physics terrain cursor", physics_height_cursor,
		"Strict physics/contact height at exactly the same lattice direction as the yellow GPU cursor.",
		func(value: bool): set_physics_height_cursor(value))
	box.add_child(_menu_note(
		"GPU/physics sampling is capped at 10 Hz. MICRO BASE means the selected vertex is in the dense micro lattice; the current rendered-contact query includes cache/morph/edits/deformation/surface bias but not the material microrelief displacement."))
	_create_height_cursor_markers()


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


func _create_height_cursor_markers() -> void:
	if _height_cursor_root != null and is_instance_valid(_height_cursor_root):
		return
	var main: Node = _main_node()
	if main == null:
		return
	_height_cursor_root = Node3D.new()
	_height_cursor_root.name = "TerrainHeightDiagnosticCursors"
	main.add_child(_height_cursor_root)
	_agl_marker = _make_height_marker("AGLGroundCursor", AGL_CURSOR_COLOR, 0.30)
	_gpu_marker = _make_height_marker("GPUTerrainCursor", GPU_CURSOR_COLOR, 0.24)
	_physics_marker = _make_height_marker("PhysicsTerrainCursor", PHYSICS_CURSOR_COLOR, 0.18)
	_height_cursor_root.add_child(_agl_marker)
	_height_cursor_root.add_child(_gpu_marker)
	_height_cursor_root.add_child(_physics_marker)


func _make_height_marker(marker_name: String, color: Color,
		radius: float) -> MeshInstance3D:
	var marker := MeshInstance3D.new()
	marker.name = marker_name
	var sphere := SphereMesh.new()
	sphere.radius = radius
	sphere.height = radius * 2.0
	sphere.radial_segments = 12
	sphere.rings = 6
	marker.mesh = sphere
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = color
	material.emission_enabled = true
	material.emission = Color(color.r, color.g, color.b)
	marker.material_override = material
	marker.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	marker.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	marker.visible = false
	return marker


func _height_diagnostics_enabled() -> bool:
	return agl_height_cursor or gpu_height_cursor or physics_height_cursor


func _refresh_height_diagnostics() -> void:
	if _height_status_label == null:
		return
	var lines := PackedStringArray()
	lines.append("CYAN AGL under player  •  YELLOW GPU vertex  •  MAGENTA physics @ GPU vertex")
	var player: Node = _player_node()
	if player == null or Planet.cfg == null or not Planet.ready_state:
		_clear_height_samples()
		lines.append("Player/planet not ready")
		_height_status_label.text = "\n".join(lines)
		return

	if agl_height_cursor:
		_refresh_agl_diagnostic(player, lines)
	else:
		_agl_sample.clear()
		_set_marker_sample(_agl_marker, _agl_sample, false)

	if gpu_height_cursor or physics_height_cursor:
		_refresh_look_height_diagnostics(lines)
	else:
		_gpu_sample.clear()
		_physics_sample.clear()
		_set_marker_sample(_gpu_marker, _gpu_sample, false)
		_set_marker_sample(_physics_marker, _physics_sample, false)

	if not _height_diagnostics_enabled():
		lines.append("All height cursors are off")
	_height_status_label.text = "\n".join(lines)


func _refresh_agl_diagnostic(player: Node, lines: PackedStringArray) -> void:
	if not player.has_method("altitude") or not player.has_method("height_above_ground") \
			or not player.has_method("up_dir"):
		_agl_sample.clear()
		_set_marker_sample(_agl_marker, _agl_sample, false)
		lines.append("CYAN AGL: unavailable")
		return
	var altitude_msl: float = float(player.call("altitude"))
	var agl_m: float = float(player.call("height_above_ground"))
	var dir_value: Variant = player.call("up_dir")
	if not (dir_value is Vector3) or not is_finite(altitude_msl) or not is_finite(agl_m):
		_agl_sample.clear()
		_set_marker_sample(_agl_marker, _agl_sample, false)
		lines.append("CYAN AGL: pending")
		return
	var direction: Vector3 = (dir_value as Vector3).normalized()
	var implied_ground_msl: float = altitude_msl - agl_m
	_agl_sample = {"dir": direction, "height": implied_ground_msl}
	_set_marker_sample(_agl_marker, _agl_sample, true)
	lines.append("CYAN AGL %.3f m  → implied ground %.3f m MSL" % [agl_m, implied_ground_msl])


func _refresh_look_height_diagnostics(lines: PackedStringArray) -> void:
	var main: Node = _main_node()
	var clipmap: Node = _clipmap()
	if main == null or clipmap == null or not clipmap.has_method("debug_closest_rendered_vertex"):
		_clear_look_height_samples()
		lines.append("LOOK vertex: clipmap diagnostic unavailable")
		return
	var aim_value: Variant = main.get("_aim")
	if not (aim_value is Dictionary):
		_clear_look_height_samples()
		lines.append("LOOK vertex: no terrain under crosshair")
		return
	var aim: Dictionary = aim_value
	var aim_dir_value: Variant = aim.get("dir", null)
	if aim.is_empty() or not (aim_dir_value is Vector3):
		_clear_look_height_samples()
		lines.append("LOOK vertex: no terrain under crosshair")
		return
	var vertex_value: Variant = clipmap.call(
		"debug_closest_rendered_vertex", (aim_dir_value as Vector3).normalized())
	if not (vertex_value is Dictionary):
		_clear_look_height_samples()
		lines.append("LOOK vertex: outside active clipmap")
		return
	var vertex: Dictionary = vertex_value
	var vertex_dir_value: Variant = vertex.get("dir", null)
	if vertex.is_empty() or not (vertex_dir_value is Vector3):
		_clear_look_height_samples()
		lines.append("LOOK vertex: outside active clipmap")
		return
	var vertex_dir: Vector3 = (vertex_dir_value as Vector3).normalized()
	var level: int = int(vertex.get("level", -1))
	var spacing_m: float = float(vertex.get("spacing_m", 0.0))
	var snap_error_m: float = float(vertex.get("target_error_m", 0.0))
	var is_micro: bool = bool(vertex.get("micro", false))
	var kind: String = "MICRO BASE L0" if is_micro else "L%d" % level
	lines.append("LOOK vertex %s  spacing %.3f m  aim snap %.3f m" % [
		kind, spacing_m, snap_error_m])

	var gpu_height: float = NAN
	if gpu_height_cursor:
		var rendered_query: Node = get_node_or_null("/root/RenderedTerrainContactQuery")
		if rendered_query != null and rendered_query.has_method("height_for_direction"):
			gpu_height = float(rendered_query.call("height_for_direction", vertex_dir, NAN))
		if is_finite(gpu_height):
			_gpu_sample = {"dir": vertex_dir, "height": gpu_height}
			_set_marker_sample(_gpu_marker, _gpu_sample, true)
			lines.append("YELLOW GPU %.3f m MSL" % gpu_height)
		else:
			_gpu_sample.clear()
			_set_marker_sample(_gpu_marker, _gpu_sample, false)
			lines.append("YELLOW GPU: pending")
	else:
		_gpu_sample.clear()
		_set_marker_sample(_gpu_marker, _gpu_sample, false)

	var physics_height: float = NAN
	if physics_height_cursor:
		physics_height = TerrainContactSampler.contact_height(vertex_dir, NAN)
		if is_finite(physics_height):
			_physics_sample = {"dir": vertex_dir, "height": physics_height}
			_set_marker_sample(_physics_marker, _physics_sample, true)
			var delta_text: String = ""
			if is_finite(gpu_height):
				var delta_gpu: float = physics_height - gpu_height
				delta_text = "  Δphys-gpu %s%.3f m" % [
					"+" if delta_gpu >= 0.0 else "", delta_gpu]
			lines.append("MAGENTA PHYS %.3f m MSL%s" % [physics_height, delta_text])
		else:
			_physics_sample.clear()
			_set_marker_sample(_physics_marker, _physics_sample, false)
			lines.append("MAGENTA PHYS: pending exact contact")
	else:
		_physics_sample.clear()
		_set_marker_sample(_physics_marker, _physics_sample, false)


func _set_marker_sample(marker: MeshInstance3D, sample: Dictionary,
		enabled: bool) -> void:
	if marker == null or not is_instance_valid(marker):
		return
	marker.visible = enabled and not sample.is_empty()


func _sync_height_cursor_positions() -> void:
	if Planet.cfg == null or _height_cursor_root == null \
			or not is_instance_valid(_height_cursor_root):
		return
	_sync_one_height_cursor(_agl_marker, _agl_sample, agl_height_cursor)
	_sync_one_height_cursor(_gpu_marker, _gpu_sample, gpu_height_cursor)
	_sync_one_height_cursor(_physics_marker, _physics_sample, physics_height_cursor)


func _sync_one_height_cursor(marker: MeshInstance3D, sample: Dictionary,
		enabled: bool) -> void:
	if marker == null or not is_instance_valid(marker):
		return
	if not enabled or sample.is_empty():
		marker.visible = false
		return
	var dir_value: Variant = sample.get("dir", null)
	if not (dir_value is Vector3):
		marker.visible = false
		return
	var height: float = float(sample.get("height", NAN))
	if not is_finite(height):
		marker.visible = false
		return
	var direction: Vector3 = (dir_value as Vector3).normalized()
	var radius: float = Planet.cfg.planet_radius + height
	var world_pos := Vec3D.new(
		direction.x * radius, direction.y * radius, direction.z * radius)
	marker.global_position = Frames.to_render(world_pos)
	marker.visible = true


func _clear_look_height_samples() -> void:
	_gpu_sample.clear()
	_physics_sample.clear()
	_set_marker_sample(_gpu_marker, _gpu_sample, false)
	_set_marker_sample(_physics_marker, _physics_sample, false)


func _clear_height_samples() -> void:
	_agl_sample.clear()
	_clear_look_height_samples()
	_set_marker_sample(_agl_marker, _agl_sample, false)


func set_agl_height_cursor(value: bool) -> void:
	agl_height_cursor = value
	_height_debug_accum = HEIGHT_DIAGNOSTIC_INTERVAL_S
	if not value:
		_agl_sample.clear()
		_set_marker_sample(_agl_marker, _agl_sample, false)
	_refresh_height_diagnostics()
	_update_status()


func set_gpu_height_cursor(value: bool) -> void:
	gpu_height_cursor = value
	_height_debug_accum = HEIGHT_DIAGNOSTIC_INTERVAL_S
	if not value:
		_gpu_sample.clear()
		_set_marker_sample(_gpu_marker, _gpu_sample, false)
	_refresh_height_diagnostics()
	_update_status()


func set_physics_height_cursor(value: bool) -> void:
	physics_height_cursor = value
	_height_debug_accum = HEIGHT_DIAGNOSTIC_INTERVAL_S
	if not value:
		_physics_sample.clear()
		_set_marker_sample(_physics_marker, _physics_sample, false)
	_refresh_height_diagnostics()
	_update_status()


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
	agl_height_cursor = false
	gpu_height_cursor = false
	physics_height_cursor = false
	_clear_height_samples()
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
	_refresh_height_diagnostics()
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
	if _height_diagnostics_enabled():
		modes.append("HEIGHT CURSORS")
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