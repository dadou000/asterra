extends "res://scripts/world_authoring/world_authoring_editor_live_phase46.gd"
## Pre-0.1.0 Planet Studio authoring workbench.
##
## This entry point composes the global map, outliner/workspace, advanced sparse
## sculpt tools, staged/applied comparison, regional hydrology refresh, custom
## feature recipes, seed exploration and authoring diagnostics around the existing
## authoritative WorldAuthoringSession. No production terrain representation is
## duplicated here.

const WORKSPACE_SCRIPT := preload("res://scripts/world_authoring/planet_studio_workspace.gd")
const MAP_VIEW_SCRIPT := preload("res://scripts/world_authoring/planet_studio_map_view.gd")
const SCULPT_OPS := preload("res://scripts/world_authoring/planet_studio_sculpt_ops.gd")
const REGIONAL_HYDROLOGY := preload("res://scripts/world_authoring/planet_studio_regional_hydrology.gd")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const FEATURE_PRESETS := preload("res://scripts/world_authoring/model/terrain_feature_preset_catalog.gd")
const SLOT_MODEL := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const BIOME_LAYER_MODEL := preload("res://scripts/world_authoring/model/biome_paint_layer.gd")
const WATER_FEATURE_MODEL := preload("res://scripts/world_authoring/model/water_feature_definition.gd")

const WORKBENCH_CATEGORY := "WORKBENCH"
const WB_MAP := 0
const WB_OUTLINER := 1
const WB_SCULPT := 2
const WB_COMPARE := 3
const WB_HYDROLOGY := 4
const WB_PRESETS := 5
const WB_SEEDS := 6
const WB_DIAGNOSTICS := 7

const ADV_SCULPT_SMOOTH := 100
const ADV_SCULPT_FLATTEN := 101
const ADV_SCULPT_TERRACE := 102
const ADV_SCULPT_NOISE := 103
const ADV_SCULPT_ERODE := 104
const ADV_SCULPT_RESTORE := 105
const ADV_SCULPT_STAMP := 106
const ADV_SCULPT_RAMP_START := 107
const ADV_SCULPT_RAMP_END := 108

var _ps_workspace := WORKSPACE_SCRIPT.new()
var _wb_tab: int = WB_MAP
var _map_view: PlanetStudioMapView
var _map_action: String = "focus"
var _map_preset_id: String = "mountain_range"
var _map_selected_feature_id: String = ""
var _last_map_direction := Vector3(1, 0, 0)

var _advanced_sculpt_strength: float = 0.55
var _advanced_sculpt_target_height_m: float = 0.0
var _advanced_sculpt_terrace_step_m: float = 2.0
var _advanced_sculpt_noise_amplitude_m: float = 1.5
var _advanced_sculpt_seed: int = 1337
var _ramp_start_dir := Vector3.ZERO
var _ramp_start_height_m: float = 0.0
var _ramp_half_width_m: float = 4.0
var _ramp_shoulder_m: float = 8.0
var _stamp_image: Image
var _stamp_path: String = ""
var _stamp_height_scale_m: float = 25.0
var _stamp_absolute: bool = false
var _stamp_dialog: FileDialog
var _export_dialog: FileDialog
var _compare_show_applied_runtime: bool = false

var _hydrology_center := Vector3.ZERO
var _hydrology_radius_m: float = 25000.0
var _hydrology_runner: PlanetStudioRegionalHydrology
var _hydrology_status: String = "No regional re-analysis has run yet."
var _seed_candidates: PackedInt64Array = PackedInt64Array()


func _ready() -> void:
	super._ready()
	_bind_workspace_to_active_body()
	_build_workbench_dialogs()
	_hydrology_runner = REGIONAL_HYDROLOGY.new()
	_hydrology_runner.progress.connect(func(label: String, fraction: float) -> void:
		_hydrology_status = "%s — %.0f%%" % [label, fraction * 100.0]
		if _category == WORKBENCH_CATEGORY and _wb_tab == WB_HYDROLOGY:
			_refresh_current_category()
	)
	_hydrology_runner.completed.connect(func(cells: int, elapsed_ms: float) -> void:
		_hydrology_status = "Updated %d coarse cells in %.1f ms." % [cells, elapsed_ms]
		_ps_workspace.set_timing("regional_hydrology", elapsed_ms)
		_ps_workspace.clear_stale_region_near(_hydrology_center, _hydrology_radius_m)
		if _map_view != null:
			_map_view.refresh()
	)


func _build_live_toolbar() -> HBoxContainer:
	var toolbar := super._build_live_toolbar()
	var workbench := _compact_toolbar_button("Tools", 58.0)
	workbench.tooltip_text = "Planet-scale map, outliner, sculpt, compare, hydrology, presets, seeds and diagnostics."
	workbench.pressed.connect(_show_category.bind(WORKBENCH_CATEGORY))
	toolbar.add_child(workbench)
	return toolbar


func _build_shell() -> void:
	super._build_shell()
	# The inherited live shell owns the narrow navigation rail. Add Workbench next
	# to the canonical categories rather than replacing the mature category shell.
	var authoring_label := _find_label_recursive(self, "AUTHORING")
	if authoring_label == null:
		return
	var navigation := authoring_label.get_parent() as VBoxContainer
	if navigation == null or _find_button_recursive(navigation, WORKBENCH_CATEGORY) != null:
		return
	var button := Button.new()
	button.text = WORKBENCH_CATEGORY
	button.custom_minimum_size.y = 40.0
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.pressed.connect(_show_category.bind(WORKBENCH_CATEGORY))
	# Insert before the expandable spacer / navigate button.
	navigation.add_child(button)
	if navigation.get_child_count() >= 3:
		navigation.move_child(button, maxi(1, navigation.get_child_count() - 3))


func _show_category(category_name: String) -> void:
	if category_name == WORKBENCH_CATEGORY:
		_category = WORKBENCH_CATEGORY
		_clear_workspace()
		_build_workbench_page()
		return
	super._show_category(category_name)


func _on_body_selected(index: int) -> void:
	super._on_body_selected(index)
	_bind_workspace_to_active_body()
	if _map_view != null:
		_map_view.bind(_session, _ps_workspace)


func _on_undo_pressed() -> void:
	super._on_undo_pressed()
	_sync_runtime_deltas_from_staged()


func _on_redo_pressed() -> void:
	super._on_redo_pressed()
	_sync_runtime_deltas_from_staged()


func _on_revert_pressed() -> void:
	super._on_revert_pressed()
	_sync_runtime_deltas_from_staged()


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	var before := Deltas.serialize()
	super._place_sculpt_stroke(direction, continuous, sign_value)
	var after := Deltas.serialize()
	if before != after:
		_commit_runtime_sculpt("Raise terrain" if sign_value > 0.0 else "Lower terrain",
			direction, _sculpt_radius_m)


