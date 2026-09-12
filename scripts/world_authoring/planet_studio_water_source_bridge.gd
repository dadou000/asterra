extends Node
## Planet Studio bridge for the production sparse-hydrology point-source API.
##
## The editor UI owns only serialized source definitions and placement. WaterSystem
## remains authoritative for sparse allocation/solve, SparseHydroSurfaceCache owns
## reconstruction, and LocalWaterSurface/OceanSystem own rendering.

const PANEL_NAME := "SparseWaterSourceAuthoringPanel"
const LIVE_EDITOR_NAME := "PlanetStudioLive"
const LIVE_VIEW_MIN_X := 1082.0
const SYNC_INTERVAL_USEC := 200000

var _editor: Control
var _session: WorldAuthoringSession
var _workspace: VBoxContainer
var _selected_source_id := ""
var _placing_source_id := ""
var _runtime_signatures: Dictionary = {}
var _panel_dirty := true
var _last_sync_usec := 0


func _ready() -> void:
	process_priority = 205
	set_process(true)
	set_process_unhandled_input(true)


func _process(_delta: float) -> void:
	if not _planet_studio_running():
		return
	if _editor == null or not is_instance_valid(_editor):
		_bind_editor()
		return
	_workspace = _editor.get("_workspace") as VBoxContainer
	if _workspace == null:
		return
	var now := Time.get_ticks_usec()
	if now - _last_sync_usec >= SYNC_INTERVAL_USEC:
		_last_sync_usec = now
		_sync_sources_to_runtime()
	if String(_editor.get("_category")) == "WATER":
		_ensure_panel()


func _unhandled_input(event: InputEvent) -> void:
	if _placing_source_id.is_empty() or _editor == null or not is_instance_valid(_editor):
		return
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and key.keycode == KEY_ESCAPE:
			_cancel_placement()
			get_viewport().set_input_as_handled()
		return
	if not (event is InputEventMouseButton):
		return
	var mouse := event as InputEventMouseButton
	if not mouse.pressed:
		return
	if mouse.button_index == MOUSE_BUTTON_RIGHT:
		_cancel_placement()
		get_viewport().set_input_as_handled()
		return
	if mouse.button_index != MOUSE_BUTTON_LEFT or mouse.position.x <= LIVE_VIEW_MIN_X:
		return
	var hit_value: Variant = _editor.call("_screen_aim", mouse.position)
	if not (hit_value is Dictionary):
		_set_status("Water source placement could not query the live terrain.")
		return
	var hit := hit_value as Dictionary
	var direction := Vector3(hit.get("dir", Vector3.ZERO))
	if direction.length_squared() < 0.99:
		_set_status("Water source placement did not intersect detailed terrain.")
		return
	_place_source(direction.normalized())
	get_viewport().set_input_as_handled()


func _planet_studio_running() -> bool:
	return get_tree().has_meta("launch_mode") \
		and String(get_tree().get_meta("launch_mode")) == "planet_studio"


func _bind_editor() -> void:
	var candidate := get_tree().root.find_child(LIVE_EDITOR_NAME, true, false) as Control
	if candidate == null:
		return
	var session_value: Variant = candidate.get("_session")
	if not (session_value is WorldAuthoringSession):
		return
	_editor = candidate
	_session = session_value as WorldAuthoringSession
	_workspace = _editor.get("_workspace") as VBoxContainer
	_panel_dirty = true
	_sync_sources_to_runtime()


func _ensure_panel() -> void:
	if _workspace == null:
		return
	var existing := _workspace.get_node_or_null(PANEL_NAME)
	if existing != null and not _panel_dirty:
		return
	_panel_dirty = false
	if existing != null:
		_workspace.remove_child(existing)
		existing.queue_free()
	_build_panel()


