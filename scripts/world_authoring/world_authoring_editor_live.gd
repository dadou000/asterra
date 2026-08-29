class_name WorldAuthoringLiveEditor
extends "res://scripts/world_authoring/world_authoring_editor_phase1.gd"
## Planet Studio over the live rendered planet.
##
## This layer keeps the transactional authoring UI from Phase 1, adds viewport
## navigation/picking, biome and water placement, and exposes the runtime sparse
## terrain-delta lattice as an immediate non-destructive sculpt layer.

signal runtime_apply_requested(system: Resource)

enum PlacementMode {
	NONE,
	BIOME,
	LAKE,
	RIVER,
	SCULPT_RAISE,
	SCULPT_LOWER,
}

const PANEL_WIDTH_PX: float = 1080.0
const PICK_MAX_RANGE_M: float = 250000.0
const AIM_COARSE_CAPTURE_M: float = 16.0
const AIM_COARSE_STEP_START_M: float = 0.5
const AIM_COARSE_STEP_MAX_M: float = 12.0
const AIM_COARSE_STEP_GROWTH: float = 1.16
const AIM_BINARY_STEPS: int = 10
const CURSOR_SAMPLE_INTERVAL_S: float = 0.04
const SCULPT_MIN_OFFSET_M: float = -10000.0
const SCULPT_MAX_OFFSET_M: float = 10000.0

var _world_host: Node
var _player: Node
var _camera: Camera3D
var _placement_mode: int = PlacementMode.NONE
var _navigation_active: bool = false
var _last_hit: Dictionary = {}
var _last_paint_dir: Vector3 = Vector3.ZERO
var _last_sculpt_dir: Vector3 = Vector3.ZERO
var _cursor_sample_accum: float = 0.0
var _live_status_label: Label
var _preview_instance: MeshInstance3D
var _preview_mesh: ImmediateMesh
var _preview_material: StandardMaterial3D

var _sculpt_radius_m: float = 12.0
var _sculpt_strength_m: float = 0.35
var _sculpt_hardness: float = 0.55


func bind_world(world_host: Node) -> void:
	_world_host = world_host


func _ready() -> void:
	super._ready()
	if _world_host == null:
		return
	_player = _world_host.get("player") as Node
	if _player != null:
		_camera = _player.get("camera") as Camera3D
	_session.applied.connect(func(system: Resource) -> void:
		runtime_apply_requested.emit(system)
	)
	_create_live_preview()
	_set_navigation(false)
	_set_status("LIVE Planet Studio — TAB toggles viewport navigation. Terrain sculpting writes directly to the authoritative sparse edit layer.")


