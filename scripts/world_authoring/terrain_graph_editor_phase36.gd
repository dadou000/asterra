extends "res://scripts/world_authoring/terrain_graph_editor_phase35.gd"
## Phase 36: explicit staged-preview versus applied-terrain transaction controls.
##
## The WorldAuthoringSession already owns separate staged/applied system Resources.
## This editor stores only a transient preview-choice flag on that session. The
## authoritative terrain renderer reads the flag and resolves one of those existing
## Resources; no ShaderMaterial, bytecode program or duplicate terrain state lives
## in this UI.

const TERRAIN_PREVIEW_META := &"terrain_graph_preview_enabled"

var _preview_button: Button
var _original_button: Button
var _transaction_status: Label
var _session_changed_callable: Callable


func _build_ui() -> void:
	super._build_ui()
	if _graph == null or int(_graph.get(&"domain")) != GRAPH_SCRIPT.Domain.DISPLACEMENT \
			or _session == null:
		return
	if not _session.has_meta(TERRAIN_PREVIEW_META):
		# Entering the terrain editor starts in non-destructive preview, preserving the
		# immediate feedback users already had. Applied/original remains one click away.
		_session.set_meta(TERRAIN_PREVIEW_META, true)
	_build_transaction_bar()
	_session_changed_callable = Callable(self, "_on_transaction_session_changed")
	if _session.has_signal("changed") \
			and not _session.is_connected("changed", _session_changed_callable):
		_session.connect("changed", _session_changed_callable)
	_refresh_transaction_status()


func _exit_tree() -> void:
	if _session != null and _session_changed_callable.is_valid() \
			and _session.has_signal("changed") \
			and _session.is_connected("changed", _session_changed_callable):
		_session.disconnect("changed", _session_changed_callable)


func _build_transaction_bar() -> void:
	var bar := HBoxContainer.new()
	bar.name = "TerrainTransactionBar"
	bar.add_theme_constant_override("separation", 7)
	add_child(bar)
	move_child(bar, mini(1, get_child_count() - 1))

	var label := Label.new()
	label.text = "World view"
	label.custom_minimum_size.x = 105.0
	bar.add_child(label)

	_preview_button = Button.new()
	_preview_button.text = "Preview Edits"
	_preview_button.tooltip_text = "Render the staged terrain safely without committing it"
	_preview_button.pressed.connect(_show_staged_preview)
	bar.add_child(_preview_button)

	_original_button = Button.new()
	_original_button.text = "Hold Original"
	_original_button.tooltip_text = "Render the last applied terrain while keeping staged edits intact"
	_original_button.pressed.connect(_show_applied_original)
	bar.add_child(_original_button)

	var separator := VSeparator.new()
	bar.add_child(separator)

	var apply := Button.new()
	apply.text = "Apply to World"
	apply.tooltip_text = "Commit the current staged Planet Studio system as the new applied snapshot"
	apply.pressed.connect(_apply_staged_to_world)
	bar.add_child(apply)

	var discard := Button.new()
	discard.text = "Discard Staged Edits"
	discard.tooltip_text = "Restore the staged document from the last applied snapshot"
	discard.pressed.connect(_discard_staged_edits)
	bar.add_child(discard)

	_transaction_status = Label.new()
	_transaction_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_transaction_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	bar.add_child(_transaction_status)


func _show_staged_preview() -> void:
	if _session == null:
		return
	_session.set_meta(TERRAIN_PREVIEW_META, true)
	_refresh_transaction_status()


func _show_applied_original() -> void:
	if _session == null:
		return
	_session.set_meta(TERRAIN_PREVIEW_META, false)
	_refresh_transaction_status()


func _apply_staged_to_world() -> void:
	if _session == null:
		return
	# Session.apply() duplicates the entire staged system before emitting `applied`.
	# Keep preview selected: staged and applied are identical immediately after the
	# commit, and the next edit again becomes an explicit non-destructive preview.
	_session.call("apply")
	_session.set_meta(TERRAIN_PREVIEW_META, true)
	_refresh_transaction_status()


func _discard_staged_edits() -> void:
	if _session == null:
		return
	_session.call("revert")
	_session.set_meta(TERRAIN_PREVIEW_META, false)
	# Revert replaces staged_system with a deep duplicate, so this editor's graph
	# Resource reference is intentionally obsolete. Rebuild from the new snapshot.
	_request_rebuild()


func _set_node_parameter(node_id: String, key: String, value: Variant) -> void:
	super._set_node_parameter(node_id, key, value)
	_refresh_transaction_status()


func _on_transaction_session_changed(_dirty: bool, _scope: int) -> void:
	_refresh_transaction_status()


func _refresh_transaction_status() -> void:
	if _session == null or _transaction_status == null:
		return
	var preview: bool = bool(_session.get_meta(TERRAIN_PREVIEW_META, true))
	var dirty: bool = bool(_session.get("dirty"))
	if _preview_button != null:
		_preview_button.disabled = preview
	if _original_button != null:
		_original_button.disabled = not preview

	if not dirty:
		_transaction_status.text = "APPLIED · staged and world terrain match"
		_transaction_status.modulate = Color(0.58, 0.76, 0.64)
	elif preview:
		_transaction_status.text = "PREVIEW · staged edits visible · world snapshot unchanged"
		_transaction_status.modulate = Color(0.78, 0.70, 0.42)
	else:
		_transaction_status.text = "ORIGINAL · last applied terrain visible · staged edits retained"
		_transaction_status.modulate = Color(0.54, 0.70, 0.82)
