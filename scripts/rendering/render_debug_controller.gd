extends Node
## Render-chain isolation tool injected into the runtime '&' debug menu.
##
## This controller deliberately lives outside the terrain/ocean inheritance trees.
## It can therefore disable complete render paths while those systems are being
## profiled without changing their normal LOD/topology implementation.
##
## Render toggles are transient diagnostics only; they are not persisted to user
## settings and do not change simulation state unless explicitly noted.

const STATUS_REFRESH_S := 0.20
const CLOUD_REASSERT_S := 0.25
const DEFAULT_AERIAL_STRENGTH := 0.78

var _state := {
	"terrain": true,
	"scatter": true,
	"micro": true,
	"pbr": true,
	"aerial": true,
	"local_ocean": true,
	"orbit_ocean": true,
	"ocean_waves": true,
	"clouds": true,
	"cloud_shadows": true,
	"sky_background": true,
	"sun": true,
	"sun_shadows": true,
}

var _menu: DebugMenu
var _tabs: TabContainer
var _status: Label
var _buttons: Dictionary = {}
var _level_buttons: Dictionary = {}
var _status_accum := 0.0
var _cloud_reassert_accum := 0.0
var _aerial_restore_strength := DEFAULT_AERIAL_STRENGTH

var _sky_background_modes: Dictionary = {}
var _sky_background_colours: Dictionary = {}
var _sun_visibility: Dictionary = {}
var _sun_shadow_state: Dictionary = {}


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 40
	get_tree().node_added.connect(_on_node_added)
	call_deferred("_scan_for_debug_menu")
	call_deferred("_capture_runtime_defaults")


func _process(delta: float) -> void:
	if not bool(_state["clouds"]) or not bool(_state["cloud_shadows"]):
		_cloud_reassert_accum += delta
		if _cloud_reassert_accum >= CLOUD_REASSERT_S:
			_cloud_reassert_accum = fmod(_cloud_reassert_accum, CLOUD_REASSERT_S)
			_apply_cloud_state()

	if _menu == null or not is_instance_valid(_menu):
		_menu = null
		_tabs = null
		return
	if not _menu.visible or not _is_render_tab_active():
		return

	_status_accum += delta
	if _status_accum >= STATUS_REFRESH_S:
		_status_accum = fmod(_status_accum, STATUS_REFRESH_S)
		_refresh_status()


func _on_node_added(node: Node) -> void:
	if node is DebugMenu:
		call_deferred("_install_into_menu", node)
		return
	if node is WorldEnvironment or node is DirectionalLight3D or node is OrbitOcean:
		call_deferred("_apply_all_states")


func _scan_for_debug_menu() -> void:
	var found := _find_debug_menu(get_tree().root)
	if found != null:
		_install_into_menu(found)


func _find_debug_menu(node: Node) -> DebugMenu:
	if node is DebugMenu:
		return node as DebugMenu
	for child: Node in node.get_children():
		var found := _find_debug_menu(child)
		if found != null:
			return found
	return null