func _build_shell() -> void:
	if _world_host == null:
		super._build_shell()
		return
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_LEFT_WIDE)
	panel.offset_left = 0.0
	panel.offset_top = 0.0
	panel.offset_right = PANEL_WIDTH_PX
	panel.offset_bottom = 0.0
	panel.mouse_filter = Control.MOUSE_FILTER_STOP
	var panel_style := StyleBoxFlat.new()
	panel_style.bg_color = Color(0.025, 0.043, 0.062, 0.965)
	panel_style.border_color = Color(0.16, 0.27, 0.36, 0.95)
	panel_style.border_width_right = 1
	panel.add_theme_stylebox_override("panel", panel_style)
	add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_top", 9)
	margin.add_theme_constant_override("margin_bottom", 9)
	panel.add_child(margin)
	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 7)
	margin.add_child(root)
	root.add_child(_build_live_toolbar())
	root.add_child(HSeparator.new())

	var content := HBoxContainer.new()
	content.size_flags_vertical = Control.SIZE_EXPAND_FILL
	content.add_theme_constant_override("separation", 7)
	root.add_child(content)
	var navigation := VBoxContainer.new()
	navigation.custom_minimum_size.x = 138.0
	navigation.add_theme_constant_override("separation", 4)
	content.add_child(navigation)
	var nav_title := Label.new()
	nav_title.text = "AUTHORING"
	nav_title.modulate = Color(0.55, 0.66, 0.76)
	nav_title.add_theme_font_size_override("font_size", 12)
	navigation.add_child(nav_title)
	for category_name: String in CATEGORY_NAMES:
		var button := Button.new()
		button.text = category_name
		button.custom_minimum_size.y = 40.0
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.pressed.connect(_show_category.bind(category_name))
		navigation.add_child(button)
	var nav_spacer := Control.new()
	nav_spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	navigation.add_child(nav_spacer)
	var navigation_button := Button.new()
	navigation_button.text = "NAVIGATE [TAB]"
	navigation_button.tooltip_text = "Give camera/movement control back to the player. Press TAB again to return to authoring."
	navigation_button.pressed.connect(func() -> void: _set_navigation(not _navigation_active))
	navigation.add_child(navigation_button)

	content.add_child(VSeparator.new())
	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	content.add_child(scroll)
	_workspace = VBoxContainer.new()
	_workspace.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_workspace.add_theme_constant_override("separation", 9)
	scroll.add_child(_workspace)

	_status_label = Label.new()
	_status_label.modulate = Color(0.58, 0.68, 0.77)
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_status_label)

	_live_status_label = Label.new()
	_live_status_label.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_live_status_label.offset_left = -690.0
	_live_status_label.offset_top = 14.0
	_live_status_label.offset_right = -18.0
	_live_status_label.offset_bottom = 72.0
	_live_status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_live_status_label.vertical_alignment = VERTICAL_ALIGNMENT_TOP
	_live_status_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_live_status_label.modulate = Color(0.82, 0.90, 0.97)
	_live_status_label.text = "LIVE VIEW — TAB: navigate"
	add_child(_live_status_label)


func _build_live_toolbar() -> HBoxContainer:
	var toolbar := HBoxContainer.new()
	toolbar.custom_minimum_size.y = 42.0
	toolbar.add_theme_constant_override("separation", 4)
	var back := _compact_toolbar_button("Back", 58.0)
	back.pressed.connect(func() -> void: get_tree().change_scene_to_file(START_MENU_SCENE))
	toolbar.add_child(back)
	var title := Label.new()
	title.text = "PLANET STUDIO"
	title.custom_minimum_size.x = 135.0
	title.add_theme_font_size_override("font_size", 18)
	toolbar.add_child(title)
	_body_selector = OptionButton.new()
	_body_selector.custom_minimum_size.x = 150.0
	_body_selector.item_selected.connect(_on_body_selected)
	toolbar.add_child(_body_selector)
	_dirty_label = Label.new()
	_dirty_label.custom_minimum_size.x = 130.0
	toolbar.add_child(_dirty_label)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(spacer)
	var save := _compact_toolbar_button("Save", 60.0)
	save.pressed.connect(func() -> void: _save_dialog.popup_centered_ratio(0.72))
	toolbar.add_child(save)
	var load_button := _compact_toolbar_button("Load", 60.0)
	load_button.pressed.connect(func() -> void: _load_dialog.popup_centered_ratio(0.72))
	toolbar.add_child(load_button)
	_undo_button = _compact_toolbar_button("Undo", 60.0)
	_undo_button.pressed.connect(_on_undo_pressed)
	toolbar.add_child(_undo_button)
	_redo_button = _compact_toolbar_button("Redo", 60.0)
	_redo_button.pressed.connect(_on_redo_pressed)
	toolbar.add_child(_redo_button)
	_apply_button = _compact_toolbar_button("Apply", 65.0)
	_apply_button.pressed.connect(_on_apply_pressed)
	toolbar.add_child(_apply_button)
	_revert_button = _compact_toolbar_button("Revert", 65.0)
	_revert_button.pressed.connect(_on_revert_pressed)
	toolbar.add_child(_revert_button)
	return toolbar