func _build_panel() -> void:
	var water := _water_profile()
	if water == null:
		return
	var panel := PanelContainer.new()
	panel.name = PANEL_NAME
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_workspace.add_child(panel)
	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 7)
	panel.add_child(root)

	var title := Label.new()
	title.text = "SPARSE HYDROLOGY TEST SOURCES"
	title.add_theme_font_size_override("font_size", 17)
	root.add_child(title)
	var intro := Label.new()
	intro.text = "These are real volumetric inputs to the sparse shallow-water solver. Positive m³/s creates water; negative m³/s removes water from an already-active domain."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.modulate = Color(0.61, 0.72, 0.81)
	root.add_child(intro)

	var visible_toggle := CheckButton.new()
	visible_toggle.text = "Show simulated inland / flood water"
	visible_toggle.button_pressed = _preview_enabled()
	visible_toggle.toggled.connect(_set_preview_enabled)
	root.add_child(visible_toggle)

	var runtime_label := Label.new()
	runtime_label.text = _runtime_status()
	runtime_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	runtime_label.modulate = Color(0.55, 0.68, 0.77)
	root.add_child(runtime_label)

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 7)
	root.add_child(header)
	var add := Button.new()
	add.text = "+ Add Source"
	add.pressed.connect(_create_source)
	header.add_child(add)
	var resync := Button.new()
	resync.text = "Resync Runtime"
	resync.pressed.connect(func() -> void:
		_runtime_signatures.clear()
		_sync_sources_to_runtime()
		_set_status("Sparse water sources resynchronized.")
	)
	header.add_child(resync)

	var sources: Array = water.get(&"point_sources")
	if sources.is_empty():
		var empty := Label.new()
		empty.text = "No sources yet. Add one, then click Place on Planet."
		empty.modulate = Color(0.58, 0.67, 0.74)
		root.add_child(empty)
		return

	if water.call("find_point_source", _selected_source_id) == null:
		var first := sources[0] as Resource
		_selected_source_id = String(first.get(&"source_id")) if first != null else ""

	var list := ItemList.new()
	list.custom_minimum_size.y = minf(190.0, 46.0 + float(sources.size()) * 31.0)
	for source_value: Variant in sources:
		var source := source_value as Resource
		if source == null:
			continue
		var id := String(source.get(&"source_id"))
		var state := "ON" if bool(source.get(&"enabled")) else "OFF"
		list.add_item("%s   ·   %.2f m³/s   ·   %s" % [
			String(source.get(&"display_name")), float(source.get(&"rate_m3_s")), state])
		var index := list.item_count - 1
		list.set_item_metadata(index, id)
		if id == _selected_source_id:
			list.select(index)
	list.item_selected.connect(func(index: int) -> void:
		_selected_source_id = String(list.get_item_metadata(index))
		_panel_dirty = true
	)
	root.add_child(list)

	var source := water.call("find_point_source", _selected_source_id) as Resource
	if source != null:
		root.add_child(HSeparator.new())
		_build_source_controls(root, source)