func _install_into_menu(menu: DebugMenu) -> void:
	if menu == null or not is_instance_valid(menu):
		return
	if _menu == menu and _tabs != null and is_instance_valid(_tabs):
		return

	var tabs := _find_tab_container(menu)
	if tabs == null:
		return
	var existing := tabs.get_node_or_null("Render")
	if existing != null:
		_menu = menu
		_tabs = tabs
		return

	_menu = menu
	_tabs = tabs
	_sync_state_from_runtime()

	var scroll := ScrollContainer.new()
	scroll.name = "Render"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tabs.add_child(scroll)
	if tabs.get_child_count() > 1:
		tabs.move_child(scroll, 1)

	var box := VBoxContainer.new()
	box.name = "RenderDebugControls"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 5)
	scroll.add_child(box)

	box.add_child(_section_title("Render-chain isolation"))
	box.add_child(_note("These switches remove actual draw/update paths. They are temporary diagnostics and are never saved."))

	_status = Label.new()
	_status.text = "Render profiler: waiting…"
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.add_theme_font_size_override("font_size", 12)
	_status.add_theme_color_override("font_color", Color(0.79, 0.88, 1.0))
	box.add_child(_status)

	var preset_row := HBoxContainer.new()
	preset_row.add_theme_constant_override("separation", 6)
	var restore := Button.new()
	restore.text = "Restore all"
	restore.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	restore.pressed.connect(_preset_restore_all)
	preset_row.add_child(restore)
	var core := Button.new()
	core.text = "Terrain core"
	core.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	core.pressed.connect(_preset_terrain_core)
	preset_row.add_child(core)
	var sky_only := Button.new()
	sky_only.text = "Sky / atmosphere"
	sky_only.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sky_only.pressed.connect(_preset_sky_only)
	preset_row.add_child(sky_only)
	box.add_child(preset_row)
	box.add_child(_note("Terrain core removes optional surface extras, water and clouds. Sky / atmosphere hides world geometry to measure the background/compositor cost."))

	box.add_child(HSeparator.new())
	box.add_child(_section_title("Terrain"))
	_add_toggle(box, "terrain", "Terrain geometry", "Entire spherical clipmap draw/update path.")
	_add_toggle(box, "scatter", "Terrain scatter", "Grass plus geological and river stones.")
	_add_toggle(box, "micro", "Near-field microgeometry", "Dense L0 microgeometry draw itself; disabling restores the ordinary full L0 centre mesh.")
	_add_toggle(box, "pbr", "Scanned terrain PBR", "Close triplanar scan albedo / normal / roughness detail.")
	_add_toggle(box, "aerial", "Terrain aerial perspective", "Terrain optical-path veil toward the horizon.")
	_build_level_controls(box)

	box.add_child(HSeparator.new())
	box.add_child(_section_title("Water"))
	_add_toggle(box, "local_ocean", "Local / regional ocean", "Ocean geometry clipmap only; GPU buoyancy queries remain available.")
	_add_toggle(box, "orbit_ocean", "Orbit ocean shell", "Global sea shell used above the 120 km handoff.")
	_add_toggle(box, "ocean_waves", "Ocean wave displacement", "Keeps water visible but removes dynamic wave displacement.")

	box.add_child(HSeparator.new())
	box.add_child(_section_title("Atmosphere and lighting"))
	_add_toggle(box, "clouds", "Volumetric clouds", "Depth-aware cloud compositor / sky fallback. Cloud shadows are controlled separately.")
	_add_toggle(box, "cloud_shadows", "Cloud shadows", "Cloud Beer-Lambert shadow sampling on terrain and water.")
	_add_toggle(box, "sky_background", "Atmosphere / sky background", "Stops drawing the sky background while preserving its scene resource for independent tests.")
	_add_toggle(box, "sun", "Sun directional light", "Direct scene lighting from Helion directional lights.")
	_add_toggle(box, "sun_shadows", "Sun shadow maps", "Directional shadow cascades only; direct sunlight remains enabled.")

	_refresh_status()
	_apply_all_states()


func _build_level_controls(box: VBoxContainer) -> void:
	box.add_child(HSeparator.new())
	box.add_child(_section_title("Terrain LOD visibility"))
	box.add_child(_note("Logical L0–L14 visual isolation. Unchecked levels are collapsed through their instance transform; production slot order, visible_instance_count and horizon/nadir culling are left untouched. Use this to inspect coverage, not to measure per-LOD GPU cost."))

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	var all_on := Button.new()
	all_on.text = "Enable all LODs"
	all_on.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	all_on.pressed.connect(_set_all_levels.bind(true))
	row.add_child(all_on)
	var all_off := Button.new()
	all_off.text = "Disable all LODs"
	all_off.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	all_off.pressed.connect(_set_all_levels.bind(false))
	row.add_child(all_off)
	box.add_child(row)

	var grid := GridContainer.new()
	grid.columns = 5
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", 8)
	grid.add_theme_constant_override("v_separation", 3)
	var level_count := 15
	if GroundGeometryClipmap.has_method("debug_level_count"):
		level_count = int(GroundGeometryClipmap.call("debug_level_count"))
	for level: int in level_count:
		var button := CheckButton.new()
		button.text = "L%d" % level
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		button.custom_minimum_size = Vector2(72, 30)
		button.add_theme_font_size_override("font_size", 12)
		var enabled := true
		if GroundGeometryClipmap.has_method("debug_level_enabled"):
			enabled = bool(GroundGeometryClipmap.call("debug_level_enabled", level))
		button.button_pressed = enabled
		button.toggled.connect(_on_level_toggle.bind(level))
		grid.add_child(button)
		_level_buttons[level] = button
	box.add_child(grid)


