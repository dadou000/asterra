extends "res://scripts/terrain/gpu_terrain_clipmap_cache.gd"
## Role-aware terrain cache scheduler.
##
## The visible cache keeps the original scheduler. A handoff/prewarm cache uses
## smaller 32x32 jobs and a hard 2048-sample/frame budget. The previous staging
## path could synthesize 12,288 very expensive terrain samples every render frame;
## on a fast GPU that compute queue can still dominate frame time while travelling.

const STAGING_TILE_SIZE: int = 32
const STAGING_SAMPLE_BUDGET: int = 2048
const STAGING_JOB_BUDGET: int = 2

var _staging_budget_mode: bool = false


func set_staging_budget_mode(enabled: bool) -> void:
	_staging_budget_mode = enabled


func staging_budget_mode() -> bool:
	return _staging_budget_mode


func _queue_full_window(level: int, window_min: Vector2i) -> void:
	if not _staging_budget_mode:
		super._queue_full_window(level, window_min)
		return

	# Smaller warm-up jobs let the staging scheduler enforce its real frame budget
	# instead of having a single 64x64/4096-sample job exceed it.
	var local_jobs: Array[Dictionary] = []
	for y: int in range(0, CACHE_RES, STAGING_TILE_SIZE):
		for x: int in range(0, CACHE_RES, STAGING_TILE_SIZE):
			var size := Vector2i(
				mini(STAGING_TILE_SIZE, CACHE_RES - x),
				mini(STAGING_TILE_SIZE, CACHE_RES - y))
			var center := Vector2(
				float(x) + float(size.x) * 0.5,
				float(y) + float(size.y) * 0.5)
			local_jobs.append({
				"level": level,
				"origin": window_min + Vector2i(x, y),
				"size": size,
				"generation": _anchor_generation,
				"priority": center.distance_squared_to(
					Vector2(float(CACHE_RES) * 0.5, float(CACHE_RES) * 0.5)),
			})
	local_jobs.sort_custom(_job_priority_less)
	for job: Dictionary in local_jobs:
		(_warm_jobs[level] as Array).append(job)
	_full_fills += 1


func _dispatch_staggered_batch() -> void:
	if not _staging_budget_mode:
		super._dispatch_staggered_batch()
		return
	if _render_batch_in_flight or not _bindings_ready:
		return

	var payloads: Array[PackedByteArray] = []
	var sizes: Array[Vector2i] = []
	var remaining: int = STAGING_SAMPLE_BUDGET
	var jobs_taken: int = 0
	_last_frame_samples = 0
	_last_frame_jobs = 0

	while remaining > 0 and jobs_taken < STAGING_JOB_BUDGET:
		var from_urgent := true
		var job: Dictionary = _take_next_job(true)
		if job.is_empty():
			from_urgent = false
			job = _take_next_job(false)
		if job.is_empty():
			break
		if int(job.get("generation", -1)) != _anchor_generation:
			continue
		var level: int = int(job.get("level", -1))
		if level < _needed_min or level > _needed_max or not _window_known[level]:
			continue

		var clipped: Dictionary = _clip_job_to_window(job, level)
		if clipped.is_empty():
			continue
		var bounded: Dictionary = _split_job_to_budget(clipped, remaining)
		var take: Dictionary = bounded.get("take", {})
		if take.is_empty():
			break
		var remainders_value: Variant = bounded.get("remainders", [])
		if remainders_value is Array:
			var remainders: Array = remainders_value as Array
			var queue: Array = _urgent_jobs[level] if from_urgent else _warm_jobs[level]
			for index: int in range(remainders.size() - 1, -1, -1):
				queue.push_front(remainders[index])

		var size: Vector2i = take["size"]
		var samples: int = size.x * size.y
		payloads.append(_make_push_constants(take))
		sizes.append(size)
		remaining -= samples
		_last_frame_samples += samples
		jobs_taken += 1

	if payloads.is_empty():
		return
	_frame_cursor += 1
	_last_frame_jobs = payloads.size()
	_render_batch_in_flight = true
	RenderingServer.call_on_render_thread(_render_dispatch_batch.bind(
		payloads, sizes, _rd_pipeline, _rd_uniform_set))


func _split_job_to_budget(job: Dictionary, budget: int) -> Dictionary:
	var size: Vector2i = job["size"]
	var samples: int = size.x * size.y
	if samples <= budget:
		return {"take": job, "remainders": []}
	if budget <= 0:
		return {"take": {}, "remainders": [job]}

	var origin: Vector2i = job["origin"]
	var generation: int = int(job.get("generation", _anchor_generation))
	var priority: float = float(job.get("priority", 0.0))
	var take_size := Vector2i.ZERO
	var remainders: Array[Dictionary] = []

	if size.x <= budget:
		var row_count: int = maxi(1, int(floor(float(budget) / float(size.x))))
		row_count = mini(row_count, size.y)
		take_size = Vector2i(size.x, row_count)
		if row_count < size.y:
			remainders.append({
				"level": int(job["level"]),
				"origin": origin + Vector2i(0, row_count),
				"size": Vector2i(size.x, size.y - row_count),
				"generation": generation,
				"priority": priority,
			})
	else:
		var column_count: int = mini(budget, size.x)
		take_size = Vector2i(column_count, 1)
		if column_count < size.x:
			remainders.append({
				"level": int(job["level"]),
				"origin": origin + Vector2i(column_count, 0),
				"size": Vector2i(size.x - column_count, 1),
				"generation": generation,
				"priority": priority,
			})
		if size.y > 1:
			remainders.append({
				"level": int(job["level"]),
				"origin": origin + Vector2i(0, 1),
				"size": Vector2i(size.x, size.y - 1),
				"generation": generation,
				"priority": priority,
			})

	return {
		"take": {
			"level": int(job["level"]),
			"origin": origin,
			"size": take_size,
			"generation": generation,
			"priority": priority,
		},
		"remainders": remainders,
	}