func _compact_toolbar_button(text: String, width: float) -> Button:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size = Vector2(width, 34.0)
	return button


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return

	_section("Live terrain sculpting")
	var sculpt_row := HBoxContainer.new()
	sculpt_row.add_theme_constant_override("separation", 7)
	_workspace.add_child(sculpt_row)
	var raise_button := Button.new()
	raise_button.text = "STOP RAISE" if _placement_mode == PlacementMode.SCULPT_RAISE else "RAISE"
	raise_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == PlacementMode.SCULPT_RAISE else PlacementMode.SCULPT_RAISE)
		_refresh_current_category()
	)
	sculpt_row.add_child(raise_button)
	var lower_button := Button.new()
	lower_button.text = "STOP LOWER" if _placement_mode == PlacementMode.SCULPT_LOWER else "LOWER"
	lower_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == PlacementMode.SCULPT_LOWER else PlacementMode.SCULPT_LOWER)
		_refresh_current_category()
	)
	sculpt_row.add_child(lower_button)
	var sculpt_info := Label.new()
	sculpt_info.text = "%.1f m radius • %.2f m/stamp" % [_sculpt_radius_m, _sculpt_strength_m]
	sculpt_info.modulate = Color(0.64, 0.76, 0.86)
	sculpt_row.add_child(sculpt_info)
	_add_number_field("Sculpt radius", _sculpt_radius_m, 0.5, 150.0, 0.5, " m", func(value: float) -> void:
		_sculpt_radius_m = value
		_update_preview()
	)
	_add_number_field("Stamp strength", _sculpt_strength_m, 0.01, 20.0, 0.01, " m", func(value: float) -> void:
		_sculpt_strength_m = value
	)
	_add_number_field("Sculpt hardness", _sculpt_hardness, 0.0, 0.98, 0.01, "", func(value: float) -> void:
		_sculpt_hardness = value
	)
	_add_note("Raise/Lower edits Deltas, the same sparse spherical layer consumed by terrain rendering and terrain contact. Untouched procedural terrain remains seed-generated. Drag with LMB; RMB/Esc stops the brush.")

	_section("Live viewport biome painting")
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("The current authoring body has no terrain profile.")
		return
	if _selected_biome_layer_id.is_empty() or terrain.call("find_biome_layer", _selected_biome_layer_id) == null:
		_add_note("Create/select a biome paint layer above, then arm viewport painting.")
		return
	var layer: Resource = terrain.call("find_biome_layer", _selected_biome_layer_id) as Resource
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)
	var paint_button := Button.new()
	paint_button.text = "STOP PAINTING" if _placement_mode == PlacementMode.BIOME else "PAINT IN VIEWPORT"
	paint_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == PlacementMode.BIOME else PlacementMode.BIOME)
		_refresh_current_category()
	)
	row.add_child(paint_button)
	var active_label := Label.new()
	active_label.text = "%s  •  radius %.1f m  •  hardness %.2f" % [BIOME_NAMES[int(layer.get(&"active_biome_id"))], float(layer.get(&"brush_radius_m")), float(layer.get(&"brush_hardness"))]
	active_label.modulate = Color(0.64, 0.76, 0.86)
	row.add_child(active_label)
	_add_note("Left-click the visible terrain to stamp. Hold left mouse and drag to paint continuously. The pick follows rendered/contact terrain, while the stored edit remains a sparse spherical post-generation override.")


func _build_water_page() -> void:
	super._build_water_page()
	if _world_host == null:
		return
	_section("Live 3D feature placement")
	var water: Resource = _session.active_water_profile() as Resource
	if water == null or _selected_water_feature_id.is_empty():
		_add_note("Create/select a lake or river above, then arm viewport placement.")
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		_add_note("Select a valid authored water feature.")
		return
	var is_river: bool = int(feature.get(&"feature_type")) == WATER_FEATURE_SCRIPT.FeatureType.RIVER
	var desired_mode: int = PlacementMode.RIVER if is_river else PlacementMode.LAKE
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)
	var place_button := Button.new()
	place_button.text = "STOP PLACEMENT" if _placement_mode == desired_mode else ("PLACE RIVER KNOTS" if is_river else "DRAW LAKE POLYGON")
	place_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == desired_mode else desired_mode)
		_refresh_current_category()
	)
	row.add_child(place_button)
	var remove_last := Button.new()
	remove_last.text = "Remove Last Point"
	remove_last.pressed.connect(func() -> void:
		_remove_last_water_point(feature)
		_refresh_current_category()
	)
	row.add_child(remove_last)
	_add_note("Each click stores a body-space 3D control point. Lakes close as a freeform polygon; rivers create cubic Bézier knots with width, depth and current metadata. The magenta preview is rebuilt in floating-origin render space.")