func _find_tab_container(node: Node) -> TabContainer:
	if node is TabContainer:
		return node as TabContainer
	for child: Node in node.get_children():
		var found := _find_tab_container(child)
		if found != null:
			return found
	return null


func _section_title(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 14)
	label.add_theme_color_override("font_color", Color(0.86, 0.91, 1.0))
	return label


func _note(text: String) -> Label:
	var label := Label.new()
	label.text = "     %s" % text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.add_theme_font_size_override("font_size", 11)
	label.add_theme_color_override("font_color", Color(0.58, 0.65, 0.76))
	return label


func _add_toggle(box: VBoxContainer, key: String, title: String, note_text: String) -> void:
	var button := CheckButton.new()
	button.text = title
	button.button_pressed = bool(_state.get(key, true))
	button.custom_minimum_size = Vector2(0, 32)
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.add_theme_font_size_override("font_size", 13)
	button.toggled.connect(_on_toggle.bind(key))
	box.add_child(button)
	_buttons[key] = button
	box.add_child(_note(note_text))


func _on_toggle(enabled: bool, key: String) -> void:
	_state[key] = enabled
	_apply_state(key)
	_refresh_status()


func _on_level_toggle(enabled: bool, level: int) -> void:
	if GroundGeometryClipmap.has_method("set_debug_level_enabled"):
		GroundGeometryClipmap.call("set_debug_level_enabled", level, enabled)
	_refresh_status()


func _set_all_levels(enabled: bool) -> void:
	if GroundGeometryClipmap.has_method("set_all_debug_levels_enabled"):
		GroundGeometryClipmap.call("set_all_debug_levels_enabled", enabled)
	_sync_level_buttons()
	_refresh_status()


func _apply_state(key: String) -> void:
	match key:
		"terrain":
			_apply_terrain_enabled(bool(_state[key]))
		"scatter":
			_apply_scatter_enabled(bool(_state[key]))
		"micro":
			if GroundGeometryClipmap.has_method("set_debug_microrelief_enabled"):
				GroundGeometryClipmap.call("set_debug_microrelief_enabled", bool(_state[key]))
		"pbr":
			if GroundGeometryClipmap.has_method("set_debug_pbr_enabled"):
				GroundGeometryClipmap.call("set_debug_pbr_enabled", bool(_state[key]))
		"aerial":
			_apply_aerial_enabled(bool(_state[key]))
		"local_ocean":
			_apply_local_ocean_enabled(bool(_state[key]))
		"orbit_ocean":
			_apply_orbit_ocean_enabled(bool(_state[key]))
		"ocean_waves":
			if OceanSystem.has_method("set_debug_waves_disabled"):
				OceanSystem.call("set_debug_waves_disabled", not bool(_state[key]))
		"clouds", "cloud_shadows":
			_apply_cloud_state()
		"sky_background":
			_apply_sky_background(bool(_state[key]))
		"sun", "sun_shadows":
			_apply_sun_state()


func _apply_all_states() -> void:
	for key_value: Variant in [
		"terrain", "scatter", "micro", "pbr", "aerial",
		"local_ocean", "orbit_ocean", "ocean_waves",
		"clouds", "cloud_shadows", "sky_background", "sun", "sun_shadows"
	]:
		_apply_state(String(key_value))