func _continuous_drag_mode() -> bool:
	return super._continuous_drag_mode() or _placement_mode in [
		ADV_SCULPT_SMOOTH, ADV_SCULPT_FLATTEN, ADV_SCULPT_TERRACE,
		ADV_SCULPT_NOISE, ADV_SCULPT_ERODE, ADV_SCULPT_RESTORE,
	]


func _placement_status_text() -> String:
	match _placement_mode:
		ADV_SCULPT_SMOOTH: return "SMOOTH — left click/drag • RMB/Esc stop • TAB navigate"
		ADV_SCULPT_FLATTEN: return "FLATTEN — left click/drag to target altitude • RMB/Esc stop"
		ADV_SCULPT_TERRACE: return "TERRACE — left click/drag • RMB/Esc stop"
		ADV_SCULPT_NOISE: return "NOISE — left click/drag • RMB/Esc stop"
		ADV_SCULPT_ERODE: return "LOCAL ERODE — left click/drag • RMB/Esc stop"
		ADV_SCULPT_RESTORE: return "RESTORE PROCEDURAL — left click/drag • RMB/Esc stop"
		ADV_SCULPT_STAMP: return "HEIGHT STAMP — click terrain once • RMB/Esc stop"
		ADV_SCULPT_RAMP_START: return "GRADE CORRIDOR — click start point"
		ADV_SCULPT_RAMP_END: return "GRADE CORRIDOR — click end point"
	return super._placement_status_text()


func _place_current_hit(continuous: bool) -> void:
	if _placement_mode < ADV_SCULPT_SMOOTH:
		super._place_current_hit(continuous)
		return
	if _last_hit.is_empty():
		_set_status("Viewport pick did not intersect terrain.")
		return
	var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
	if direction.length_squared() < 0.5:
		return
	var radius := _active_planet_radius()
	var changed := 0
	match _placement_mode:
		ADV_SCULPT_SMOOTH:
			changed = SCULPT_OPS.apply_filter(SCULPT_OPS.Mode.SMOOTH, direction,
				_sculpt_radius_m, _advanced_sculpt_strength, _sculpt_hardness, radius)
		ADV_SCULPT_FLATTEN:
			changed = SCULPT_OPS.apply_filter(SCULPT_OPS.Mode.FLATTEN, direction,
				_sculpt_radius_m, _advanced_sculpt_strength, _sculpt_hardness, radius,
				_advanced_sculpt_target_height_m)
		ADV_SCULPT_TERRACE:
			changed = SCULPT_OPS.apply_filter(SCULPT_OPS.Mode.TERRACE, direction,
				_sculpt_radius_m, _advanced_sculpt_strength, _sculpt_hardness, radius,
				0.0, _advanced_sculpt_seed, _advanced_sculpt_terrace_step_m)
		ADV_SCULPT_NOISE:
			changed = SCULPT_OPS.apply_filter(SCULPT_OPS.Mode.NOISE, direction,
				_sculpt_radius_m, _advanced_sculpt_strength, _sculpt_hardness, radius,
				0.0, _advanced_sculpt_seed, _advanced_sculpt_noise_amplitude_m)
		ADV_SCULPT_ERODE:
			changed = SCULPT_OPS.apply_filter(SCULPT_OPS.Mode.ERODE, direction,
				_sculpt_radius_m, _advanced_sculpt_strength, _sculpt_hardness, radius)
		ADV_SCULPT_RESTORE:
			changed = SCULPT_OPS.apply_filter(SCULPT_OPS.Mode.RESTORE, direction,
				_sculpt_radius_m, _advanced_sculpt_strength, _sculpt_hardness, radius)
		ADV_SCULPT_STAMP:
			if _stamp_image != null:
				changed = SCULPT_OPS.apply_image_stamp(_stamp_image, direction,
					_sculpt_radius_m, _stamp_height_scale_m, radius,
					_advanced_sculpt_strength, _stamp_absolute,
					_advanced_sculpt_target_height_m)
			_set_placement_mode(PlacementMode.NONE)
		ADV_SCULPT_RAMP_START:
			_ramp_start_dir = direction.normalized()
			_ramp_start_height_m = float(_last_hit.get("height", 0.0))
			_set_placement_mode(ADV_SCULPT_RAMP_END)
			_set_status("Corridor start sampled at %.2f m. Click the end point." % _ramp_start_height_m)
			return
		ADV_SCULPT_RAMP_END:
			var end_height := float(_last_hit.get("height", 0.0))
			changed = SCULPT_OPS.apply_corridor_grade(_ramp_start_dir, direction,
				_ramp_start_height_m, end_height, _ramp_half_width_m,
				_ramp_shoulder_m, radius, _advanced_sculpt_strength)
			_set_placement_mode(PlacementMode.NONE)
	if changed > 0:
		_commit_runtime_sculpt("Advanced terrain sculpt", direction, _sculpt_radius_m)
		_set_status("Advanced sculpt changed %d sparse terrain samples." % changed)
	_update_preview()


func _unhandled_input(event: InputEvent) -> void:
	# Hold V for a fast 3D sculpt-delta applied/staged A/B view. Graph/material/
	# biome/water differences remain visible in the Workbench Compare page.
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.keycode == KEY_V and not key.echo:
			if key.pressed and not _compare_show_applied_runtime:
				_compare_show_applied_runtime = true
				_show_applied_sculpt_runtime()
				get_viewport().set_input_as_handled()
				return
			elif not key.pressed and _compare_show_applied_runtime:
				_compare_show_applied_runtime = false
				_sync_runtime_deltas_from_staged()
				get_viewport().set_input_as_handled()
				return
	super._unhandled_input(event)


func _build_workbench_page() -> void:
	_page_title("Planet Workbench",
		"Planet-scale authoring, organization, non-destructive sculpt tools, dependency refresh and diagnostics. All edits target the same staged planet resources and sparse Deltas lattice used by the live runtime.")
	var tabs := HBoxContainer.new()
	tabs.add_theme_constant_override("separation", 5)
	_workspace.add_child(tabs)
	for item in [
		[WB_MAP, "MAP"], [WB_OUTLINER, "OUTLINER"], [WB_SCULPT, "SCULPT"],
		[WB_COMPARE, "COMPARE"], [WB_HYDROLOGY, "HYDRO"], [WB_PRESETS, "PRESETS"],
		[WB_SEEDS, "SEEDS"], [WB_DIAGNOSTICS, "DIAGNOSTICS"],
	]:
		var button := Button.new()
		button.text = String(item[1])
		button.button_pressed = int(item[0]) == _wb_tab
		button.pressed.connect(func() -> void:
			_wb_tab = int(item[0])
			_refresh_current_category()
		)
		tabs.add_child(button)
	_workspace.add_child(HSeparator.new())
	match _wb_tab:
		WB_MAP: _build_map_page()
		WB_OUTLINER: _build_outliner_page()
		WB_SCULPT: _build_sculpt_page()
		WB_COMPARE: _build_compare_page()
		WB_HYDROLOGY: _build_hydrology_page()
		WB_PRESETS: _build_presets_page()
		WB_SEEDS: _build_seeds_page()
		WB_DIAGNOSTICS: _build_diagnostics_page()


