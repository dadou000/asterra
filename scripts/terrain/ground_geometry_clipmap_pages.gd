extends "res://scripts/terrain/ground_geometry_clipmap_strips.gd"
## Direct GPU-page clipmap renderer.
##
## This removes the old CPU-local 69x69 height-window worker from ordinary travel.
## GroundHeightStore streams immutable 33x33 world pages; GroundHeightPageAtlas
## uploads each page once and exposes a GPU hash table. The vertex shader converts
## its tangent-plane position back to cube-sphere coordinates and looks up the
## corresponding page directly.
##
## Fine pages may be absent without stalling: the shader walks toward coarser
## resident levels. The entire local clipmap is enabled only when its L6 backing
## coverage is resident, so a teleport/cold cache falls back to the global depth-10
## terrain instead of exposing a hole.

const PAGE_REQUEST_INTERVAL := 0.10
const COVERAGE_CHECK_INTERVAL := 0.05
const REQUEST_GRID_STEPS := 3
const COVERAGE_GRID_STEPS := 5
const FOOTPRINT_MARGIN_CELLS := 2.0

var _page_request_left := 0.0
var _coverage_check_left := 0.0
var _coverage_ready := false
var _coverage_dirty := true
var _bound_atlas: Texture2D
var _bound_table: Texture2D


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/ground_geometry_clipmap_pages.gdshader")
	_ensure_height_ready_marker()
	_bind_page_resources(true)
	_set_active(false)


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_active(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_active(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_active(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_dir: Vector3 = planet_pos.normalized()
	var agl: float = maxf(planet_pos.length() - radius - Planet.macro_height(observer_dir), 0.0)
	if agl > ACTIVE_AGL_M:
		_set_active(false)
		return

	if not _have_frame:
		_reset_frame(observer_dir)

	var observer_surface: Vector3 = observer_dir * radius
	var frame_surface: Vector3 = _frame_dir * radius
	var rel: Vector3 = observer_surface - frame_surface
	var px: float = rel.dot(_frame_right)
	var py: float = rel.dot(_frame_up)

	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_frame(observer_dir)
		px = 0.0
		py = 0.0

	var target := Vector2(
		round(px / _base_spacing) * _base_spacing,
		round(py / _base_spacing) * _base_spacing)
	if target.distance_squared_to(_published_center) > 1e-8:
		_published_center = target
		_requested_center = target
		_coverage_dirty = true

	_bind_page_resources(false)
	_page_request_left -= dt
	if _page_request_left <= 0.0:
		_page_request_left = PAGE_REQUEST_INTERVAL
		_request_visible_pages()

	_coverage_check_left -= dt
	if _coverage_dirty or _coverage_check_left <= 0.0:
		_coverage_check_left = COVERAGE_CHECK_INTERVAL
		_coverage_dirty = false
		_coverage_ready = _check_backing_coverage()

	_sync_page_uniforms()
	_sync_common_uniforms(origin)
	_sync_material_control()
	_set_active(_coverage_ready and GroundHeightPageAtlas.ready_for_shader())


## Re-anchor only the tangent frame. GPU pages are global cube-sphere addresses and
## remain useful across floating-origin/frame changes, so no height data is rebuilt.
func _reset_frame(center: Vector3) -> void:
	_frame_dir = center.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_frame_dir)
	_frame_right = tangent[0]
	_frame_up = tangent[1]
	_base_spacing = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	_published_center = Vector2.ZERO
	_requested_center = Vector2.ZERO
	_have_frame = true
	_frame_epoch += 1
	_page_request_left = 0.0
	_coverage_check_left = 0.0
	_coverage_ready = false
	_coverage_dirty = true
	_set_visible(false)
	_sync_global_cutout(false)


func _on_world_ready(_fields: PlanetFields) -> void:
	_have_frame = false
	_frame_epoch += 1
	_coverage_ready = false
	_coverage_dirty = true
	_page_request_left = 0.0
	_coverage_check_left = 0.0
	_bound_atlas = null
	_bound_table = null
	_ensure_height_ready_marker()
	_set_visible(false)
	_sync_global_cutout(false)


## GroundHeightPageAtlas consumes the page itself. The renderer only needs to
## reconsider whether enough coarse backing data exists to turn the clipmap on.
func _on_height_tile_ready(_level: int, _face: int, _tile_x: int, _tile_y: int) -> void:
	_coverage_dirty = true


func _on_region_changed(_center: Vector3, _radius_m: float) -> void:
	# The page atlas re-uploads only edited resident pages. No local image rebuild.
	_coverage_dirty = true


func _bind_page_resources(force: bool) -> void:
	var atlas: Texture2D = GroundHeightPageAtlas.atlas_texture()
	var table: Texture2D = GroundHeightPageAtlas.page_table_texture()
	if force or atlas != _bound_atlas:
		_bound_atlas = atlas
		_material.set_shader_parameter("u_height_page_atlas", atlas)
	if force or table != _bound_table:
		_bound_table = table
		_material.set_shader_parameter("u_height_page_table", table)


func _sync_page_uniforms() -> void:
	_material.set_shader_parameter("u_page_atlas_ready",
		1.0 if GroundHeightPageAtlas.ready_for_shader() else 0.0)
	_material.set_shader_parameter("u_page_atlas_size", GroundHeightPageAtlas.atlas_size())
	_material.set_shader_parameter("u_page_table_capacity",
		float(GroundHeightPageAtlas.table_capacity()))
	_material.set_shader_parameter("u_page_atlas_cols",
		float(GroundHeightPageAtlas.atlas_columns()))
	_material.set_shader_parameter("u_page_tile_cells", 32.0)
	_material.set_shader_parameter("u_page_tile_verts", 33.0)
	_material.set_shader_parameter("u_finest_cells",
		float(GroundHeightStore.cells_per_face(0)))


func _request_visible_pages() -> void:
	if not _have_frame or Planet.cfg == null:
		return
	for level: int in STORAGE_LEVELS:
		var directions: Array[Vector3] = _footprint_directions(level, REQUEST_GRID_STEPS,
			_full_level_half_extent(level))
		GroundHeightStore.request_samples(directions, level, 0.0)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _check_backing_coverage() -> bool:
	if not _have_frame or not GroundHeightPageAtlas.ready_for_shader():
		return false
	var outer_half: float = (float(GRID_CELLS) * 0.5 + FOOTPRINT_MARGIN_CELLS) \
		* _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var directions: Array[Vector3] = _footprint_directions(STORAGE_LEVELS - 1,
		COVERAGE_GRID_STEPS, outer_half)
	return GroundHeightPageAtlas.touch_samples(directions, STORAGE_LEVELS - 1)


func _full_level_half_extent(level: int) -> float:
	var spacing: float = _base_spacing * pow(2.0, float(level))
	return (float(GRID_CELLS) * 0.5 + FOOTPRINT_MARGIN_CELLS) * spacing


func _footprint_directions(level: int, steps: int, half_extent: float) -> Array[Vector3]:
	var result: Array[Vector3] = []
	if steps < 2 or Planet.cfg == null:
		return result
	var radius: float = Planet.cfg.planet_radius
	var denom: float = float(steps - 1)
	for yi: int in steps:
		var fy: float = -1.0 + 2.0 * float(yi) / denom
		for xi: int in steps:
			var fx: float = -1.0 + 2.0 * float(xi) / denom
			var plane: Vector2 = _published_center + Vector2(fx, fy) * half_extent
			result.append(_direction_for_plane_static(_frame_dir, _frame_right, _frame_up,
				plane, radius))
	return result


func gpu_stream_stats() -> Dictionary:
	var page_stats: Dictionary = GroundHeightPageAtlas.stats()
	return {
		"draw_batches": 2,
		"page_resident": int(page_stats.get("resident_pages", 0)),
		"page_capacity": int(page_stats.get("slot_capacity", 0)),
		"page_uploads": int(page_stats.get("pages_uploaded", 0)),
		"page_reuploads": int(page_stats.get("page_reuploads", 0)),
		"page_evictions": int(page_stats.get("evictions", 0)),
		"page_texels": int(page_stats.get("uploaded_texels", 0)),
		"table_updates": int(page_stats.get("table_updates", 0)),
		"table_rebuilds": int(page_stats.get("table_rebuilds", 0)),
		"table_tombstones": int(page_stats.get("table_tombstones", 0)),
		"table_failures": int(page_stats.get("table_insert_failures", 0)),
		"coverage_ready": _coverage_ready,
	}