func _apply_terrain_enabled(enabled: bool) -> void:
	GroundGeometryClipmap.set_process(enabled)
	if not enabled and GroundGeometryClipmap.has_method("_set_visible"):
		GroundGeometryClipmap.call("_set_visible", false)


func _apply_scatter_enabled(enabled: bool) -> void:
	TerrainScatter.set_process(enabled)
	if TerrainScatter.has_method("set_debug_enabled"):
		TerrainScatter.call("set_debug_enabled", enabled)


func _apply_aerial_enabled(enabled: bool) -> void:
	if not GroundGeometryClipmap.has_method("set_aerial_strength"):
		return
	if enabled:
		GroundGeometryClipmap.call("set_aerial_strength", maxf(_aerial_restore_strength, 0.01))
		return
	if GroundGeometryClipmap.has_method("aerial_strength"):
		var current := float(GroundGeometryClipmap.call("aerial_strength"))
		if current > 0.001:
			_aerial_restore_strength = current
	GroundGeometryClipmap.call("set_aerial_strength", 0.0)


func _apply_local_ocean_enabled(enabled: bool) -> void:
	OceanSystem.set_process(enabled)
	if not enabled and OceanSystem.has_method("_set_visible"):
		OceanSystem.call("_set_visible", false)


func _apply_orbit_ocean_enabled(enabled: bool) -> void:
	var oceans: Array[OrbitOcean] = []
	_collect_orbit_oceans(get_tree().root, oceans)
	for ocean: OrbitOcean in oceans:
		ocean.set_process(enabled)
		if not enabled:
			var mesh_value: Variant = ocean.get("_mesh_instance")
			if mesh_value is MeshInstance3D:
				(mesh_value as MeshInstance3D).visible = false


func _apply_cloud_state() -> void:
	var visible_clouds := bool(_state["clouds"])
	var cloud_shadows := bool(_state["cloud_shadows"])
	VolumetricClouds.set_process(visible_clouds or cloud_shadows)

	var effect_value: Variant = VolumetricClouds.get("_depth_effect")
	var compositor_ready := false
	if effect_value is CompositorEffect:
		var effect := effect_value as CompositorEffect
		effect.enabled = visible_clouds
		if effect.has_method("is_ready"):
			compositor_ready = bool(effect.call("is_ready"))

	var material_value: Variant = VolumetricClouds.get("_material")
	if material_value is ShaderMaterial:
		(material_value as ShaderMaterial).set_shader_parameter(
			"u_cloud_enabled", 1.0 if visible_clouds and not compositor_ready else 0.0)

	var refs_value: Variant = VolumetricClouds.get("_shadow_receivers")
	if refs_value is Array:
		for ref_value: Variant in refs_value:
			if not (ref_value is WeakRef):
				continue
			var receiver_value: Variant = (ref_value as WeakRef).get_ref()
			if receiver_value is ShaderMaterial:
				(receiver_value as ShaderMaterial).set_shader_parameter(
					"u_cloud_shadow_enabled", 1.0 if cloud_shadows else 0.0)


func _apply_sky_background(enabled: bool) -> void:
	var environments: Array[WorldEnvironment] = []
	_collect_world_environments(get_tree().root, environments)
	for world_environment: WorldEnvironment in environments:
		var environment := world_environment.environment
		if environment == null:
			continue
		var id := environment.get_instance_id()
		if not _sky_background_modes.has(id):
			_sky_background_modes[id] = environment.background_mode
			_sky_background_colours[id] = environment.background_color
		if enabled:
			environment.background_mode = int(_sky_background_modes[id]) as Environment.BGMode
			environment.background_color = _sky_background_colours[id] as Color
		else:
			environment.background_mode = Environment.BG_COLOR
			environment.background_color = Color(0.008, 0.010, 0.014)


