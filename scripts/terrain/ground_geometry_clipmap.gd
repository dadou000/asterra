extends Node3D
## Fixed-cost ground geometry clipmap.
##
## The global planetary quadtree is intentionally allowed to stop at a coarse
## level. Close terrain is six immutable concentric grids (about 0.75, 1.5, 3,
## 6, 12 and 24 metres between vertices) displaced from a seven-layer height
## pyramid on the GPU. The seventh ~48 m layer is not drawn; it is the exact
## transition target for the outer ring and matches the global depth-10 mesh.
##
## All rings share one centre snapped to the ~48 m backing lattice. Consequently
## every finer level moves by an integer number of its own cells when the centre
## advances. Existing image texels are scrolled; newly exposed samples come from
## GroundHeightStore's persistent cube-sphere tiles. Visual cache misses are
## non-blocking: the clipmap renders the nearest resident parent/macro surface
## immediately, then refreshes as fine tiles arrive in the background.

const TARGET_FINE_DEPTH := 16
# 64 cells keeps the 0.75 m representation immediately around the player while
# keeping GPU cost fixed. Persistent backing tiles now own expensive synthesis;
# this grid only chooses how much of those tiles is visible at each ring level.
const GRID_CELLS := 64
const GRID_VERTS := GRID_CELLS + 1
const RENDER_LEVELS := 6
const STORAGE_LEVELS := RENDER_LEVELS + 1
# The depth-10 planetary mesh is sufficient higher up. Do not start fine height
# streaming while the camera is still several kilometres above ground.
const ACTIVE_AGL_M := 3500.0
const REANCHOR_M := 12000.0
const MATERIAL_RES := 128.0
const MATERIAL_TEXEL_M := Vector3(8.0, 128.0, 2048.0)
const GLOBAL_CUT_FRACTION := 0.90
const GLOBAL_BUILD_CAP := 4
const LOCAL_GLOBAL_BUILD_CAP := 2
# Several cache pages can finish close together. Rebuilding the complete 7-layer
# image for every page would trade one kind of churn for another, so coalesce
# notifications into a small number of progressive refinement updates.
const CACHE_REFRESH_DELAY := 0.20

var _material: ShaderMaterial
var _rings: Array[MeshInstance3D] = []
var _images: Array[Image] = []
var _height_texture: Texture2DArray

var _frame_dir := Vector3(1.0, 0.0, 0.0)
var _frame_right := Vector3(0.0, 0.0, -1.0)
var _frame_up := Vector3(0.0, 1.0, 0.0)
var _have_frame := false
var _frame_epoch := 0
var _base_spacing := 0.75

var _published_center := Vector2.ZERO
var _requested_center := Vector2.ZERO
var _building := false
var _task_id := -1
var _force_full_pending := false
var _active := false
var _last_material_control: Texture2DArray
var _terrain_ref: WeakRef
var _cache_refresh_pending := false
var _cache_refresh_left := 0.0