func _build_source_controls(root: VBoxContainer, source: Resource) -> void:
	var source_id := String(source.get(&"source_id"))
	var name_row := HBoxContainer.new()
	name_row.add_theme_constant_override("separation", 8)
	root.add_child(name_row)
	var name_label := Label.new()
	name_label.text = "Name"
	name_label.custom_minimum_size.x = 150.0
	name_row.add_child(name_label)
	var name_edit := LineEdit.new()
	name_edit.text = String(source.get(&"display_name"))
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_edit.text_submitted.connect(func(value: String) -> void:
		var clean := value.strip_edges()
		_stage("Rename water source", func() -> void:
			source.set(&"display_name", clean if not clean.is_empty() else "Water Source")
		)
	)
	name_row.add_child(name_edit)

	var enabled := CheckButton.new()
	enabled.text = "Source enabled"
	enabled.button_pressed = bool(source.get(&"enabled"))
	enabled.toggled.connect(func(value: bool) -> void:
		_stage("Toggle water source", func() -> void: source.set(&"enabled", value))
	)
	root.add_child(enabled)

	var flow_row := HBoxContainer.new()
	flow_row.add_theme_constant_override("separation", 8)
	root.add_child(flow_row)
	var flow_label := Label.new()
	flow_label.text = "Volumetric flow"
	flow_label.custom_minimum_size.x = 150.0
	flow_row.add_child(flow_label)
	var flow := SpinBox.new()
	flow.min_value = -1000000.0
	flow.max_value = 1000000.0
	flow.step = 0.1
	flow.suffix = " m³/s"
	flow.value = float(source.get(&"rate_m3_s"))
	flow.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	flow.value_changed.connect(func(value: float) -> void:
		_stage("Change water source flow", func() -> void: source.set(&"rate_m3_s", value), false)
	)
	flow_row.add_child(flow)

	var direction := Vector3(source.get(&"direction")).normalized()
	var lat := rad_to_deg(asin(clampf(direction.y, -1.0, 1.0)))
	var lon := rad_to_deg(atan2(direction.x, direction.z))
	var location := Label.new()
	location.text = "%.5f° lat, %.5f° lon   ·   hydro tile: %s" % [
		lat, lon, "auto" if int(source.get(&"tile_level")) < 0 else str(int(source.get(&"tile_level")))]
	location.modulate = Color(0.60, 0.72, 0.81)
	root.add_child(location)

	var actions := HBoxContainer.new()
	actions.add_theme_constant_override("separation", 7)
	root.add_child(actions)
	var place := Button.new()
	place.text = "CLICK TERRAIN TO PLACE" if _placing_source_id == source_id else "Place on Planet"
	place.pressed.connect(func() -> void:
		_placing_source_id = "" if _placing_source_id == source_id else source_id
		_set_status("WATER SOURCE ARMED — click Asterra terrain • RMB/Esc cancel" \
			if not _placing_source_id.is_empty() else "Water source placement stopped.")
		_panel_dirty = true
	)
	actions.add_child(place)
	var focus := Button.new()
	focus.text = "Center Water Cache"
	focus.pressed.connect(func() -> void: _focus_cache(Vector3(source.get(&"direction"))))
	actions.add_child(focus)
	var remove := Button.new()
	remove.text = "Delete"
	remove.pressed.connect(func() -> void: _delete_source(source_id))
	actions.add_child(remove)

	var note := Label.new()
	note.text = "New sources start disabled until placed. Injection velocity stays at zero for this first test, so the terrain and gravity determine the initial flow direction."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.52, 0.63, 0.71)
	root.add_child(note)


func _create_source() -> void:
	var water := _water_profile()
	if water == null or _session == null:
		return
	var created_box: Array[Resource] = []
	var count := (water.get(&"point_sources") as Array).size() + 1
	_session.stage_action("Create water source", func() -> void:
		var source := water.call("create_point_source", "Source %d" % count) as Resource
		if source != null:
			created_box.append(source)
	, WorldAuthoringSession.ApplyScope.TILES)
	if created_box.is_empty():
		return
	_selected_source_id = String(created_box[0].get(&"source_id"))
	_placing_source_id = _selected_source_id
	_set_preview_enabled(true)
	_panel_dirty = true
	_set_status("New source created. Click detailed Asterra terrain to place and enable it.")


func _delete_source(source_id: String) -> void:
	var water := _water_profile()
	if water == null or _session == null:
		return
	_session.stage_action("Delete water source", func() -> void:
		water.call("remove_point_source", source_id)
	, WorldAuthoringSession.ApplyScope.TILES)
	if _placing_source_id == source_id:
		_placing_source_id = ""
	if _selected_source_id == source_id:
		_selected_source_id = ""
	_panel_dirty = true
	_sync_sources_to_runtime()


func _stage(action_name: String, action: Callable, rebuild_panel: bool = true) -> void:
	if _session == null or not action.is_valid():
		return
	_session.stage_action(action_name, action, WorldAuthoringSession.ApplyScope.TILES)
	if rebuild_panel:
		_panel_dirty = true
	_sync_sources_to_runtime()


func _place_source(direction: Vector3) -> void:
	var water := _water_profile()
	if water == null:
		return
	var source := water.call("find_point_source", _placing_source_id) as Resource
	if source == null:
		_cancel_placement()
		return
	_session.stage_action("Place water source", func() -> void:
		source.set(&"direction", direction)
		source.set(&"enabled", true)
	, WorldAuthoringSession.ApplyScope.TILES)
	var source_name := String(source.get(&"display_name"))
	var flow := float(source.get(&"rate_m3_s"))
	_placing_source_id = ""
	_selected_source_id = String(source.get(&"source_id"))
	_set_preview_enabled(true)
	_runtime_signatures.erase(_selected_source_id)
	_sync_sources_to_runtime()
	_focus_cache(direction)
	_panel_dirty = true
	_set_status("Placed %s at %.2f m³/s. The sparse solver is now feeding the visible local water surface." % [source_name, flow])


