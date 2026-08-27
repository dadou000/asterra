extends "res://scripts/terrain/gpu_rendered_terrain_contact_query.gd"
## Stable source invalidation for rendered-contact queries.
##
## The base query originally invalidated every in-flight result whenever the
## clipmap's snapped centre, visible LOD window or deformation ping-pong index
## changed. Those are presentation/window changes, not necessarily changes to the
## world-space surface at the requested direction. With an asynchronous readback
## this could starve a contact forever while the camera/clipmap was moving.
##
## Only changes that can actually invalidate the sampled height field advance the
## source serial here: a cache reanchor/texture swap, persistent edit generation,
## or a live deformation generation/window change. The existing short sample age
## still prevents old values from lingering.


func _update_source_serial(snapshot: Dictionary) -> void:
	var render_params: Dictionary = snapshot["render"]
	var edit_params: Dictionary = snapshot["edit"]
	var active_params: Dictionary = snapshot["active"]
	var cache_texture: Texture2DArray = snapshot["cache_texture"]
	var edit_texture: Texture2D = snapshot["edit_texture"]
	var cache_rid: RID = cache_texture.get_rid()
	var edit_rid: RID = edit_texture.get_rid()
	var cache_generation: int = int(render_params.get("cache_generation", -1))
	var edit_generation: int = int(edit_params.get("generation", -1))
	var active_ready: bool = bool(active_params.get("ready", false))
	var active_generation: int = int(active_params.get("generation", -1)) if active_ready else -1
	var active_window_generation: int = int(active_params.get("window_generation", -1)) if active_ready else -1

	var changed: bool = cache_generation != _last_cache_generation \
		or edit_generation != _last_edit_generation \
		or active_generation != _last_active_generation \
		or active_window_generation != _last_active_window_generation \
		or cache_rid != _last_cache_server_rid \
		or edit_rid != _last_edit_server_rid
	if not changed:
		return

	_source_serial += 1
	_last_cache_generation = cache_generation
	_last_edit_generation = edit_generation
	_last_active_generation = active_generation
	_last_active_window_generation = active_window_generation
	_last_cache_server_rid = cache_rid
	_last_edit_server_rid = edit_rid
