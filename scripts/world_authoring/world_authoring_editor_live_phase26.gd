extends "res://scripts/world_authoring/world_authoring_editor_live_phase25.gd"
## Phase 26: keep live-source -> file-backed shader transitions transactional.
## Clearing the source before the inherited path edit is now its own Undo step,
## instead of mutating the staged profile outside WorldAuthoringSession history.


func _on_runtime_shader_path_submitted(path: String, target_id: String,
		material: ShaderMaterial, path_edit: LineEdit) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile != null and profile.has_method("has_runtime_shader_source") \
			and bool(profile.call("has_runtime_shader_source", target_id)):
		_session.stage_action("Release live shader source for path change", func() -> void:
			profile.call("clear_runtime_shader_source", target_id)
		, WorldAuthoringSession.ApplyScope.HOT)
		_live_shader_pending.erase(target_id)
		_live_shader_cache.erase(target_id)
	# Phase 25's override sees the source already released, so it performs only the
	# ordinary file-path transaction from Phase 18 and does not mutate outside Undo.
	super._on_runtime_shader_path_submitted(path, target_id, material, path_edit)