func _build_map_page() -> void:
	_section("Global authoring map")
	var controls := HBoxContainer.new()
	controls.add_theme_constant_override("separation", 6)
	_workspace.add_child(controls)
	var layer := OptionButton.new()
	for label in PlanetMap.LAYER_NAMES:
		layer.add_item(String(label))
	layer.select(int(_ps_workspace.ui_value("map_layer", PlanetMap.Layer.ELEVATION)))
	layer.item_selected.connect(func(index: int) -> void:
		_ps_workspace.set_ui_value("map_layer", index)
		if _map_view != null: _map_view.set_layer(index)
	)
	controls.add_child(layer)
	var action := OptionButton.new()
	for item in [
		["Focus 3D here", "focus"], ["Move selected feature", "move_feature"],
		["Paint selected biome", "paint_biome"], ["Place terrain preset", "place_preset"],
		["Add bookmark", "bookmark"], ["Set hydro region centre", "hydro"],
	]:
		action.add_item(String(item[0]))
		action.set_item_metadata(action.item_count - 1, String(item[1]))
	controls.add_child(action)
	action.item_selected.connect(func(index: int) -> void:
		_map_action = String(action.get_item_metadata(index))
	)
	var preset := OptionButton.new()
	for preset_id in FEATURE_PRESETS.IDS:
		preset.add_item(FEATURE_PRESETS.label(preset_id))
		preset.set_item_metadata(preset.item_count - 1, preset_id)
	preset.item_selected.connect(func(index: int) -> void:
		_map_preset_id = String(preset.get_item_metadata(index))
	)
	controls.add_child(preset)

	var overlays := HBoxContainer.new()
	_workspace.add_child(overlays)
	for item in [
		["Features", "features"], ["Biomes", "biomes"], ["Water", "water"],
		["Bookmarks", "bookmarks"], ["Stale regions", "stale"], ["Sculpt diff", "diff"],
	]:
		var check := CheckBox.new()
		check.text = String(item[0])
		check.button_pressed = true if String(item[1]) != "diff" else false
		check.toggled.connect(_set_map_overlay.bind(String(item[1])))
		overlays.add_child(check)

	_map_view = MAP_VIEW_SCRIPT.new()
	_workspace.add_child(_map_view)
	_map_view.bind(_session, _ps_workspace)
	_map_view.set_layer(layer.selected)
	_map_view.map_clicked.connect(_on_map_clicked)
	_map_view.feature_clicked.connect(func(slot_id: String) -> void:
		_map_selected_feature_id = slot_id
		_phase43_selected_feature_id = slot_id
		_set_status("Selected terrain feature from map: %s" % slot_id)
		_map_view.refresh_overlays()
	)
	_update_map_compare_snapshots()
	_add_note("Left click uses the selected map action. Clicking a guided feature selects it. Map layers are the same Planet.fields inspector contract used by the existing whole-planet map; authoring overlays come from the staged WorldAuthoringSession.")


func _set_map_overlay(enabled: bool, key: String) -> void:
	if _map_view == null:
		return
	match key:
		"features": _map_view.show_features = enabled
		"biomes": _map_view.show_biomes = enabled
		"water": _map_view.show_water = enabled
		"bookmarks": _map_view.show_bookmarks = enabled
		"stale": _map_view.show_stale_regions = enabled
		"diff": _map_view.show_sculpt_diff = enabled
	_map_view.refresh_overlays()


func _on_map_clicked(direction: Vector3, button: int) -> void:
	if button != MOUSE_BUTTON_LEFT:
		_focus_direction_3d(direction)
		return
	_last_map_direction = direction.normalized()
	match _map_action:
		"focus": _focus_direction_3d(direction)
		"move_feature": _move_selected_feature_to(direction)
		"paint_biome": _paint_selected_biome_at(direction)
		"place_preset": _place_builtin_preset_at(_map_preset_id, direction)
		"bookmark":
			_ps_workspace.add_bookmark("Bookmark %d" % (_ps_workspace.bookmarks.size() + 1), direction)
			_set_status("Added geographic bookmark.")
		"hydro":
			_hydrology_center = direction.normalized()
			_ps_workspace.mark_region_stale(direction, _hydrology_radius_m,
				PackedStringArray(["drainage", "watersheds", "soil", "biomes"]),
				"Author selected a hydrology refresh region")
			_set_status("Hydrology region centre set from global map.")
	if _map_view != null:
		_map_view.refresh_overlays()


func _build_outliner_page() -> void:
	_section("Authoring outliner")
	var group_row := HBoxContainer.new()
	_workspace.add_child(group_row)
	var add_group := Button.new()
	add_group.text = "+ Group"
	add_group.pressed.connect(func() -> void:
		_ps_workspace.add_group("Group %d" % (_ps_workspace.groups.size() + 1))
		_refresh_current_category()
	)
	group_row.add_child(add_group)
	var add_bookmark := Button.new()
	add_bookmark.text = "+ Bookmark at last map point"
	add_bookmark.pressed.connect(func() -> void:
		_ps_workspace.add_bookmark("Bookmark %d" % (_ps_workspace.bookmarks.size() + 1), _last_map_direction)
		_refresh_current_category()
	)
	group_row.add_child(add_bookmark)

	for group in _ps_workspace.groups:
		var header := HBoxContainer.new()
		_workspace.add_child(header)
		var name_edit := LineEdit.new()
		name_edit.text = String(group.get("name", "Group"))
		name_edit.custom_minimum_size.x = 280.0
		name_edit.text_submitted.connect(func(value: String) -> void:
			_ps_workspace.rename_group(String(group.get("id", "")), value)
			_refresh_current_category()
		)
		header.add_child(name_edit)
		var remove := Button.new()
		remove.text = "Delete Group"
		remove.pressed.connect(func() -> void:
			_ps_workspace.remove_group(String(group.get("id", "")))
			_refresh_current_category()
		)
		header.add_child(remove)

	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain != null:
		_section("Terrain Features")
		for slot in _guided_feature_slots(terrain):
			_build_outliner_resource_row("feature", slot, String(slot.get(&"slot_id")),
				String(slot.get(&"display_name")), Callable(self, "_duplicate_feature"))
		_section("Sculpt Layer")
		var sculpt_label := Label.new()
		sculpt_label.text = "Deltas — %d edited tiles • persistent sparse cube-sphere layer" % terrain.call("sculpt_edited_tile_count")
		_workspace.add_child(sculpt_label)
		_section("Biome Paint")
		for layer in terrain.get(&"biome_override_layers") as Array:
			if layer is Resource:
				_build_outliner_resource_row("biome", layer, String(layer.get(&"layer_id")),
					String(layer.get(&"display_name")), Callable(self, "_duplicate_biome_layer"))
	var water: Resource = _session.active_water_profile() as Resource
	if water != null:
		_section("Water")
		for feature in water.get(&"features") as Array:
			if feature is Resource:
				_build_outliner_resource_row("water", feature, String(feature.get(&"feature_id")),
					String(feature.get(&"display_name")), Callable(self, "_duplicate_water_feature"))
	_section("Regions / Bookmarks")
	for bookmark in _ps_workspace.bookmarks:
		var row := HBoxContainer.new()
		_workspace.add_child(row)
		var label := Label.new()
		label.text = "⌖ %s" % String(bookmark.get("name", "Bookmark"))
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(label)
		var focus := Button.new()
		focus.text = "Focus"
		focus.pressed.connect(_focus_direction_3d.bind(_ps_workspace.bookmark_direction(bookmark)))
		row.add_child(focus)
		var remove := Button.new()
		remove.text = "Delete"
		remove.pressed.connect(func() -> void:
			_ps_workspace.remove_bookmark(String(bookmark.get("id", "")))
			_refresh_current_category()
		)
		row.add_child(remove)