func _build_celestials_page() -> void:
	super._build_celestials_page()
	if _world_host != null:
		_section("Live body preview")
		_add_note("Selecting a body changes the authoring target immediately. Apply asks the live runtime host to rebuild the active non-star body from its staged generation/planet/atmosphere values.")


func _process(delta: float) -> void:
	if _world_host == null or _navigation_active or _placement_mode == PlacementMode.NONE:
		return
	_cursor_sample_accum += delta
	if _cursor_sample_accum < CURSOR_SAMPLE_INTERVAL_S:
		return
	_cursor_sample_accum = fmod(_cursor_sample_accum, CURSOR_SAMPLE_INTERVAL_S)
	var mouse_position := get_viewport().get_mouse_position()
	if not _is_live_viewport_point(mouse_position):
		_last_hit.clear()
		_update_preview()
		return
	_last_hit = _screen_aim(mouse_position)
	_update_live_status(mouse_position)
	_update_preview()


func _unhandled_input(event: InputEvent) -> void:
	if _world_host == null:
		return
	if event is InputEventKey:
		var key_event := event as InputEventKey
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_TAB:
			_set_navigation(not _navigation_active)
			get_viewport().set_input_as_handled()
			return
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_ESCAPE and _placement_mode != PlacementMode.NONE:
			_set_placement_mode(PlacementMode.NONE)
			get_viewport().set_input_as_handled()
			return
	if _navigation_active or _placement_mode == PlacementMode.NONE:
		return
	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed:
			_set_placement_mode(PlacementMode.NONE)
			get_viewport().set_input_as_handled()
			return
		if mouse_button.button_index == MOUSE_BUTTON_LEFT and mouse_button.pressed and _is_live_viewport_point(mouse_button.position):
			_last_hit = _screen_aim(mouse_button.position)
			_place_current_hit(false)
			get_viewport().set_input_as_handled()
			return
	if event is InputEventMouseMotion and _continuous_drag_mode():
		var mouse_motion := event as InputEventMouseMotion
		if (mouse_motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0 and _is_live_viewport_point(mouse_motion.position):
			_last_hit = _screen_aim(mouse_motion.position)
			_place_current_hit(true)
			get_viewport().set_input_as_handled()


func _continuous_drag_mode() -> bool:
	return _placement_mode == PlacementMode.BIOME \
		or _placement_mode == PlacementMode.SCULPT_RAISE \
		or _placement_mode == PlacementMode.SCULPT_LOWER


func _set_navigation(enabled: bool) -> void:
	_navigation_active = enabled
	if _player != null:
		_player.set("input_enabled", enabled)
		if _player.has_method("set_mouse_captured"):
			_player.call("set_mouse_captured", enabled)
	else:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED if enabled else Input.MOUSE_MODE_VISIBLE)
	if enabled:
		_last_hit.clear()
		_update_preview()
	if _live_status_label != null:
		_live_status_label.text = "VIEWPORT NAVIGATION — TAB to return to authoring" if enabled else _placement_status_text()


func _set_placement_mode(mode: int) -> void:
	_placement_mode = mode
	_last_paint_dir = Vector3.ZERO
	_last_sculpt_dir = Vector3.ZERO
	if mode != PlacementMode.NONE:
		_set_navigation(false)
	if _live_status_label != null:
		_live_status_label.text = _placement_status_text()
	_update_preview()


