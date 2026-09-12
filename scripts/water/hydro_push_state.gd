class_name HydroPushState
extends RefCounted
## Godot 4.7's compute list keeps the last `compute_list_set_push_constant` size in
## `state.set_push_constant_size`, and `compute_list_bind_compute_pipeline` does NOT
## reset it. So dispatching a pipeline that takes NO push constant, later in the
## same compute list than a pipeline that was given one, fails validation:
##   "This compute pipeline requires (0) bytes of push constant data, supplied: (16)"
## and the pass silently never runs.
##
## The sparse-hydrology subcycled solver interleaves 16-byte-push and no-push
## compute passes throughout one `compute_list_begin()..end()`, so every no-push
## dispatch after a pushed one was being dropped -- the solver never advanced a
## tile and a point source produced no water.
##
## Call `HydroPushState.clear(rd, compute)` right after binding a no-push pipeline
## (before its dispatch): pushing a zero-length constant is valid against a
## 0-byte pipeline and resets `set_push_constant_size` back to 0.

static func clear(rd: RenderingDevice, compute_list: int) -> void:
	rd.compute_list_set_push_constant(compute_list, PackedByteArray(), 0)