func _build_outliner_resource_row(kind: String, resource: Resource, id: String,
		display_name: String, duplicate_callable: Callable) -> void:
	var panel := HBoxContainer.new()
	panel.add_theme_constant_override("separation", 4)
	_workspace.add_child(panel)
	var enabled := CheckBox.new()
	enabled.button_pressed = bool(resource.get(&"enabled")) if _has_property(resource, &"enabled") else true
	enabled.tooltip_text = "Enable/disable this authoritative authoring object."
	enabled.toggled.connect(func(value: bool) -> void:
		if _ps_workspace.is_locked(kind, id): return
		if _has_property(resource, &"enabled"):
			_session.stage_set(resource, &"enabled", value, WorldAuthoringSession.ApplyScope.GRAPH,
				"Toggle %s" % display_name)
		_refresh_current_category()
	)
	panel.add_child(enabled)
	var name_edit := LineEdit.new()
	name_edit.text = display_name
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_edit.text_submitted.connect(func(value: String) -> void:
		if _ps_workspace.is_locked(kind, id): return
		_session.stage_set(resource, &"display_name", value, WorldAuthoringSession.ApplyScope.GRAPH,
			"Rename %s" % display_name)
		_refresh_current_category()
	)
	panel.add_child(name_edit)
	var hidden := Button.new()
	hidden.text = "Show" if _ps_workspace.is_hidden(kind, id) else "Hide"
	hidden.pressed.connect(func() -> void:
		_ps_workspace.set_hidden(kind, id, not _ps_workspace.is_hidden(kind, id))
		_refresh_current_category()
	)
	panel.add_child(hidden)
	var lock := Button.new()
	lock.text = "Unlock" if _ps_workspace.is_locked(kind, id) else "Lock"
	lock.pressed.connect(func() -> void:
		_ps_workspace.set_locked(kind, id, not _ps_workspace.is_locked(kind, id))
		_refresh_current_category()
	)
	panel.add_child(lock)
	var duplicate := Button.new()
	duplicate.text = "Duplicate"
	duplicate.disabled = _ps_workspace.is_locked(kind, id)
	duplicate.pressed.connect(func() -> void:
		duplicate_callable.call(resource)
		_refresh_current_category()
	)
	panel.add_child(duplicate)
	var solo := Button.new()
	solo.text = "Solo"
	solo.pressed.connect(func() -> void:
		_solo_outliner_item(kind, id)
		_refresh_current_category()
	)
	panel.add_child(solo)
	var groups := OptionButton.new()
	groups.add_item("No group")
	groups.set_item_metadata(0, "")
	var selected := 0
	for group in _ps_workspace.groups:
		groups.add_item(String(group.get("name", "Group")))
		groups.set_item_metadata(groups.item_count - 1, String(group.get("id", "")))
		if _ps_workspace.group_for(kind, id) == String(group.get("id", "")):
			selected = groups.item_count - 1
	groups.select(selected)
	groups.item_selected.connect(func(index: int) -> void:
		_ps_workspace.assign_to_group(String(groups.get_item_metadata(index)), kind, id)
	)
	panel.add_child(groups)


func _build_sculpt_page() -> void:
	_section("Advanced sparse sculpt")
	_add_note("Every operation writes Deltas, the same sparse spherical lattice used by contact and rendering. Smooth/terrace/noise/erosion never bake a second local heightfield; Restore moves authored offsets back toward zero.")
	var tools := GridContainer.new()
	tools.columns = 4
	_workspace.add_child(tools)
	for item in [
		["Smooth", ADV_SCULPT_SMOOTH], ["Flatten", ADV_SCULPT_FLATTEN],
		["Terrace", ADV_SCULPT_TERRACE], ["Noise", ADV_SCULPT_NOISE],
		["Erode", ADV_SCULPT_ERODE], ["Restore Procedural", ADV_SCULPT_RESTORE],
		["Image Stamp", ADV_SCULPT_STAMP], ["Road / Rail Grade", ADV_SCULPT_RAMP_START],
	]:
		var button := Button.new()
		button.text = String(item[0])
		button.pressed.connect(func() -> void:
			if int(item[1]) == ADV_SCULPT_STAMP and _stamp_image == null:
				_stamp_dialog.popup_centered_ratio(0.70)
				return
			_set_placement_mode(int(item[1]))
		)
		tools.add_child(button)
	_add_number_field("Brush radius", _sculpt_radius_m, 0.5, 2000.0, 0.5, " m", func(value: float) -> void:
		_sculpt_radius_m = value
	)
	_add_number_field("Operator strength", _advanced_sculpt_strength, 0.01, 1.0, 0.01, "", func(value: float) -> void:
		_advanced_sculpt_strength = value
	)
	_add_number_field("Hardness", _sculpt_hardness, 0.0, 0.98, 0.01, "", func(value: float) -> void:
		_sculpt_hardness = value
	)
	_add_number_field("Flatten / absolute stamp target", _advanced_sculpt_target_height_m, -10000.0, 20000.0, 0.1, " m", func(value: float) -> void:
		_advanced_sculpt_target_height_m = value
	)
	var sample_target := Button.new()
	sample_target.text = "Use last map point height as flatten target"
	sample_target.pressed.connect(func() -> void:
		_advanced_sculpt_target_height_m = SCULPT_OPS.sample_current_height(_last_map_direction)
		_refresh_current_category()
	)
	_workspace.add_child(sample_target)
	_add_number_field("Terrace step", _advanced_sculpt_terrace_step_m, 0.05, 1000.0, 0.05, " m", func(value: float) -> void:
		_advanced_sculpt_terrace_step_m = value
	)
	_add_number_field("Noise / stamp scale", _advanced_sculpt_noise_amplitude_m, 0.01, 1000.0, 0.05, " m", func(value: float) -> void:
		_advanced_sculpt_noise_amplitude_m = value
	)
	_add_number_field("Corridor half-width", _ramp_half_width_m, 0.5, 500.0, 0.5, " m", func(value: float) -> void:
		_ramp_half_width_m = value
	)
	_add_number_field("Corridor shoulder", _ramp_shoulder_m, 0.0, 1000.0, 0.5, " m", func(value: float) -> void:
		_ramp_shoulder_m = value
	)
	_section("Heightfield / stamp exchange")
	var exchange := HBoxContainer.new()
	_workspace.add_child(exchange)
	var import_stamp := Button.new()
	import_stamp.text = "Import PNG/EXR Stamp"
	import_stamp.pressed.connect(func() -> void: _stamp_dialog.popup_centered_ratio(0.72))
	exchange.add_child(import_stamp)
	var export_stamp := Button.new()
	export_stamp.text = "Export Local EXR"
	export_stamp.pressed.connect(func() -> void: _export_dialog.popup_centered_ratio(0.72))
	exchange.add_child(export_stamp)
	var absolute := CheckBox.new()
	absolute.text = "Stamp as absolute height"
	absolute.button_pressed = _stamp_absolute
	absolute.toggled.connect(func(value: bool) -> void: _stamp_absolute = value)
	exchange.add_child(absolute)
	_add_number_field("Imported stamp height range", _stamp_height_scale_m, 0.01, 10000.0, 0.1, " m", func(value: float) -> void:
		_stamp_height_scale_m = value
	)
	if not _stamp_path.is_empty():
		_add_note("Loaded stamp: %s" % _stamp_path)