func _placement_status_text() -> String:
	match _placement_mode:
		PlacementMode.BIOME:
			return "BIOME PAINT ARMED — left click/drag terrain • RMB/Esc stop • TAB navigate"
		PlacementMode.LAKE:
			return "LAKE POLYGON ARMED — click shoreline vertices • RMB/Esc stop • TAB navigate"
		PlacementMode.RIVER:
			return "RIVER BÉZIER ARMED — click knots downstream • RMB/Esc stop • TAB navigate"
		PlacementMode.SCULPT_RAISE:
			return "RAISE TERRAIN — left click/drag • RMB/Esc stop • TAB navigate"
		PlacementMode.SCULPT_LOWER:
			return "LOWER TERRAIN — left click/drag • RMB/Esc stop • TAB navigate"
		_:
			return "LIVE VIEW — TAB: navigate"


func _is_live_viewport_point(point: Vector2) -> bool:
	return point.x > PANEL_WIDTH_PX + 2.0 and point.x < get_viewport_rect().size.x and point.y >= 0.0 and point.y < get_viewport_rect().size.y


func _screen_aim(screen_position: Vector2) -> Dictionary:
	if _player == null:
		_player = _world_host.get("player") as Node if _world_host != null else null
	if _player == null:
		return {}
	if _camera == null:
		_camera = _player.get("camera") as Camera3D
	if _camera == null or Planet.cfg == null or not Planet.ready_state:
		return {}
	var origin: Vec3D = Frames.to_world(_camera.global_position)
	var ray: Vector3 = _camera.project_ray_normal(screen_position).normalized()
	var step: float = AIM_COARSE_STEP_START_M
	var previous_t := 0.0
	var t := 0.0
	while t < PICK_MAX_RANGE_M:
		previous_t = t
		t = minf(t + step, PICK_MAX_RANGE_M)
		step = minf(step * AIM_COARSE_STEP_GROWTH, AIM_COARSE_STEP_MAX_M)
		if _coarse_gap_at(origin, ray, t) <= AIM_COARSE_CAPTURE_M:
			var lo: float = previous_t
			var hi: float = t
			for _i: int in AIM_BINARY_STEPS:
				var mid: float = (lo + hi) * 0.5
				if _coarse_gap_at(origin, ray, mid) <= AIM_COARSE_CAPTURE_M:
					hi = mid
				else:
					lo = mid
			var hit_t: float = hi
			var p: Vec3D = origin.add(Vec3D.from_v3(ray).mul(hit_t))
			var direction: Vector3 = p.normalized().to_v3()
			var coarse_height: float = TerrainContactSampler.coarse_height(direction)
			var height: float = TerrainContactSampler.contact_height(direction, coarse_height)
			var precise_gap: float = p.length() - (Planet.cfg.planet_radius + height)
			var radial_rate: float = ray.dot(direction)
			if absf(radial_rate) > 0.08:
				hit_t = clampf(hit_t - precise_gap / radial_rate, 0.0, PICK_MAX_RANGE_M)
				p = origin.add(Vec3D.from_v3(ray).mul(hit_t))
				direction = p.normalized().to_v3()
				coarse_height = TerrainContactSampler.coarse_height(direction)
				height = TerrainContactSampler.contact_height(direction, coarse_height)
			var surface_world: Vec3D = Vec3D.from_v3(direction).mul(Planet.cfg.planet_radius + height)
			return {"world": surface_world, "dir": direction, "distance": hit_t, "height": height}
	return {}


func _coarse_gap_at(origin: Vec3D, ray: Vector3, distance_m: float) -> float:
	var p: Vec3D = origin.add(Vec3D.from_v3(ray).mul(distance_m))
	var direction: Vector3 = p.normalized().to_v3()
	var height: float = TerrainContactSampler.coarse_height(direction)
	return p.length() - (Planet.cfg.planet_radius + height)


