extends "res://scripts/world_authoring/world_authoring_editor_live_phase20.gd"
## Phase 21: runtime UI hardening.
##
## - Godot's `%` formatter does not support C's `%g`; scientific star fields now
##   use Godot's native float string conversion instead.
## - Category rebuilds are deferred one idle turn. Replacing Controls while their
##   mouse event is still being dispatched can leave the old Control detached and
##   make Control.get_local_mouse_position() observe a null viewport.

var _category_refresh_pending: bool = false


func _add_scientific_text_field(label_text: String, value: float, suffix: String,
		callback: Callable) -> void:
	_add_text_field(label_text, _scientific_value_text(value, suffix), func(text: String) -> void:
		var clean: String = text.strip_edges()
		if not suffix.is_empty() and clean.ends_with(suffix):
			clean = clean.left(clean.length() - suffix.length()).strip_edges()
		if not clean.is_valid_float():
			_set_status("%s requires a numeric value." % label_text)
			return
		callback.call(clean.to_float())
	)


func _scientific_value_text(value: float, suffix: String = "") -> String:
	# `str(float)` is locale-independent GDScript numeric text and automatically
	# uses exponent notation where appropriate. Avoid `%g`, which Godot rejects.
	return str(value) + suffix


func _refresh_current_category() -> void:
	if not is_inside_tree() or _workspace == null:
		return
	if _category_refresh_pending:
		return
	_category_refresh_pending = true
	call_deferred("_flush_category_refresh")


func _flush_category_refresh() -> void:
	_category_refresh_pending = false
	if not is_inside_tree() or get_viewport() == null or _workspace == null:
		return
	super._refresh_current_category()


func _unhandled_input(event: InputEvent) -> void:
	# A scene transition can detach this Control before a queued input event drains.
	if not is_inside_tree() or get_viewport() == null:
		return
	super._unhandled_input(event)