func _build_compare_page() -> void:
	_section("Staged ↔ Applied comparison")
	var staged_terrain := _session.active_terrain_profile() as Resource
	var applied_terrain := _applied_active_terrain()
	var staged_tiles := int(staged_terrain.call("sculpt_edited_tile_count")) if staged_terrain != null else 0
	var applied_tiles := int(applied_terrain.call("sculpt_edited_tile_count")) if applied_terrain != null else 0
	_add_note("Hold V in the 3D viewport to temporarily show the applied sculpt-delta state; release V to restore staged sculpting. This A/B switch only swaps the sparse sculpt layer and never changes the staged document.")
	_add_note("Sculpt tiles: staged %d • applied %d • delta %+d" % [staged_tiles, applied_tiles, staged_tiles - applied_tiles])
	var staged_features := _guided_feature_slots(staged_terrain).size() if staged_terrain != null else 0
	var applied_features := _guided_feature_slots(applied_terrain).size() if applied_terrain != null else 0
	_add_note("Guided terrain features: staged %d • applied %d" % [staged_features, applied_features])
	var staged_biomes := (staged_terrain.get(&"biome_override_layers") as Array).size() if staged_terrain != null else 0
	var applied_biomes := (applied_terrain.get(&"biome_override_layers") as Array).size() if applied_terrain != null else 0
	_add_note("Biome paint layers: staged %d • applied %d" % [staged_biomes, applied_biomes])
	var staged_water := _session.active_water_profile() as Resource
	var applied_water := _applied_active_water()
	_add_note("Authored water features: staged %d • applied %d" % [
		(staged_water.get(&"features") as Array).size() if staged_water != null else 0,
		(applied_water.get(&"features") as Array).size() if applied_water != null else 0,
	])
	var show_applied := Button.new()
	show_applied.text = "Show Applied Sculpt Now"
	show_applied.pressed.connect(_show_applied_sculpt_runtime)
	_workspace.add_child(show_applied)
	var restore := Button.new()
	restore.text = "Restore Staged Sculpt"
	restore.pressed.connect(_sync_runtime_deltas_from_staged)
	_workspace.add_child(restore)
	var map := MAP_VIEW_SCRIPT.new()
	_workspace.add_child(map)
	map.bind(_session, _ps_workspace)
	map.show_sculpt_diff = true
	var staged_snap := staged_terrain.call("sculpt_delta_serialized") as Dictionary if staged_terrain != null else {}
	var applied_snap := applied_terrain.call("sculpt_delta_serialized") as Dictionary if applied_terrain != null else {}
	map.set_sculpt_snapshots(staged_snap, applied_snap)


func _build_hydrology_page() -> void:
	_section("Regional hydrology dependency refresh")
	_add_note("Terrain edits mark drainage → watersheds → soil → biome consequences stale. Flow topology is globally dependent, so the resolver computes receivers/accumulation with global boundary context but replaces hydrology only inside the selected spherical region.")
	_add_number_field("Region radius", _hydrology_radius_m / 1000.0, 0.1, 2000.0, 0.1, " km", func(value: float) -> void:
		_hydrology_radius_m = value * 1000.0
	)
	var row := HBoxContainer.new()
	_workspace.add_child(row)
	var use_map := Button.new()
	use_map.text = "Use last map point"
	use_map.pressed.connect(func() -> void:
		_hydrology_center = _last_map_direction
		_ps_workspace.mark_region_stale(_hydrology_center, _hydrology_radius_m,
			PackedStringArray(["drainage", "watersheds", "soil", "biomes"]), "Manual region selection")
		_refresh_current_category()
	)
	row.add_child(use_map)
	var run := Button.new()
	run.text = "Re-analyse Region"
	run.disabled = _hydrology_center.length_squared() < 0.5 or not Planet.ready_state
	run.pressed.connect(_run_regional_hydrology)
	row.add_child(run)
	var clear := Button.new()
	clear.text = "Clear stale markers"
	clear.pressed.connect(func() -> void:
		_ps_workspace.clear_stale_regions()
		_refresh_current_category()
	)
	row.add_child(clear)
	_add_note(_hydrology_status)
	if _ps_workspace.stale_regions.is_empty():
		_add_note("No dependency-stale regions are currently tracked.")
	else:
		for entry in _ps_workspace.stale_regions:
			_add_note("STALE %.1f km • %s • %s" % [
				float(entry.get("radius_m", 0.0)) / 1000.0,
				", ".join(entry.get("domains", []) as Array),
				String(entry.get("reason", "terrain changed")),
			])


func _run_regional_hydrology() -> void:
	if _hydrology_runner == null or _hydrology_center.length_squared() < 0.5:
		return
	var result := _hydrology_runner.reanalyse(_hydrology_center, _hydrology_radius_m)
	if not bool(result.get("ok", false)):
		_hydrology_status = String(result.get("reason", "Regional hydrology failed."))
	_refresh_current_category()