func _place_current_hit(continuous: bool) -> void:
	if _last_hit.is_empty():
		_set_status("Viewport pick did not intersect terrain.")
		return
	var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
	if direction.length_squared() < 0.99:
		return
	match _placement_mode:
		PlacementMode.BIOME:
			_place_biome_stroke(direction, continuous)
		PlacementMode.LAKE, PlacementMode.RIVER:
			_place_water_point()
		PlacementMode.SCULPT_RAISE:
			_place_sculpt_stroke(direction, continuous, 1.0)
		PlacementMode.SCULPT_LOWER:
			_place_sculpt_stroke(direction, continuous, -1.0)
	_update_preview()


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	if Planet.cfg == null:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if continuous and _last_sculpt_dir.length_squared() > 0.99:
		var arc_distance: float = acos(clampf(_last_sculpt_dir.dot(direction), -1.0, 1.0)) * planet_radius
		if arc_distance < maxf(_sculpt_radius_m * 0.16, Deltas.sample_spacing(planet_radius) * 1.5):
			return
	var changed: int = int(Deltas.apply_radial_brush(
		direction,
		_sculpt_radius_m,
		_sculpt_strength_m * sign_value,
		_sculpt_hardness,
		planet_radius,
		SCULPT_MIN_OFFSET_M,
		SCULPT_MAX_OFFSET_M
	))
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	var verb: String = "Raised" if sign_value > 0.0 else "Lowered"
	_set_status("%s terrain: %d samples • %.1f m radius • %.2f m stamp." % [verb, changed, _sculpt_radius_m, _sculpt_strength_m])


func _place_biome_stroke(direction: Vector3, continuous: bool) -> void:
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		return
	var layer: Resource = terrain.call("find_biome_layer", _selected_biome_layer_id) as Resource
	if layer == null:
		_set_status("Select a biome paint layer before painting.")
		return
	var radius_m: float = float(layer.get(&"brush_radius_m"))
	if continuous and _last_paint_dir.length_squared() > 0.99:
		var body: Resource = _session.active_body() as Resource
		var planet_radius: float = maxf(float(body.get(&"radius_m")), 1.0) if body != null else maxf(float(Planet.cfg.planet_radius), 1.0)
		var arc_distance: float = acos(clampf(_last_paint_dir.dot(direction), -1.0, 1.0)) * planet_radius
		if arc_distance < maxf(radius_m * 0.22, 0.25):
			return
	var biome_id: int = int(layer.get(&"active_biome_id"))
	var hardness: float = float(layer.get(&"brush_hardness"))
	var opacity: float = float(layer.get(&"brush_opacity"))
	if _session.add_biome_stroke(_selected_biome_layer_id, direction, biome_id, radius_m, hardness, opacity):
		_last_paint_dir = direction
		_set_status("Painted %s at %.1f m radius." % [BIOME_NAMES[biome_id], radius_m])


func _place_water_point() -> void:
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		_set_status("Select a lake or river before placing control points.")
		return
	var world: Vec3D = _last_hit.get("world") as Vec3D
	if world == null:
		return
	var body_point := Vector3(float(world.x), float(world.y), float(world.z))
	_session.stage_action("Place water control point", func() -> void:
		if _placement_mode == PlacementMode.RIVER:
			feature.call("add_river_knot", body_point, 20.0, float(feature.get(&"default_depth_m")), 1.0)
		else:
			feature.call("add_lake_point", body_point)
	, SESSION_SCRIPT.ApplyScope.TILES)
	_set_status("Added %s control point." % ("river" if _placement_mode == PlacementMode.RIVER else "lake"))


func _remove_last_water_point(feature: Resource) -> void:
	if feature == null:
		return
	var is_river: bool = int(feature.get(&"feature_type")) == WATER_FEATURE_SCRIPT.FeatureType.RIVER
	var count: int = (feature.get(&"river_knots") as Array).size() if is_river else (feature.get(&"lake_polygon_body_m") as PackedVector3Array).size()
	if count <= 0:
		return
	_session.stage_action("Remove water control point", func() -> void:
		if is_river:
			feature.call("remove_river_knot", count - 1)
		else:
			feature.call("remove_lake_point", count - 1)
	, SESSION_SCRIPT.ApplyScope.TILES)
	_update_preview()


