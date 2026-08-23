extends "res://scripts/terrain/ground_geometry_clipmap_pages.gd"
## Coverage-hardened direct GPU-page renderer.
##
## The original page renderer sampled too sparsely when deciding both what to
## request and whether it was safe to cut away the global terrain. That could
## report coverage OK while one of the shader's fallback/normal samples still
## addressed a missing L6 page. This version probes the real footprint more
## densely and only requests the L6 physical area that is actually used as the
## outer-ring backing source.

const STABLE_REQUEST_GRID_STEPS: int = 5
const STABLE_COVERAGE_GRID_STEPS: int = 9
const BACKING_PRIORITY: float = -100.0


func _request_visible_pages() -> void:
	if not _have_frame or Planet.cfg == null:
		return
	for level: int in STORAGE_LEVELS:
		var half_extent: float
		if level == STORAGE_LEVELS - 1:
			# L6 only backs the outer visible L5 ring; requesting an entire L6-sized
			# clip square doubles the physical radius and wastes cache residency.
			half_extent = (float(GRID_CELLS) * 0.5 + FOOTPRINT_MARGIN_CELLS) \
				* _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
		else:
			half_extent = _full_level_half_extent(level)
		var directions: Array[Vector3] = _footprint_directions(level,
			STABLE_REQUEST_GRID_STEPS, half_extent)
		GroundHeightStore.request_samples(directions, level, 0.0)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _check_backing_coverage() -> bool:
	if not _have_frame or not GroundHeightPageAtlas.ready_for_shader():
		return false
	var outer_half: float = (float(GRID_CELLS) * 0.5 + FOOTPRINT_MARGIN_CELLS) \
		* _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var directions: Array[Vector3] = _footprint_directions(STORAGE_LEVELS - 1,
		STABLE_COVERAGE_GRID_STEPS, outer_half)
	# Explicitly request the same points used by the safety test. touch_samples()
	# will also restore a GPU page immediately when it is already resident in RAM.
	GroundHeightStore.request_samples(directions, STORAGE_LEVELS - 1, BACKING_PRIORITY)
	return GroundHeightPageAtlas.touch_samples(directions, STORAGE_LEVELS - 1)