func _build_presets_page() -> void:
	_section("Reusable terrain recipes")
	var terrain := _session.active_terrain_profile() as Resource
	if terrain == null:
		_add_note("No active terrain profile.")
		return
	var selected := terrain.call("find_shader_slot", _phase43_selected_feature_id) as Resource
	if selected != null and GUIDED.is_guided_graph(selected.get(&"graph") as Resource):
		var save := Button.new()
		save.text = "Save Selected Feature as Custom Preset"
		save.pressed.connect(func() -> void:
			var graph: Resource = selected.get(&"graph") as Resource
			_ps_workspace.save_custom_preset(String(selected.get(&"display_name")), [{
				"name": String(selected.get(&"display_name")),
				"config": GUIDED.config_from_graph(graph),
				"strength": float(selected.get(&"strength")),
				"blend_mode": int(selected.get(&"blend_mode")),
			}])
			_refresh_current_category()
		)
		_workspace.add_child(save)
	else:
		_add_note("Select a guided local terrain feature in Terrain or the map to save it as a reusable recipe.")
	_section("Built-in")
	var builtins := GridContainer.new()
	builtins.columns = 3
	_workspace.add_child(builtins)
	for preset_id in FEATURE_PRESETS.IDS:
		var b := Button.new()
		b.text = "+ %s at map point" % FEATURE_PRESETS.label(preset_id)
		b.pressed.connect(_place_builtin_preset_at.bind(preset_id, _last_map_direction))
		builtins.add_child(b)
	_section("Custom")
	if _ps_workspace.custom_presets.is_empty():
		_add_note("No custom terrain presets saved yet.")
	for entry in _ps_workspace.custom_presets:
		var row := HBoxContainer.new()
		_workspace.add_child(row)
		var label := Label.new()
		label.text = String(entry.get("name", "Custom Terrain"))
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(label)
		var place := Button.new()
		place.text = "Place at map point"
		place.pressed.connect(_place_custom_preset_at.bind(String(entry.get("id", "")), _last_map_direction))
		row.add_child(place)
		var remove := Button.new()
		remove.text = "Delete"
		remove.pressed.connect(func() -> void:
			_ps_workspace.remove_custom_preset(String(entry.get("id", "")))
			_refresh_current_category()
		)
		row.add_child(remove)