func _create_live_preview() -> void:
	if _world_host == null or not (_world_host is Node3D):
		return
	_preview_mesh = ImmediateMesh.new()
	_preview_instance = MeshInstance3D.new()
	_preview_instance.name = "PlanetStudioLivePreview"
	_preview_instance.mesh = _preview_mesh
	_preview_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_preview_instance.extra_cull_margin = 1000000.0
	_preview_material = StandardMaterial3D.new()
	_preview_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_preview_material.vertex_color_use_as_albedo = true
	_preview_material.no_depth_test = true
	_preview_instance.material_override = _preview_material
	_world_host.add_child(_preview_instance)


func _update_preview() -> void:
	if _preview_mesh == null:
		return
	_preview_mesh.clear_surfaces()
	if _navigation_active:
		return
	if not _last_hit.is_empty():
		var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
		var height: float = float(_last_hit.get("height", 0.0))
		var radius := 2.0
		var color := Color(1.0, 0.22, 0.82, 1.0)
		match _placement_mode:
			PlacementMode.BIOME:
				color = Color(0.25, 0.86, 1.0, 1.0)
				var terrain: Resource = _session.active_terrain_profile() as Resource
				if terrain != null:
					var layer: Resource = terrain.call("find_biome_layer", _selected_biome_layer_id) as Resource
					if layer != null:
						radius = maxf(0.1, float(layer.get(&"brush_radius_m")))
			PlacementMode.SCULPT_RAISE:
				radius = _sculpt_radius_m
				color = Color(0.34, 1.0, 0.46, 1.0)
			PlacementMode.SCULPT_LOWER:
				radius = _sculpt_radius_m
				color = Color(1.0, 0.48, 0.20, 1.0)
		_draw_surface_ring(direction, height, radius, color)
	_draw_selected_water_feature()


func _draw_surface_ring(direction: Vector3, height: float, radius_m: float, color: Color) -> void:
	if direction.length_squared() < 0.99 or Planet.cfg == null:
		return
	var up := direction.normalized()
	var reference := Vector3.UP if absf(up.dot(Vector3.UP)) < 0.95 else Vector3.RIGHT
	var tangent_x := reference.cross(up).normalized()
	var tangent_y := up.cross(tangent_x).normalized()
	var base_radius: float = Planet.cfg.planet_radius + height + 0.08
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for index: int in 65:
		var angle: float = TAU * float(index) / 64.0
		var offset: Vector3 = tangent_x * cos(angle) * radius_m + tangent_y * sin(angle) * radius_m
		var world_point: Vec3D = Vec3D.from_v3(up).mul(base_radius).add(Vec3D.from_v3(offset))
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(world_point))
	_preview_mesh.surface_end()


func _draw_selected_water_feature() -> void:
	if _selected_water_feature_id.is_empty():
		return
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		return
	var color := Color(1.0, 0.18, 0.78, 1.0)
	var is_river: bool = int(feature.get(&"feature_type")) == WATER_FEATURE_SCRIPT.FeatureType.RIVER
	var points: Array[Vector3] = []
	if is_river:
		var segment_count: int = int(feature.call("river_segment_count"))
		for segment: int in segment_count:
			for sample_index: int in 17:
				if segment > 0 and sample_index == 0:
					continue
				points.append(Vector3(feature.call("sample_river_segment", segment, float(sample_index) / 16.0)))
	else:
		var polygon: PackedVector3Array = feature.get(&"lake_polygon_body_m")
		for point: Vector3 in polygon:
			points.append(point)
		if polygon.size() > 2:
			points.append(polygon[0])
	if points.size() < 2:
		return
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for point: Vector3 in points:
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(Vec3D.from_v3(point)))
	_preview_mesh.surface_end()


func _update_live_status(mouse_position: Vector2) -> void:
	if _live_status_label == null:
		return
	if _last_hit.is_empty():
		_live_status_label.text = "%s\nNo terrain hit at %.0f, %.0f" % [_placement_status_text(), mouse_position.x, mouse_position.y]
		return
	_live_status_label.text = "%s\nterrain %.2f m MSL • range %.1f m" % [_placement_status_text(), float(_last_hit.get("height", 0.0)), float(_last_hit.get("distance", 0.0))]


func _exit_tree() -> void:
	if _preview_instance != null and is_instance_valid(_preview_instance):
		_preview_instance.queue_free()
