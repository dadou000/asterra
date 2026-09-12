extends "res://scripts/world_authoring/world_authoring_editor_live_complete.gd"
## Final pre-0.1.0 Planet Studio entry point.
## Keeps the complete workbench composed over phase46 while adapting the current
## WaterAuthoringProfile schema (`authored_features`).

const MAP_VIEW_PRE01 := preload("res://scripts/world_authoring/planet_studio_map_view_schema.gd")


func _build_map_page() -> void:
	_section("Global authoring map")
	var controls := HBoxContainer.new()
	controls.add_theme_constant_override("separation", 6)
	_workspace.add_child(controls)

	var layer := OptionButton.new()
	for label: String in PlanetMap.LAYER_NAMES:
		layer.add_item(label)
	layer.select(int(_ps_workspace.ui_value("map_layer", PlanetMap.Layer.ELEVATION)))
	layer.item_selected.connect(func(index: int) -> void:
		_ps_workspace.set_ui_value("map_layer", index)
		if _map_view != null:
			_map_view.set_layer(index)
	)
	controls.add_child(layer)

	var action := OptionButton.new()
	for item: Array in [
		["Focus 3D here", "focus"],
		["Move selected feature", "move_feature"],
		["Paint selected biome", "paint_biome"],
		["Place terrain preset", "place_preset"],
		["Add bookmark", "bookmark"],
		["Set hydro region centre", "hydro"],
	]:
		action.add_item(String(item[0]))
		action.set_item_metadata(action.item_count - 1, String(item[1]))
	action.item_selected.connect(func(index: int) -> void:
		_map_action = String(action.get_item_metadata(index))
	)
	controls.add_child(action)

	var preset := OptionButton.new()
	for preset_id: String in FEATURE_PRESETS.IDS:
		preset.add_item(FEATURE_PRESETS.label(preset_id))
		preset.set_item_metadata(preset.item_count - 1, preset_id)
	preset.item_selected.connect(func(index: int) -> void:
		_map_preset_id = String(preset.get_item_metadata(index))
	)
	controls.add_child(preset)

	var overlays := HBoxContainer.new()
	_workspace.add_child(overlays)
	for item: Array in [
		["Features", "features"], ["Biomes", "biomes"], ["Water", "water"],
		["Bookmarks", "bookmarks"], ["Stale regions", "stale"], ["Sculpt diff", "diff"],
	]:
		var check := CheckBox.new()
		check.text = String(item[0])
		check.button_pressed = String(item[1]) != "diff"
		check.toggled.connect(_set_map_overlay.bind(String(item[1])))
		overlays.add_child(check)

	_map_view = MAP_VIEW_PRE01.new() as PlanetStudioMapView
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
	_add_note("Left click uses the selected map action. Clicking a guided feature selects it. Environmental layers reuse PlanetMap; staged authoring overlays reuse WorldAuthoringSession.")


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

	for group: Dictionary in _ps_workspace.groups:
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
		for slot: Resource in _guided_feature_slots(terrain):
			_build_outliner_resource_row("feature", slot, String(slot.get(&"slot_id")),
				String(slot.get(&"display_name")), Callable(self, "_duplicate_feature"))
		_section("Sculpt Layer")
		var sculpt_label := Label.new()
		sculpt_label.text = "Deltas — %d edited tiles • persistent sparse cube-sphere layer" % int(terrain.call("sculpt_edited_tile_count"))
		_workspace.add_child(sculpt_label)
		_section("Biome Paint")
		for layer_value: Variant in terrain.get(&"biome_override_layers") as Array:
			var biome_layer := layer_value as Resource
			if biome_layer != null:
				_build_outliner_resource_row("biome", biome_layer, String(biome_layer.get(&"layer_id")),
					String(biome_layer.get(&"display_name")), Callable(self, "_duplicate_biome_layer"))

	var water: Resource = _session.active_water_profile() as Resource
	if water != null:
		_section("Water")
		for feature_value: Variant in water.get(&"authored_features") as Array:
			var feature := feature_value as Resource
			if feature != null:
				_build_outliner_resource_row("water", feature, String(feature.get(&"feature_id")),
					String(feature.get(&"display_name")), Callable(self, "_duplicate_water_feature"))

	_section("Regions / Bookmarks")
	for bookmark: Dictionary in _ps_workspace.bookmarks:
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


func _build_compare_page() -> void:
	_section("Staged ↔ Applied comparison")
	var staged_terrain := _session.active_terrain_profile() as Resource
	var applied_terrain := _applied_active_terrain()
	var staged_tiles := int(staged_terrain.call("sculpt_edited_tile_count")) if staged_terrain != null else 0
	var applied_tiles := int(applied_terrain.call("sculpt_edited_tile_count")) if applied_terrain != null else 0
	_add_note("Hold V in the 3D viewport to temporarily show the applied sculpt-delta state; release V to restore staged sculpting.")
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
		(staged_water.get(&"authored_features") as Array).size() if staged_water != null else 0,
		(applied_water.get(&"authored_features") as Array).size() if applied_water != null else 0,
	])
	var show_applied := Button.new()
	show_applied.text = "Show Applied Sculpt Now"
	show_applied.pressed.connect(_show_applied_sculpt_runtime)
	_workspace.add_child(show_applied)
	var restore := Button.new()
	restore.text = "Restore Staged Sculpt"
	restore.pressed.connect(_sync_runtime_deltas_from_staged)
	_workspace.add_child(restore)
	var map := MAP_VIEW_PRE01.new() as PlanetStudioMapView
	_workspace.add_child(map)
	map.bind(_session, _ps_workspace)
	map.show_sculpt_diff = true
	var staged_snap := staged_terrain.call("sculpt_delta_serialized") as Dictionary if staged_terrain != null else {}
	var applied_snap := applied_terrain.call("sculpt_delta_serialized") as Dictionary if applied_terrain != null else {}
	map.set_sculpt_snapshots(staged_snap, applied_snap)


func _duplicate_water_feature(resource: Resource) -> void:
	var water := _session.active_water_profile() as Resource
	if water == null:
		return
	_session.stage_action("Duplicate water feature", func() -> void:
		var copy := resource.duplicate(true) as Resource
		copy.set(&"display_name", "%s Copy" % String(resource.get(&"display_name")))
		copy.set(&"feature_id", WATER_FEATURE_MODEL.make_feature_id(String(copy.get(&"display_name"))))
		var authored: Array = water.get(&"authored_features") as Array
		authored.append(copy)
		water.set(&"authored_features", authored)
	, WorldAuthoringSession.ApplyScope.TILES)


func _solo_outliner_item(kind: String, id: String) -> void:
	var terrain := _session.active_terrain_profile() as Resource
	if terrain != null:
		for slot: Resource in _guided_feature_slots(terrain):
			_ps_workspace.set_hidden("feature", String(slot.get(&"slot_id")),
				not (kind == "feature" and String(slot.get(&"slot_id")) == id))
		for layer_value: Variant in terrain.get(&"biome_override_layers") as Array:
			var layer := layer_value as Resource
			if layer != null:
				_ps_workspace.set_hidden("biome", String(layer.get(&"layer_id")),
					not (kind == "biome" and String(layer.get(&"layer_id")) == id))
	var water := _session.active_water_profile() as Resource
	if water != null:
		for feature_value: Variant in water.get(&"authored_features") as Array:
			var feature := feature_value as Resource
			if feature != null:
				_ps_workspace.set_hidden("water", String(feature.get(&"feature_id")),
					not (kind == "water" and String(feature.get(&"feature_id")) == id))