func _apply_sun_state() -> void:
	var lights: Array[DirectionalLight3D] = []
	_collect_directional_lights(get_tree().root, lights)
	for light: DirectionalLight3D in lights:
		var id := light.get_instance_id()
		if not _sun_visibility.has(id):
			_sun_visibility[id] = light.visible
		if not _sun_shadow_state.has(id):
			_sun_shadow_state[id] = light.shadow_enabled
		light.visible = bool(_sun_visibility[id]) if bool(_state["sun"]) else false
		light.shadow_enabled = bool(_sun_shadow_state[id]) if bool(_state["sun_shadows"]) else false


func _capture_runtime_defaults() -> void:
	if GroundGeometryClipmap.has_method("aerial_strength"):
		_aerial_restore_strength = maxf(
			float(GroundGeometryClipmap.call("aerial_strength")), 0.01)
	var environments: Array[WorldEnvironment] = []
	_collect_world_environments(get_tree().root, environments)
	for world_environment: WorldEnvironment in environments:
		var environment := world_environment.environment
		if environment != null:
			_sky_background_modes[environment.get_instance_id()] = environment.background_mode
			_sky_background_colours[environment.get_instance_id()] = environment.background_color
	var lights: Array[DirectionalLight3D] = []
	_collect_directional_lights(get_tree().root, lights)
	for light: DirectionalLight3D in lights:
		_sun_visibility[light.get_instance_id()] = light.visible
		_sun_shadow_state[light.get_instance_id()] = light.shadow_enabled


func _sync_state_from_runtime() -> void:
	_state["terrain"] = GroundGeometryClipmap.is_processing()
	if TerrainScatter.has_method("debug_enabled"):
		_state["scatter"] = bool(TerrainScatter.call("debug_enabled"))
	if GroundGeometryClipmap.has_method("debug_microrelief_enabled"):
		_state["micro"] = bool(GroundGeometryClipmap.call("debug_microrelief_enabled"))
	if GroundGeometryClipmap.has_method("debug_pbr_enabled"):
		_state["pbr"] = bool(GroundGeometryClipmap.call("debug_pbr_enabled"))
	if GroundGeometryClipmap.has_method("aerial_strength"):
		var aerial := float(GroundGeometryClipmap.call("aerial_strength"))
		_state["aerial"] = aerial > 0.001
		if aerial > 0.001:
			_aerial_restore_strength = aerial
	_state["local_ocean"] = OceanSystem.is_processing()
	if OceanSystem.has_method("debug_waves_disabled"):
		_state["ocean_waves"] = not bool(OceanSystem.call("debug_waves_disabled"))


func _preset_restore_all() -> void:
	for key: Variant in _state.keys():
		_state[key] = true
	if GroundGeometryClipmap.has_method("set_all_debug_levels_enabled"):
		GroundGeometryClipmap.call("set_all_debug_levels_enabled", true)
	_sync_buttons()
	_sync_level_buttons()
	_apply_all_states()
	_refresh_status()


func _preset_terrain_core() -> void:
	_set_many({
		"terrain": true,
		"scatter": false,
		"micro": false,
		"pbr": false,
		"aerial": false,
		"local_ocean": false,
		"orbit_ocean": false,
		"ocean_waves": false,
		"clouds": false,
		"cloud_shadows": false,
		"sky_background": true,
		"sun": true,
		"sun_shadows": false,
	})


func _preset_sky_only() -> void:
	_set_many({
		"terrain": false,
		"scatter": false,
		"micro": false,
		"pbr": false,
		"aerial": false,
		"local_ocean": false,
		"orbit_ocean": false,
		"ocean_waves": false,
		"clouds": true,
		"cloud_shadows": false,
		"sky_background": true,
		"sun": false,
		"sun_shadows": false,
	})


func _set_many(values: Dictionary) -> void:
	for key: Variant in values.keys():
		_state[String(key)] = bool(values[key])
	_sync_buttons()
	_apply_all_states()
	_refresh_status()