func _cancel_placement() -> void:
	_placing_source_id = ""
	_panel_dirty = true
	_set_status("Water source placement cancelled.")


func _water_profile() -> Resource:
	return _session.active_water_profile() as Resource if _session != null else null


func _runtime_target_available() -> bool:
	if _session == null or not Planet.ready_state or Planet.cfg == null:
		return false
	var body := _session.active_body() as Resource
	if body == null:
		return false
	var runtime_radius := maxf(float(Planet.cfg.planet_radius), 1.0)
	return absf(float(body.get(&"radius_m")) - runtime_radius) <= maxf(2.0, runtime_radius * 1.0e-5)


func _signature(source: Resource) -> String:
	var d := Vector3(source.get(&"direction")).normalized()
	var v := Vector3(source.get(&"injection_velocity_world"))
	return "%.10f|%.10f|%.10f|%.8f|%.8f|%.8f|%.8f|%d|%d" % [
		d.x, d.y, d.z, float(source.get(&"rate_m3_s")), v.x, v.y, v.z,
		int(source.get(&"tile_level")), 1 if bool(source.get(&"enabled")) else 0]


func _sync_sources_to_runtime() -> void:
	if not _runtime_target_available():
		return
	var water := _water_profile()
	if water == null:
		return
	var seen: Dictionary = {}
	for source_value: Variant in (water.get(&"point_sources") as Array):
		var source := source_value as Resource
		if source == null:
			continue
		source.call("ensure_valid")
		var source_id := String(source.get(&"source_id"))
		var signature := _signature(source)
		seen[source_id] = true
		if String(_runtime_signatures.get(source_id, "")) == signature:
			continue
		var err := WaterSystem.upsert_point_water_source(
			source_id,
			Vector3(source.get(&"direction")),
			float(source.get(&"rate_m3_s")),
			Vector3(source.get(&"injection_velocity_world")),
			int(source.get(&"tile_level")),
			bool(source.get(&"enabled")))
		if err == OK:
			_runtime_signatures[source_id] = signature
		else:
			_set_status("Sparse source sync failed for %s (error %d)." % [source_id, int(err)])
	var stale: Array[String] = []
	for id_value: Variant in _runtime_signatures.keys():
		var id := String(id_value)
		if not seen.has(id):
			stale.append(id)
	for id: String in stale:
		WaterSystem.remove_point_water_source(id)
		_runtime_signatures.erase(id)


func _set_preview_enabled(enabled: bool) -> void:
	WaterSystem.set_dynamic_surface_render_enabled(enabled)
	LocalWaterSurface.set_render_enabled(enabled)
	if enabled and SparseHydroSurfaceCache.available():
		SparseHydroSurfaceCache.request_update()
	_panel_dirty = true


func _preview_enabled() -> bool:
	return WaterSystem.dynamic_surface_render_enabled() and LocalWaterSurface.is_render_enabled()


func _focus_cache(direction: Vector3) -> void:
	if direction.length_squared() < 0.5:
		return
	WaterSystem.set_dynamic_surface_anchor_direction(direction.normalized())
	WaterSystem.set_dynamic_surface_center_plane(Vector2.ZERO)
	if SparseHydroSurfaceCache.available():
		SparseHydroSurfaceCache.request_update()


func _runtime_status() -> String:
	return "Runtime: %s   ·   source definitions: %d   ·   reconstruction: %s   ·   local renderer: %s" % [
		WaterSystem.sparse_runtime_state(), WaterSystem.point_water_source_count(),
		"READY" if SparseHydroSurfaceCache.available() else "WAITING",
		"READY" if LocalWaterSurface.renderer_available() else "WAITING"]


func _set_status(message: String) -> void:
	if _editor == null or not is_instance_valid(_editor):
		return
	_editor.call("_set_status", message)
	var label := _editor.get("_live_status_label") as Label
	if label != null:
		label.text = message