func _ready() -> void:
	process_priority = 9
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/ground_geometry_clipmap.gdshader")
	_build_ring_nodes()
	_set_visible(false)
	Planet.world_ready.connect(_on_world_ready)
	Deltas.region_changed.connect(_on_region_changed)
	GroundHeightStore.tile_ready.connect(_on_height_tile_ready)


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_active(false)
		return

	# Fine pages arrive asynchronously. A short debounce turns a burst of page
	# completions into one image assembly instead of rebuilding the texture array
	# once per disk read/tile bake.
	if _cache_refresh_pending:
		_cache_refresh_left -= dt
		if _cache_refresh_left <= 0.0 and _have_frame and not _building:
			_cache_refresh_pending = false
			_queue_target(_requested_center, true)

	var camera := get_viewport().get_camera_3d()
	if camera == null:
		_set_active(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos := camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_active(false)
		return

	var radius := Planet.cfg.planet_radius
	var observer_dir := planet_pos.normalized()
	# Activation does not need the full expensive sub-grid synthesis. Macro height
	# can be hundreds of metres off in extreme relief and there is still kilometres
	# of hysteresis before the clipmap ceiling, so use the cheap field here.
	var agl := maxf(planet_pos.length() - radius - Planet.macro_height(observer_dir), 0.0)
	if agl > ACTIVE_AGL_M:
		_set_active(false)
		return

	if not _have_frame:
		_reset_frame(observer_dir)

	# Express the observer on one persistent tangent frame. Keeping that frame
	# fixed for kilometres lets the image scroll rather than forcing a new local
	# projection every few metres. The persistent store itself is face-addressed,
	# so a reanchor still reuses exactly the same canonical height tiles.
	var observer_surface := observer_dir * radius
	var frame_surface := _frame_dir * radius
	var rel := observer_surface - frame_surface
	var px := rel.dot(_frame_right)
	var py := rel.dot(_frame_up)

	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_frame(observer_dir)
		px = 0.0
		py = 0.0

	var backing_spacing := _base_spacing * pow(2.0, float(STORAGE_LEVELS - 1))
	var target := Vector2(
		round(px / backing_spacing) * backing_spacing,
		round(py / backing_spacing) * backing_spacing)
	if target.distance_squared_to(_requested_center) > 1e-6:
		_queue_target(target, false)

	_sync_common_uniforms(origin)
	_sync_material_control()
	_set_active(_height_texture != null)


func _reset_frame(center: Vector3) -> void:
	_frame_dir = center.normalized()
	var tangent := CubeSphere.tangent_basis(_frame_dir)
	_frame_right = tangent[0]
	_frame_up = tangent[1]
	_base_spacing = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	_published_center = Vector2.ZERO
	_requested_center = Vector2.ZERO
	_have_frame = true
	_frame_epoch += 1
	_cache_refresh_pending = false
	# Old image samples belong to a different tangent projection. Keep the global
	# quadtree visible until the replacement image has been assembled from cached
	# canonical tiles rather than stretching the old image across the new frame.
	_images.clear()
	_height_texture = null
	_material.set_shader_parameter("u_height_ready", 0.0)
	_set_visible(false)
	_sync_global_cutout(false)
	_queue_target(Vector2.ZERO, true)


func _queue_target(target: Vector2, force_full: bool) -> void:
	_requested_center = target
	_force_full_pending = _force_full_pending or force_full
	if _building:
		return
	_start_build(_requested_center, _force_full_pending)


func _start_build(target: Vector2, force_full: bool) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return
	_building = true
	_force_full_pending = false
	var epoch := _frame_epoch
	var frame_dir := _frame_dir
	var frame_right := _frame_right
	var frame_up := _frame_up
	var radius := Planet.cfg.planet_radius
	var base_spacing := _base_spacing
	var old_center := _published_center
	var old_images: Array[Image] = []
	if not force_full and _images.size() == STORAGE_LEVELS:
		for image in _images:
			old_images.append(image)

	# One edit snapshot serves every level and every new strip in this update. The
	# immutable base comes from GroundHeightStore; edits remain a separate runtime
	# delta layer and therefore never invalidate or rewrite the disk cache.
	var outer_spacing := base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var outer_half := float(GRID_CELLS) * 0.5 * outer_spacing
	var center_dir := _direction_for_plane_static(frame_dir, frame_right, frame_up,
		target, radius)
	var snap := Deltas.snapshot_for_bounds(center_dir, outer_half * 1.65 / radius)

	var task := func() -> void:
		var built := _build_height_images(frame_dir, frame_right, frame_up,
			old_images, old_center, target, base_spacing, radius, snap)
		call_deferred("_on_build_ready", epoch, target, built)
	# This worker now performs only image assembly, RAM lookups and cheap fallbacks.
	# Missing visual tiles are queued inside GroundHeightStore and synthesized/read
	# independently by its one low-priority worker.
	_task_id = WorkerThreadPool.add_task(task, false, "asterra_ground_clipmap")


func _on_build_ready(epoch: int, built_center: Vector2, built: Array[Image]) -> void:
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
	_building = false

	if epoch == _frame_epoch and built.size() == STORAGE_LEVELS:
		var texture := Texture2DArray.new()
		if texture.create_from_images(built) == OK:
			_images = built
			_height_texture = texture
			_published_center = built_center
			_material.set_shader_parameter("u_height_pyramid", _height_texture)
			_material.set_shader_parameter("u_height_ready", 1.0)
			_set_visible(_active)
			_sync_global_cutout(_active)

	# If the player covered several backing cells while this worker was assembling
	# the image, publish what completed and immediately continue from it. The next
	# job copies old texels and asks the store only for newly exposed strips.
	if _force_full_pending or _requested_center.distance_squared_to(_published_center) > 1e-6:
		_start_build(_requested_center, _force_full_pending)


static func _build_height_images(frame_dir: Vector3, frame_right: Vector3,
		frame_up: Vector3, old_images: Array[Image], old_center: Vector2,
		new_center: Vector2, base_spacing: float, radius: float,
		snap: Dictionary) -> Array[Image]:
	var out: Array[Image] = []
	var half := float(GRID_CELLS) * 0.5

	for level in STORAGE_LEVELS:
		var spacing := base_spacing * pow(2.0, float(level))
		var shift := Vector2i(
			int(round((new_center.x - old_center.x) / spacing)),
			int(round((new_center.y - old_center.y) / spacing)))
		var can_scroll := old_images.size() == STORAGE_LEVELS \
			and absi(shift.x) < GRID_VERTS and absi(shift.y) < GRID_VERTS
		var old: Image = old_images[level] if can_scroll else null
		var image := Image.create(GRID_VERTS, GRID_VERTS, false, Image.FORMAT_RF)

		for y in GRID_VERTS:
			for x in GRID_VERTS:
				var sx := x + shift.x
				var sy := y + shift.y
				if old != null and sx >= 0 and sx < GRID_VERTS and sy >= 0 and sy < GRID_VERTS:
					image.set_pixel(x, y, old.get_pixel(sx, sy))
					continue

				var plane := new_center + Vector2(float(x) - half, float(y) - half) * spacing
				var d := _direction_for_plane_static(frame_dir, frame_right, frame_up,
					plane, radius)
				# This call is intentionally non-blocking. On a cold cache it can return
				# a 48 m/parent/macro value for a fine ring while requesting the missing
				# page in the background. The geometry remains valid and sharpens later.
				var h := GroundHeightStore.sample_height_nonblocking(d, level, snap)
				image.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
		out.append(image)
	return out


static func _direction_for_plane_static(frame_dir: Vector3, frame_right: Vector3,
		frame_up: Vector3, plane: Vector2, radius: float) -> Vector3:
	return (frame_dir + frame_right * (plane.x / radius)
		+ frame_up * (plane.y / radius)).normalized()


func _build_ring_nodes() -> void:
	var full := _build_grid_mesh(false)
	var ring := _build_grid_mesh(true)
	for level in RENDER_LEVELS:
		var mi := MeshInstance3D.new()
		mi.name = "GroundClipmapL%d" % level
		mi.mesh = full if level == 0 else ring
		mi.material_override = _material
		mi.set_instance_shader_parameter("clip_level", float(level))
		mi.set_instance_shader_parameter("clip_outer", 1.0 if level == RENDER_LEVELS - 1 else 0.0)
		# Vertex displacement places the mesh around the floating-origin observer,
		# not inside its tiny undeformed import bounds.
		mi.custom_aabb = AABB(Vector3(-100000.0, -100000.0, -100000.0),
			Vector3(200000.0, 200000.0, 200000.0))
		add_child(mi)
		_rings.append(mi)


static func _build_grid_mesh(with_hole: bool) -> ArrayMesh:
	var vertex_count := GRID_VERTS * GRID_VERTS
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	vertices.resize(vertex_count)
	normals.resize(vertex_count)
	uvs.resize(vertex_count)
	var half := float(GRID_CELLS) * 0.5
	for y in GRID_VERTS:
		for x in GRID_VERTS:
			var i := y * GRID_VERTS + x
			vertices[i] = Vector3(float(x) - half, 0.0, float(y) - half)
			normals[i] = Vector3.UP
			uvs[i] = Vector2(float(x) / float(GRID_CELLS), float(y) / float(GRID_CELLS))

	var indices := PackedInt32Array()
	var inner_half := float(GRID_CELLS) * 0.25
	for y in GRID_CELLS:
		for x in GRID_CELLS:
			if with_hole:
				var cx := float(x) + 0.5 - half
				var cy := float(y) + 0.5 - half
				if absf(cx) < inner_half and absf(cy) < inner_half:
					continue
			var a := y * GRID_VERTS + x
			var b := a + 1
			var c := a + GRID_VERTS
			var d := c + 1
			indices.append_array(PackedInt32Array([a, c, b, b, c, d]))

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _sync_common_uniforms(origin: Vector3) -> void:
	_material.set_shader_parameter("u_origin", origin)
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_frame_dir", _frame_dir)
	_material.set_shader_parameter("u_frame_right", _frame_right)
	_material.set_shader_parameter("u_frame_up", _frame_up)
	_material.set_shader_parameter("u_center_plane", _published_center)
	_material.set_shader_parameter("u_base_spacing", _base_spacing)
	_material.set_shader_parameter("u_grid_cells", float(GRID_CELLS))
	_material.set_shader_parameter("u_storage_levels", float(STORAGE_LEVELS))
	_sync_global_cutout(_active and _height_texture != null)


func _sync_material_control() -> void:
	var source := get_node_or_null("/root/MaterialClipmap")
	if source == null:
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var value: Variant = source.get("_texture")
	if not (value is Texture2DArray):
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var texture: Texture2DArray = value
	if texture == _last_material_control:
		return
	_last_material_control = texture
	_material.set_shader_parameter("u_material_clipmap", texture)
	_material.set_shader_parameter("u_material_clipmap_ready", 1.0)
	_material.set_shader_parameter("u_material_center", source.get("_center"))
	_material.set_shader_parameter("u_material_right", source.get("_right"))
	_material.set_shader_parameter("u_material_up", source.get("_up"))
	_material.set_shader_parameter("u_material_res", MATERIAL_RES)
	_material.set_shader_parameter("u_material_texel_m", MATERIAL_TEXEL_M)


func _sync_global_cutout(enabled: bool) -> void:
	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null:
		terrain = _find_terrain(get_tree().root)
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null:
		return
	var mats := terrain.debug_materials()
	if mats.is_empty():
		return
	var ground: ShaderMaterial = mats[0]
	ground.set_shader_parameter("u_ground_clipmap_cutout", 1.0 if enabled else 0.0)
	if not enabled:
		return
	var outer_spacing := _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var outer_half := float(GRID_CELLS) * 0.5 * outer_spacing
	ground.set_shader_parameter("u_ground_clipmap_frame_dir", _frame_dir)
	ground.set_shader_parameter("u_ground_clipmap_right", _frame_right)
	ground.set_shader_parameter("u_ground_clipmap_up", _frame_up)
	ground.set_shader_parameter("u_ground_clipmap_center_plane", _published_center)
	ground.set_shader_parameter("u_ground_clipmap_cut_half_extent",
		outer_half * GLOBAL_CUT_FRACTION)


func _sync_terrain_worker_budget(local_active: bool) -> void:
	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null:
		terrain = _find_terrain(get_tree().root)
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null or not (terrain is FastPlanetTerrain):
		return
	# Cache misses still need one compute worker to bake an unseen persistent tile.
	# Leave CPU headroom by allowing only two global ChunkBuilder jobs while local
	# ground streaming is active. Warm-cache travel is substantially cheaper, but
	# keeping this cap prevents a sudden uncached region from causing a frame spike.
	var normal_cap := clampi(int(OS.get_processor_count() / 4), 2, GLOBAL_BUILD_CAP)
	terrain.set("_max_concurrent_builds",
		LOCAL_GLOBAL_BUILD_CAP if local_active else normal_cap)


func _find_terrain(node: Node) -> PlanetTerrain:
	if node is PlanetTerrain:
		return node
	for child in node.get_children():
		var found := _find_terrain(child)
		if found != null:
			return found
	return null


func _set_active(value: bool) -> void:
	if _active != value:
		_sync_terrain_worker_budget(value)
	_active = value
	_set_visible(value and _height_texture != null)
	_sync_global_cutout(value and _height_texture != null)


func _set_visible(value: bool) -> void:
	for ring in _rings:
		ring.visible = value


func _on_height_tile_ready(_level: int, _face: int, _tile_x: int, _tile_y: int) -> void:
	if not _have_frame:
		return
	_cache_refresh_pending = true
	# Preserve an already-near deadline instead of postponing forever while a
	# continuous stream of tiles completes.
	if _cache_refresh_left <= 0.0:
		_cache_refresh_left = CACHE_REFRESH_DELAY


func _on_world_ready(_fields: PlanetFields) -> void:
	_have_frame = false
	_frame_epoch += 1
	_cache_refresh_pending = false
	_cache_refresh_left = 0.0
	_images.clear()
	_height_texture = null
	_material.set_shader_parameter("u_height_ready", 0.0)
	_set_visible(false)
	_sync_global_cutout(false)


func _on_region_changed(_center: Vector3, _radius_m: float) -> void:
	if not _have_frame:
		return
	# Edits never invalidate pristine cache files. Reassembling the local image
	# simply reapplies the new Deltas over cached base samples.
	_queue_target(_requested_center, true)


func _exit_tree() -> void:
	_sync_global_cutout(false)
	_sync_terrain_worker_budget(false)
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