func _sync_buttons() -> void:
	for key: Variant in _buttons.keys():
		var value: Variant = _buttons[key]
		if value is CheckButton:
			(value as CheckButton).set_pressed_no_signal(bool(_state.get(key, true)))


func _sync_level_buttons() -> void:
	for key: Variant in _level_buttons.keys():
		var value: Variant = _level_buttons[key]
		if not (value is CheckButton):
			continue
		var enabled := true
		if GroundGeometryClipmap.has_method("debug_level_enabled"):
			enabled = bool(GroundGeometryClipmap.call("debug_level_enabled", int(key)))
		(value as CheckButton).set_pressed_no_signal(enabled)


func _is_render_tab_active() -> bool:
	if _tabs == null or _tabs.get_tab_count() <= 0:
		return false
	return _tabs.get_tab_title(_tabs.current_tab) == "Render"


func _format_levels(value: Variant) -> String:
	if not (value is Array):
		return "—"
	var parts := PackedStringArray()
	for entry: Variant in value:
		parts.append("L%d" % int(entry))
	return ",".join(parts) if not parts.is_empty() else "none"


func _refresh_status() -> void:
	if _status == null:
		return
	var fps := float(Engine.get_frames_per_second())
	var frame_ms := 1000.0 / maxf(fps, 0.001)
	var lines := PackedStringArray()
	lines.append("FPS %.0f  •  %.2f ms/frame" % [fps, frame_ms])

	if GroundGeometryClipmap.has_method("gpu_stream_stats"):
		var terrain_stats: Dictionary = GroundGeometryClipmap.call("gpu_stream_stats")
		lines.append("Terrain %s  •  logical L%d–L%d  •  physical %d rings  •  %d batches" % [
			"ON" if bool(_state["terrain"]) else "OFF",
			int(terrain_stats.get("active_min_level", 0)),
			int(terrain_stats.get("active_max_level", 0)),
			int(terrain_stats.get("physical_ring_instances", 0)),
			int(terrain_stats.get("draw_batches", 0)),
		])
		lines.append("LOD visible: %s  •  production ring prefix %d  •  view %s" % [
			_format_levels(terrain_stats.get("debug_visible_levels", [])),
			int(terrain_stats.get("view_ring_instances", 0)),
			String(terrain_stats.get("view_cull_reason", "none")),
		])
	if OceanSystem.has_method("gpu_stats"):
		var ocean_stats: Dictionary = OceanSystem.call("gpu_stats")
		lines.append("Ocean %s  •  %d levels  •  %d visible sectors  •  waves %s" % [
			"ON" if bool(_state["local_ocean"]) else "OFF",
			int(ocean_stats.get("active_levels", 0)),
			int(ocean_stats.get("visible_sectors", 0)),
			"ON" if bool(_state["ocean_waves"]) else "OFF",
		])
	lines.append("Scatter %s  •  Clouds %s  •  cloud shadows %s  •  sky %s" % [
		"ON" if bool(_state["scatter"]) else "OFF",
		"ON" if bool(_state["clouds"]) else "OFF",
		"ON" if bool(_state["cloud_shadows"]) else "OFF",
		"ON" if bool(_state["sky_background"]) else "OFF",
	])
	_status.text = "\n".join(lines)


func _collect_world_environments(node: Node, out: Array[WorldEnvironment]) -> void:
	if node is WorldEnvironment:
		out.append(node as WorldEnvironment)
	for child: Node in node.get_children():
		_collect_world_environments(child, out)


func _collect_directional_lights(node: Node, out: Array[DirectionalLight3D]) -> void:
	if node is DirectionalLight3D:
		out.append(node as DirectionalLight3D)
	for child: Node in node.get_children():
		_collect_directional_lights(child, out)


func _collect_orbit_oceans(node: Node, out: Array[OrbitOcean]) -> void:
	if node is OrbitOcean:
		out.append(node as OrbitOcean)
	for child: Node in node.get_children():
		_collect_orbit_oceans(child, out)