func _build_seeds_page() -> void:
	_section("Seed explorer + parameter locks")
	var terrain := _session.active_terrain_profile() as Resource
	var generation: Resource = terrain.get(&"generation_profile") as Resource if terrain != null else null
	if generation == null:
		_add_note("No procedural generation profile is active.")
		return
	_add_note("Generate a deterministic candidate shelf, choose a world seed, and lock generation parameters you do not want the explorer to randomize. Applying a seed remains staged and correctly requests a full generator rebuild.")
	var seed_row := HBoxContainer.new()
	_workspace.add_child(seed_row)
	var generate := Button.new()
	generate.text = "Generate 8 Candidates"
	generate.pressed.connect(func() -> void:
		_seed_candidates = PackedInt64Array()
		var base_seed := int(generation.get(&"world_seed"))
		for i in 8:
			_seed_candidates.append(int(HashRNG.hash2(base_seed, i + 1)))
		_refresh_current_category()
	)
	seed_row.add_child(generate)
	var randomize := Button.new()
	randomize.text = "Randomize Unlocked Parameters"
	randomize.pressed.connect(_randomize_unlocked_generation.bind(generation))
	seed_row.add_child(randomize)
	var candidates := GridContainer.new()
	candidates.columns = 2
	_workspace.add_child(candidates)
	for seed in _seed_candidates:
		var card := Button.new()
		card.text = "Seed %d\nApply as staged world seed" % int(seed)
		card.custom_minimum_size = Vector2(300, 68)
		card.pressed.connect(func() -> void:
			_session.stage_set(generation, &"world_seed", int(seed),
				WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Choose seed candidate")
			_refresh_current_category()
		)
		candidates.add_child(card)
	_section("Locks")
	for property_name in [
		"ocean_fraction", "continent_scale", "max_uplift", "plate_count",
		"erosion_iterations", "stream_power_k", "base_precip", "orographic_gain",
		"detail_amplitude", "detail_base_frequency",
	]:
		var check := CheckBox.new()
		check.text = "Lock %s" % String(property_name).replace("_", " ")
		check.button_pressed = _ps_workspace.seed_locked(property_name)
		check.toggled.connect(func(value: bool) -> void:
			_ps_workspace.set_seed_lock(property_name, value)
		)
		_workspace.add_child(check)


func _randomize_unlocked_generation(generation: Resource) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = int(Time.get_unix_time_from_system()) ^ int(Time.get_ticks_usec())
	var ranges := {
		"ocean_fraction": [0.35, 0.78], "continent_scale": [0.7, 2.2],
		"max_uplift": [2500.0, 9000.0], "plate_count": [8, 42],
		"erosion_iterations": [8, 48], "stream_power_k": [1.0e-6, 8.0e-6],
		"base_precip": [450.0, 1800.0], "orographic_gain": [0.8, 4.5],
		"detail_amplitude": [80.0, 650.0], "detail_base_frequency": [0.00012, 0.0012],
	}
	_session.stage_action("Randomize unlocked generation parameters", func() -> void:
		for property_name in ranges.keys():
			if _ps_workspace.seed_locked(String(property_name)):
				continue
			var bounds: Array = ranges[property_name]
			if generation.get(StringName(property_name)) is int:
				generation.set(StringName(property_name), rng.randi_range(int(bounds[0]), int(bounds[1])))
			else:
				generation.set(StringName(property_name), rng.randf_range(float(bounds[0]), float(bounds[1])))
	, WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	_refresh_current_category()


func _build_diagnostics_page() -> void:
	_section("Authoring diagnostics / profiler")
	_add_note("Current apply scope: %s" % _scope_name(_session.apply_scope))
	_add_note("Dirty staged document: %s • Undo %s • Redo %s" % [
		"yes" if _session.dirty else "no",
		"available" if _session.can_undo() else "empty",
		"available" if _session.can_redo() else "empty",
	])
	var terrain := _session.active_terrain_profile() as Resource
	if terrain != null:
		_add_note("Sparse sculpt: runtime %d tiles • staged %d tiles" % [
			Deltas.edited_tile_count(), int(terrain.call("sculpt_edited_tile_count"))])
		_add_note("Guided terrain features: %d • biome paint layers: %d" % [
			_guided_feature_slots(terrain).size(),
			(terrain.get(&"biome_override_layers") as Array).size(),
		])
	_add_note("Dependency-stale regions: %d" % _ps_workspace.stale_regions.size())
	if not _ps_workspace.timings_ms.is_empty():
		_section("Recorded operation timings")
		for key in _ps_workspace.timings_ms.keys():
			_add_note("%s: %.2f ms" % [String(key), float(_ps_workspace.timings_ms[key])])
	_section("Runtime")
	_add_note("Planet ready: %s • coarse cells: %d • blank mode: %s" % [
		"yes" if Planet.ready_state else "no",
		Planet.grid.cell_count if Planet.grid != null else 0,
		"yes" if bool(Planet.get("blank_mode")) else "no",
	])
	var status := Button.new()
	status.text = "Refresh diagnostics"
	status.pressed.connect(_refresh_current_category)
	_workspace.add_child(status)


func _bind_workspace_to_active_body() -> void:
	var body := _session.active_body() as Resource if _session != null else null
	_ps_workspace.bind_body(String(body.get(&"body_id")) if body != null else "asterra")


func _build_workbench_dialogs() -> void:
	_stamp_dialog = FileDialog.new()
	_stamp_dialog.title = "Import Terrain Height Stamp"
	_stamp_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	_stamp_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_stamp_dialog.filters = PackedStringArray(["*.png,*.exr,*.hdr,*.jpg,*.jpeg,*.webp;Height images"])
	_stamp_dialog.file_selected.connect(_on_stamp_selected)
	add_child(_stamp_dialog)
	_export_dialog = FileDialog.new()
	_export_dialog.title = "Export Local Terrain Delta Stamp"
	_export_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	_export_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_export_dialog.filters = PackedStringArray(["*.exr;OpenEXR height field"])
	_export_dialog.current_file = "asterra_local_delta.exr"
	_export_dialog.file_selected.connect(_export_local_delta_stamp)
	add_child(_export_dialog)


func _on_stamp_selected(path: String) -> void:
	var image := Image.load_from_file(path)
	if image == null or image.is_empty():
		_set_status("Could not load height stamp: %s" % path)
		return
	_stamp_image = image
	_stamp_path = path
	_set_status("Loaded height stamp %dx%d. Arm Image Stamp and click terrain." % [image.get_width(), image.get_height()])
	if _category == WORKBENCH_CATEGORY and _wb_tab == WB_SCULPT:
		_refresh_current_category()


func _export_local_delta_stamp(path: String) -> void:
	var radius := _active_planet_radius()
	var resolution := 513
	var center := _last_map_direction.normalized()
	var reference := Vector3.UP if absf(center.dot(Vector3.UP)) < 0.92 else Vector3.RIGHT
	var right := reference.cross(center).normalized()
	var up := center.cross(right).normalized()
	var spacing := (_sculpt_radius_m * 2.0) / float(resolution - 1)
	var data := Deltas.sample_tangent_patch(center, right, up, resolution, spacing, radius)
	var image := Image.create(resolution, resolution, false, Image.FORMAT_RF)
	for y in resolution:
		for x in resolution:
			image.set_pixel(x, y, Color(data[y * resolution + x], 0, 0, 1))
	var err := image.save_exr(path, false)
	_set_status("Exported local sparse delta EXR: %s" % path if err == OK else "EXR export failed (%d)." % err)


func _commit_runtime_sculpt(action_name: String, direction: Vector3, radius_m: float) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("set_sculpt_delta_serialized"):
		return
	var serialized := Deltas.serialize()
	if terrain.call("sculpt_delta_matches", serialized):
		return
	_session.stage_action(action_name, func() -> void:
		terrain.call("set_sculpt_delta_serialized", serialized)
	, WorldAuthoringSession.ApplyScope.TILES)
	_ps_workspace.mark_region_stale(direction, maxf(radius_m, 1.0) * 1.5,
		PackedStringArray(["drainage", "watersheds", "soil", "biomes"]), action_name)


func _sync_runtime_deltas_from_staged() -> void:
	var terrain := _session.active_terrain_profile() as Resource if _session != null else null
	if terrain != null and terrain.has_method("sculpt_delta_serialized"):
		Deltas.deserialize(terrain.call("sculpt_delta_serialized") as Dictionary)


func _show_applied_sculpt_runtime() -> void:
	var terrain := _applied_active_terrain()
	if terrain != null and terrain.has_method("sculpt_delta_serialized"):
		Deltas.deserialize(terrain.call("sculpt_delta_serialized") as Dictionary)
		_set_status("Showing APPLIED sculpt state. Hold/release V or Restore Staged to return.")


func _applied_active_body() -> Resource:
	if _session == null or _session.applied_system == null or _session.staged_system == null:
		return null
	var active_id := String(_session.staged_system.get(&"active_body_id"))
	return _session.applied_system.call("find_body", active_id) as Resource


func _applied_active_terrain() -> Resource:
	var body := _applied_active_body()
	var profile := body.get(&"planet_profile") as Resource if body != null else null
	return profile.get(&"terrain") as Resource if profile != null else null


func _applied_active_water() -> Resource:
	var body := _applied_active_body()
	var profile := body.get(&"planet_profile") as Resource if body != null else null
	return profile.get(&"water") as Resource if profile != null else null


func _update_map_compare_snapshots() -> void:
	if _map_view == null:
		return
	var staged := _session.active_terrain_profile() as Resource
	var applied := _applied_active_terrain()
	_map_view.set_sculpt_snapshots(
		staged.call("sculpt_delta_serialized") as Dictionary if staged != null else {},
		applied.call("sculpt_delta_serialized") as Dictionary if applied != null else {})


func _focus_direction_3d(direction: Vector3) -> void:
	if direction.length_squared() < 0.5 or _player == null:
		return
	var d := direction.normalized()
	var radius := _active_planet_radius()
	var height := TerrainContactSampler.contact_height(d, TerrainContactSampler.coarse_height(d))
	var altitude := maxf(1500.0, _sculpt_radius_m * 8.0)
	var world_pos := Vec3D.from_v3(d).mul(radius + height + altitude)
	_player.set("world_pos", world_pos)
	if _player.has_signal("moved"):
		_player.emit_signal("moved", world_pos)
	if _camera != null:
		_camera.look_at(Frames.to_render(Vec3D.new()), d)
	_set_status("Focused 3D viewport at map location.")


func _move_selected_feature_to(direction: Vector3) -> void:
	var feature_id := _map_selected_feature_id if not _map_selected_feature_id.is_empty() else _phase43_selected_feature_id
	if feature_id.is_empty() or _ps_workspace.is_locked("feature", feature_id):
		_set_status("Select an unlocked guided feature first.")
		return
	var terrain := _session.active_terrain_profile() as Resource
	var slot := terrain.call("find_shader_slot", feature_id) as Resource if terrain != null else null
	var graph := slot.get(&"graph") as Resource if slot != null else null
	if graph == null or not GUIDED.is_guided_graph(graph):
		_set_status("Selected feature cannot be moved from the simple map.")
		return
	var latlon := CubeSphere.dir_to_latlon(direction.normalized())
	_session.stage_action("Move terrain feature from map", func() -> void:
		GUIDED.set_config_value(graph, "center_latitude_deg", rad_to_deg(latlon.x))
		GUIDED.set_config_value(graph, "center_longitude_deg", rad_to_deg(latlon.y))
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_set_status("Moved selected terrain feature to map location.")


func _paint_selected_biome_at(direction: Vector3) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	var layer := terrain.call("find_biome_layer", _selected_biome_layer_id) as Resource if terrain != null else null
	if layer == null:
		_set_status("Create/select a biome paint layer in Terrain first.")
		return
	_session.add_biome_stroke(_selected_biome_layer_id, direction,
		int(layer.get(&"active_biome_id")), float(layer.get(&"brush_radius_m")),
		float(layer.get(&"brush_hardness")), float(layer.get(&"brush_opacity")))
	_set_status("Painted %s from global map." % BIOME_NAMES[int(layer.get(&"active_biome_id"))])


func _place_builtin_preset_at(preset_id: String, direction: Vector3) -> void:
	var specs := FEATURE_PRESETS.specs(preset_id, _guided_feature_slots(_session.active_terrain_profile()).size() + 1)
	_place_feature_specs_at(specs, direction, FEATURE_PRESETS.label(preset_id))


func _place_custom_preset_at(preset_id: String, direction: Vector3) -> void:
	var entry := _ps_workspace.custom_preset(preset_id)
	if entry.is_empty():
		return
	_place_feature_specs_at(entry.get("specs", []) as Array, direction, String(entry.get("name", "Custom Terrain")))


func _place_feature_specs_at(raw_specs: Array, direction: Vector3, action_label: String) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	if terrain == null or direction.length_squared() < 0.5:
		return
	var latlon := CubeSphere.dir_to_latlon(direction.normalized())
	var created: Array[Resource] = []
	_session.stage_action("Place %s" % action_label, func() -> void:
		for spec_value in raw_specs:
			if not (spec_value is Dictionary): continue
			var spec := spec_value as Dictionary
			var config := (spec.get("config", {}) as Dictionary).duplicate(true)
			var area := String(config.get("area_kind", GUIDED.AREA_RADIAL))
			if area in [GUIDED.AREA_RADIAL, GUIDED.AREA_RING]:
				config["center_latitude_deg"] = rad_to_deg(latlon.x)
				config["center_longitude_deg"] = rad_to_deg(latlon.y)
			var slot := terrain.call("create_shader_slot", SLOT_MODEL.Domain.DISPLACEMENT,
				String(spec.get("name", action_label))) as Resource
			if slot == null: continue
			slot.set(&"enabled", true)
			slot.set(&"clipmap_level_mask", (1 << 15) - 1)
			slot.set(&"strength", float(spec.get("strength", 1.0)))
			slot.set(&"blend_mode", int(spec.get("blend_mode", SLOT_MODEL.BlendMode.ADD)))
			var graph := slot.get(&"graph") as Resource
			if graph == null or not GUIDED.rebuild(graph, config):
				terrain.call("remove_shader_slot", String(slot.get(&"slot_id")))
				continue
			created.append(slot)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	if not created.is_empty():
		_phase43_selected_feature_id = String(created[0].get(&"slot_id"))
		_map_selected_feature_id = _phase43_selected_feature_id
	_set_status("Placed %s at global map location." % action_label)


func _duplicate_feature(resource: Resource) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	if terrain == null: return
	_session.stage_action("Duplicate terrain feature", func() -> void:
		var copy := resource.duplicate(true) as Resource
		copy.set(&"display_name", "%s Copy" % String(resource.get(&"display_name")))
		copy.set(&"slot_id", SLOT_MODEL.make_slot_id(String(copy.get(&"display_name"))))
		terrain.get(&"displacement_slots").append(copy)
	, WorldAuthoringSession.ApplyScope.GRAPH)


func _duplicate_biome_layer(resource: Resource) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	if terrain == null: return
	_session.stage_action("Duplicate biome paint layer", func() -> void:
		var copy := resource.duplicate(true) as Resource
		copy.set(&"display_name", "%s Copy" % String(resource.get(&"display_name")))
		copy.set(&"layer_id", BIOME_LAYER_MODEL.make_layer_id(String(copy.get(&"display_name"))))
		terrain.get(&"biome_override_layers").append(copy)
	, WorldAuthoringSession.ApplyScope.TILES)


func _duplicate_water_feature(resource: Resource) -> void:
	var water := _session.active_water_profile() as Resource
	if water == null: return
	_session.stage_action("Duplicate water feature", func() -> void:
		var copy := resource.duplicate(true) as Resource
		copy.set(&"display_name", "%s Copy" % String(resource.get(&"display_name")))
		copy.set(&"feature_id", WATER_FEATURE_MODEL.make_feature_id(String(copy.get(&"display_name"))))
		water.get(&"features").append(copy)
	, WorldAuthoringSession.ApplyScope.TILES)


func _solo_outliner_item(kind: String, id: String) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	if terrain != null:
		for slot in _guided_feature_slots(terrain):
			_ps_workspace.set_hidden("feature", String(slot.get(&"slot_id")),
				not (kind == "feature" and String(slot.get(&"slot_id")) == id))
		for layer in terrain.get(&"biome_override_layers") as Array:
			if layer is Resource:
				_ps_workspace.set_hidden("biome", String(layer.get(&"layer_id")),
					not (kind == "biome" and String(layer.get(&"layer_id")) == id))
	var water := _session.active_water_profile() as Resource
	if water != null:
		for feature in water.get(&"features") as Array:
			if feature is Resource:
				_ps_workspace.set_hidden("water", String(feature.get(&"feature_id")),
					not (kind == "water" and String(feature.get(&"feature_id")) == id))


func _guided_feature_slots(terrain: Resource) -> Array[Resource]:
	var out: Array[Resource] = []
	if terrain == null:
		return out
	for value in terrain.get(&"displacement_slots") as Array:
		var slot := value as Resource
		var graph := slot.get(&"graph") as Resource if slot != null else null
		if graph != null and GUIDED.is_guided_graph(graph):
			out.append(slot)
	return out


func _active_planet_radius() -> float:
	var body := _session.active_body() as Resource if _session != null else null
	return maxf(float(body.get(&"radius_m")), 1.0) if body != null else \
		(maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1000000.0)


func _has_property(resource: Resource, property_name: StringName) -> bool:
	for property in resource.get_property_list():
		if StringName(property.get("name", "")) == property_name:
			return true
	return false


func _find_label_recursive(node: Node, text: String) -> Label:
	if node is Label and (node as Label).text == text:
		return node as Label
	for child in node.get_children():
		var found := _find_label_recursive(child, text)
		if found != null:
			return found
	return null


func _find_button_recursive(node: Node, text: String) -> Button:
	if node is Button and (node as Button).text == text:
		return node as Button
	for child in node.get_children():
		var found := _find_button_recursive(child, text)
		if found != null:
			return found
	return null
