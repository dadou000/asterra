class_name WorldAuthoringLiveEditorPhase2
extends "res://scripts/world_authoring/world_authoring_editor_live.gd"
## Transactional live-world terrain authoring.
##
## The base live editor performs immediate Deltas sculpting for zero-latency visual
## feedback. This layer turns those runtime edits into proper Planet Studio state:
## one mouse stroke becomes one undo entry, sparse data round-trips through presets,
## each celestial body owns its own sculpt layer, and Undo/Redo/Revert immediately
## republish the matching Deltas state to rendering and terrain contact.

var _sculpt_transaction_active: bool = false
var _sculpt_transaction_body_id: String = ""


func _ready() -> void:
	super._ready()
	if _world_host == null:
		return
	_adopt_runtime_sculpt_as_baseline()
	_refresh_toolbar()


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		return
	_section("Sculpt layer persistence")
	var tile_count: int = int(terrain.call("sculpt_edited_tile_count")) if terrain.has_method("sculpt_edited_tile_count") else 0
	var blob_bytes: int = 0
	if terrain.has_method("sculpt_delta_serialized"):
		var data_value: Variant = terrain.call("sculpt_delta_serialized")
		if data_value is Dictionary:
			var blob_value: Variant = (data_value as Dictionary).get("tiles", PackedByteArray())
			if blob_value is PackedByteArray:
				blob_bytes = (blob_value as PackedByteArray).size()
	var persistence_row := HBoxContainer.new()
	persistence_row.add_theme_constant_override("separation", 7)
	_workspace.add_child(persistence_row)
	var status := Label.new()
	status.text = "%d sparse tile(s) • %.2f MiB packed" % [tile_count, float(blob_bytes) / (1024.0 * 1024.0)]
	status.custom_minimum_size.x = 300.0
	status.modulate = Color(0.64, 0.76, 0.86)
	persistence_row.add_child(status)
	var clear_button := Button.new()
	clear_button.text = "CLEAR SCULPT LAYER"
	clear_button.disabled = tile_count <= 0 and Deltas.is_empty()
	clear_button.pressed.connect(_clear_sculpt_layer)
	persistence_row.add_child(clear_button)
	_add_note("A complete drag is committed as one Planet Studio history action. Sculpt deltas are saved with presets and swapped when selecting another celestial body; the procedural generator itself remains untouched.")


func _unhandled_input(event: InputEvent) -> void:
	var should_finish_transaction := false
	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			if mouse_button.pressed and _is_sculpt_placement() and _is_live_viewport_point(mouse_button.position):
				_begin_sculpt_transaction()
			elif not mouse_button.pressed and _sculpt_transaction_active:
				should_finish_transaction = true
		elif mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed \
				and _sculpt_transaction_active:
			should_finish_transaction = true
	elif event is InputEventKey:
		var key_event := event as InputEventKey
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_ESCAPE \
				and _sculpt_transaction_active:
			should_finish_transaction = true

	super._unhandled_input(event)

	if should_finish_transaction:
		_commit_sculpt_transaction()


func _is_sculpt_placement() -> bool:
	return _placement_mode == PlacementMode.SCULPT_RAISE \
		or _placement_mode == PlacementMode.SCULPT_LOWER


func _begin_sculpt_transaction() -> void:
	if _sculpt_transaction_active:
		return
	_sculpt_transaction_active = true
	_sculpt_transaction_body_id = _active_body_id()


func _commit_sculpt_transaction() -> void:
	if not _sculpt_transaction_active:
		return
	_sculpt_transaction_active = false
	var started_body_id := _sculpt_transaction_body_id
	_sculpt_transaction_body_id = ""
	if started_body_id.is_empty() or started_body_id != _active_body_id():
		_sync_runtime_sculpt_from_profile()
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("set_sculpt_delta_serialized"):
		return
	var snapshot: Dictionary = Deltas.serialize()
	if terrain.has_method("sculpt_delta_matches") and bool(terrain.call("sculpt_delta_matches", snapshot)):
		return
	_session.stage_action("Sculpt terrain", func() -> void:
		terrain.call("set_sculpt_delta_serialized", snapshot)
	, SESSION_SCRIPT.ApplyScope.TILES)
	_refresh_toolbar()


func _clear_sculpt_layer() -> void:
	_commit_sculpt_transaction()
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("clear_sculpt_deltas"):
		return
	var had_profile_data: bool = bool(terrain.call("has_sculpt_deltas")) if terrain.has_method("has_sculpt_deltas") else false
	if not had_profile_data and Deltas.is_empty():
		return
	_session.stage_action("Clear terrain sculpt", func() -> void:
		terrain.call("clear_sculpt_deltas")
	, SESSION_SCRIPT.ApplyScope.TILES)
	Deltas.clear()
	_last_sculpt_dir = Vector3.ZERO
	_last_hit.clear()
	_update_preview()
	_refresh_current_category()
	_set_status("Cleared the active body's sparse sculpt layer. Undo restores it immediately.")


func _on_undo_pressed() -> void:
	_commit_sculpt_transaction()
	super._on_undo_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_redo_pressed() -> void:
	_commit_sculpt_transaction()
	super._on_redo_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_apply_pressed() -> void:
	_commit_sculpt_transaction()
	super._on_apply_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_revert_pressed() -> void:
	_sculpt_transaction_active = false
	_sculpt_transaction_body_id = ""
	super._on_revert_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_body_selected(index: int) -> void:
	_commit_sculpt_transaction()
	super._on_body_selected(index)
	_sync_runtime_sculpt_from_profile()
	_last_hit.clear()
	_last_sculpt_dir = Vector3.ZERO
	_update_preview()


func _on_preset_selected(path: String) -> void:
	_commit_sculpt_transaction()
	super._on_preset_selected(path)
	_sync_runtime_sculpt_from_profile()
	_last_hit.clear()
	_last_sculpt_dir = Vector3.ZERO
	_update_preview()


func _active_body_id() -> String:
	if _session == null or _session.staged_system == null:
		return ""
	return String(_session.staged_system.get(&"active_body_id"))


func _adopt_runtime_sculpt_as_baseline() -> void:
	if _session == null:
		return
	var snapshot: Dictionary = Deltas.serialize()
	var staged_terrain: Resource = _session.active_terrain_profile() as Resource
	if staged_terrain != null and staged_terrain.has_method("set_sculpt_delta_serialized"):
		staged_terrain.call("set_sculpt_delta_serialized", snapshot)
	var applied_terrain: Resource = _active_terrain_for_system(_session.applied_system)
	if applied_terrain != null and applied_terrain.has_method("set_sculpt_delta_serialized"):
		applied_terrain.call("set_sculpt_delta_serialized", snapshot)


func _active_terrain_for_system(system: Resource) -> Resource:
	if system == null or not system.has_method("active_body"):
		return null
	var body: Resource = system.call("active_body") as Resource
	if body == null:
		return null
	var profile: Resource = body.get(&"planet_profile") as Resource
	if profile == null:
		return null
	return profile.get(&"terrain") as Resource


func _sync_runtime_sculpt_from_profile() -> void:
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("sculpt_delta_serialized"):
		Deltas.clear()
		return
	var data_value: Variant = terrain.call("sculpt_delta_serialized")
	if not (data_value is Dictionary):
		Deltas.clear()
		return
	Deltas.deserialize(data_value as Dictionary)
	_last_hit.clear()
	_last_sculpt_dir = Vector3.ZERO
	_update_preview()
